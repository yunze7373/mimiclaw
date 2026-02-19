#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"
#include "cJSON.h"

#include "mimi_config.h"
#include "bus/message_bus.h"
#include "wifi/wifi_manager.h"
#include "llm/llm_proxy.h"
#include "agent/agent_loop.h"
#include "memory/memory_store.h"
#include "memory/session_mgr.h"
#include "cli/serial_cli.h"
#include "tools/tool_registry.h"
#include "buttons/button_driver.h"
#include "rgb/rgb.h"

#if CONFIG_MIMI_ENABLE_TELEGRAM
#include "telegram/telegram_bot.h"
#endif
#if CONFIG_MIMI_ENABLE_WEBSOCKET
#include "gateway/ws_server.h"
#endif
#if CONFIG_MIMI_ENABLE_WEB_UI
#include "web_ui/web_ui.h"
#endif
#if CONFIG_MIMI_ENABLE_HTTP_PROXY
#include "proxy/http_proxy.h"
#endif
#if MIMI_HAS_LCD
#include "display/display.h"
#include "ui/config_screen.h"
#include "imu/imu_manager.h"
#endif
#if CONFIG_MIMI_ENABLE_OLED
#include "display/ssd1306.h"
#endif
#if CONFIG_MIMI_ENABLE_CRON
#include "cron/cron_service.h"
#endif
#if CONFIG_MIMI_ENABLE_HEARTBEAT
#include "heartbeat/heartbeat.h"
#endif
#if CONFIG_MIMI_ENABLE_SKILLS
#include "skills/skill_engine.h"
#endif
#if CONFIG_MIMI_ENABLE_OTA
#include "ota/ota_manager.h"
#endif
#if CONFIG_MIMI_ENABLE_MDNS
#include "discovery/mdns_service.h"
#endif
#include "component/component_mgr.h"

static const char *TAG = "mimi";

#define SAFEMODE_NVS_NS       "safe_mode"
#define SAFEMODE_NVS_KEY      "boot_cnt"
#define SAFEMODE_THRESHOLD    3        /* 3 consecutive rapid reboots → safe mode */
#define SAFEMODE_STABLE_MS    60000    /* 60s uptime = stable boot */
static bool s_safe_mode = false;
static esp_timer_handle_t s_stability_timer = NULL;

/* PSRAM malloc wrapper for cJSON hooks */
static void *psram_malloc(size_t sz) {
    void *p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        /* Fallback to internal if PSRAM fails (should be rare) */
        p = malloc(sz);
    }
    return p;
}

static esp_err_t init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

static esp_err_t init_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = MIMI_SPIFFS_BASE,
        .partition_label = NULL,
        .max_files = 10,
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    esp_spiffs_info(NULL, &total, &used);
    ESP_LOGI(TAG, "SPIFFS: total=%d, used=%d", (int)total, (int)used);

    return ESP_OK;
}

/* ── Safe Mode: crash loop detection ────────────────────────────── */
static void stability_timer_cb(void *arg)
{
    (void)arg;
    /* We survived 60s → reset boot counter */
    nvs_handle_t nvs;
    if (nvs_open(SAFEMODE_NVS_NS, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, SAFEMODE_NVS_KEY, 0);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "Stable boot confirmed — boot counter reset");
    }

#if CONFIG_MIMI_ENABLE_OTA
    /* Also confirm OTA firmware if pending verification */
    ota_confirm_running_firmware();
#endif
}

static bool check_safe_mode(void)
{
    nvs_handle_t nvs;
    uint8_t boot_cnt = 0;

    if (nvs_open(SAFEMODE_NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) {
        return false;  /* Can't determine — assume normal */
    }

    nvs_get_u8(nvs, SAFEMODE_NVS_KEY, &boot_cnt);  /* 0 if not found */
    boot_cnt++;
    ESP_LOGI(TAG, "Boot count: %d (threshold: %d)", (int)boot_cnt, SAFEMODE_THRESHOLD);

    nvs_set_u8(nvs, SAFEMODE_NVS_KEY, boot_cnt);
    nvs_commit(nvs);
    nvs_close(nvs);

    if (boot_cnt >= SAFEMODE_THRESHOLD) {
        ESP_LOGW(TAG, "⚠ SAFE MODE ACTIVE — Skills disabled due to %d consecutive rapid reboots", (int)boot_cnt);
        ESP_LOGW(TAG, "  Use 'safe_reset' CLI command to clear and reboot normally");
        return true;
    }

    /* Start stability timer — if we survive 60s, clear counter */
    esp_timer_create_args_t args = {
        .callback = stability_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "safe_stab",
    };
    if (esp_timer_create(&args, &s_stability_timer) == ESP_OK) {
        esp_timer_start_once(s_stability_timer, (uint64_t)SAFEMODE_STABLE_MS * 1000ULL);
    }

    return false;
}

bool mimi_is_safe_mode(void)
{
    return s_safe_mode;
}

/* Outbound dispatch task: reads from outbound queue and routes to channels */
static void outbound_dispatch_task(void *arg)
{
    ESP_LOGI(TAG, "Outbound dispatch started");

    while (1) {
        mimi_msg_t msg;
        if (message_bus_pop_outbound(&msg, UINT32_MAX) != ESP_OK) continue;

        ESP_LOGI(TAG, "Dispatching response to %s:%s", msg.channel, msg.chat_id);

#if CONFIG_MIMI_ENABLE_TELEGRAM
        if (strcmp(msg.channel, MIMI_CHAN_TELEGRAM) == 0) {
            telegram_send_message(msg.chat_id, msg.content);
        } else
#endif
#if CONFIG_MIMI_ENABLE_WEBSOCKET
        if (strcmp(msg.channel, MIMI_CHAN_WEBSOCKET) == 0) {
            ws_server_send(msg.chat_id, msg.content);
        } else
#endif
        if (strcmp(msg.channel, MIMI_CHAN_SYSTEM) == 0) {
            ESP_LOGI(TAG, "System message [%s]: %.128s", msg.chat_id, msg.content);
        } else {
            ESP_LOGW(TAG, "Unknown channel: %s", msg.channel);
        }

        free(msg.content);
    }
}

void app_main(void)
{
    /* ── Redirect cJSON to PSRAM ──────────────────────────────── */
    /* This is critical: cJSON uses malloc() internally for ALL JSON
     * operations. Without this, every JSON parse/build/serialize eats
     * internal SRAM, leaving nothing for TLS/AES DMA buffers. */
    cJSON_Hooks hooks = {
        .malloc_fn = psram_malloc,
        .free_fn = free
    };
    cJSON_InitHooks(&hooks);

    /* Silence noisy components */
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  MimiClaw - ESP32-S3 AI Agent");
    ESP_LOGI(TAG, "========================================");

    /* Print memory info */
    ESP_LOGI(TAG, "Internal free: %d bytes",
             (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "PSRAM free:    %d bytes",
             (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* Display + input (pre-component init — these are HW-level) */
#if MIMI_HAS_LCD
    ESP_ERROR_CHECK(display_init());
    display_show_banner();
#endif

    button_Init();
#if MIMI_HAS_LCD
    config_screen_init();
    imu_manager_init();
    imu_manager_set_shake_callback(config_screen_toggle);
#endif

    /* ── Phase 1: Core infrastructure (pre-component manager) ─── */
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(init_spiffs());

    /* ── Phase 2: Register components ──────────────────────────── */

    /* L0: Base — no deps */
    comp_register("msg_bus",     COMP_LAYER_BASE, true,  false,
                  message_bus_init, NULL, NULL, NULL);
    comp_register("memory",     COMP_LAYER_BASE, true,  false,
                  memory_store_init, NULL, NULL, NULL);
    comp_register("session",    COMP_LAYER_BASE, true,  false,
                  session_mgr_init, NULL, NULL, NULL);
    comp_register("wifi",       COMP_LAYER_BASE, true,  false,
                  wifi_manager_init, NULL, NULL, NULL);
#if CONFIG_MIMI_ENABLE_HTTP_PROXY
    comp_register("http_proxy", COMP_LAYER_BASE, false, false,
                  http_proxy_init, NULL, NULL, NULL);
#endif

    /* L1: Core — depends on base */
    const char *core_deps[] = {"msg_bus", "memory", "session", NULL};
    comp_register("llm",         COMP_LAYER_CORE, true,  false,
                  llm_proxy_init, NULL, NULL, core_deps);
    comp_register("tool_reg",    COMP_LAYER_CORE, true,  false,
                  tool_registry_init, NULL, NULL, core_deps);

    /* Skill engine depends on tool_reg + needs safe mode check */
#if CONFIG_MIMI_ENABLE_SKILLS
    const char *skill_deps[] = {"tool_reg", NULL};
    if (!s_safe_mode) {
        /* check_safe_mode must be called before registration */
        s_safe_mode = check_safe_mode();
    }
    if (!s_safe_mode) {
        comp_register("skill_engine", COMP_LAYER_CORE, false, false,
                      skill_engine_init, NULL, NULL, skill_deps);
    } else {
        ESP_LOGW(TAG, "Skipping skill_engine registration — SAFE MODE");
    }
#endif

    const char *agent_deps[] = {"llm", "tool_reg", "msg_bus", NULL};
#if CONFIG_MIMI_ENABLE_CRON
    comp_register("cron",     COMP_LAYER_CORE, false, false,
                  cron_service_init, cron_service_start, NULL, core_deps);
#endif
#if CONFIG_MIMI_ENABLE_HEARTBEAT
    comp_register("heartbeat", COMP_LAYER_CORE, false, false,
                  heartbeat_init, heartbeat_start, NULL, core_deps);
#endif
    comp_register("agent",    COMP_LAYER_CORE, true,  true,
                  agent_loop_init, agent_loop_start, NULL, agent_deps);

    /* L2: Entry — depends on core, many need WiFi */
    comp_register("cli",       COMP_LAYER_ENTRY, false, false,
                  serial_cli_init, NULL, NULL, NULL);

#if CONFIG_MIMI_ENABLE_TELEGRAM
    const char *tg_deps[] = {"agent", "msg_bus", NULL};
    comp_register("telegram",  COMP_LAYER_ENTRY, false, true,
                  telegram_bot_init, telegram_bot_start, NULL, tg_deps);
#endif

#if CONFIG_MIMI_ENABLE_WEBSOCKET
    const char *ws_deps[] = {"agent", NULL};
    comp_register("websocket", COMP_LAYER_ENTRY, false, true,
                  NULL, ws_server_start, NULL, ws_deps);
#if CONFIG_MIMI_ENABLE_WEB_UI
    comp_register("web_ui",    COMP_LAYER_ENTRY, false, true,
                  NULL, web_ui_init, NULL, ws_deps);
#endif
#endif

    /* L3: Extensions — optional WiFi-dependent services */
#if CONFIG_MIMI_ENABLE_MDNS
    const char *mdns_deps[] = {"wifi", NULL};
    comp_register("mdns", COMP_LAYER_EXT, false, true,
                  mdns_service_init, mdns_service_start, NULL, mdns_deps);
#endif

    /* ── Phase 3: Load config + Initialize all ──────────────────── */
    comp_load_config();  /* Disable components per /spiffs/config/components.json */
    ESP_ERROR_CHECK(comp_init_all());

    /* Initialize RGB LED (lazy init in tool, but try here for early boot feedback) */
    rgb_init();

#if CONFIG_MIMI_ENABLE_OLED
    /* Initialize SSD1306 OLED if connected */
    if (ssd1306_is_connected()) {
        ssd1306_init();
        ssd1306_clear();
        ssd1306_draw_string(0, 0, "MimiClaw Ready!");
        ssd1306_update();
    }
#endif

    /* ── Phase 4: WiFi connect + start WiFi-dependents ────────── */
    esp_err_t wifi_err = wifi_manager_start();
    if (wifi_err == ESP_OK) {
        ESP_LOGI(TAG, "Scanning nearby APs on boot...");
        wifi_manager_scan_and_print();
        ESP_LOGI(TAG, "Waiting for WiFi connection...");
        if (wifi_manager_wait_connected(30000) == ESP_OK) {
            ESP_LOGI(TAG, "WiFi connected: %s", wifi_manager_get_ip());

            /* Start all WiFi-dependent components */
            ESP_LOGI(TAG, "Memory before services: %d KB free",
                     heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);

            comp_start_wifi_dependents();

            /* Outbound dispatch task */
            xTaskCreatePinnedToCore(
                outbound_dispatch_task, "outbound",
                MIMI_OUTBOUND_STACK, NULL,
                MIMI_OUTBOUND_PRIO, NULL, MIMI_OUTBOUND_CORE);

            ESP_LOGI(TAG, "Memory after all services: %d KB free",
                     heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
            ESP_LOGI(TAG, "All services started!");
        } else {
            ESP_LOGW(TAG, "WiFi connection timeout. Check MIMI_SECRET_WIFI_SSID in mimi_secrets.h");
        }
    } else {
        ESP_LOGW(TAG, "No WiFi credentials. Set MIMI_SECRET_WIFI_SSID in mimi_secrets.h");
    }

    ESP_LOGI(TAG, "MimiClaw ready. Type 'help' for CLI commands.");
}

