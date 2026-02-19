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
#include "cJSON.h"

#include "mimi_config.h"
#include "bus/message_bus.h"
#include "wifi/wifi_manager.h"
#include "telegram/telegram_bot.h"
#include "llm/llm_proxy.h"
#include "agent/agent_loop.h"
#include "memory/memory_store.h"
#include "memory/session_mgr.h"
#include "gateway/ws_server.h"
#include "cli/serial_cli.h"
#include "proxy/http_proxy.h"
#include "tools/tool_registry.h"
#include "display/display.h"
#include "display/ssd1306.h"
#include "buttons/button_driver.h"
#include "ui/config_screen.h"
#include "imu/imu_manager.h"
#include "rgb/rgb.h"
#include "cron/cron_service.h"
#include "heartbeat/heartbeat.h"
#include "web_ui/web_ui.h"
#include "skills/skill_engine.h"

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

        if (strcmp(msg.channel, MIMI_CHAN_TELEGRAM) == 0) {
            telegram_send_message(msg.chat_id, msg.content);
        } else if (strcmp(msg.channel, MIMI_CHAN_WEBSOCKET) == 0) {
            ws_server_send(msg.chat_id, msg.content);
        } else if (strcmp(msg.channel, MIMI_CHAN_SYSTEM) == 0) {
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

    /* Display + input */
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

    /* Phase 1: Core infrastructure */
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(init_spiffs());

    /* Initialize subsystems */
    ESP_ERROR_CHECK(message_bus_init());
    ESP_ERROR_CHECK(memory_store_init());
    ESP_ERROR_CHECK(session_mgr_init());
    ESP_ERROR_CHECK(wifi_manager_init());
    ESP_ERROR_CHECK(http_proxy_init());
    ESP_ERROR_CHECK(telegram_bot_init());
    ESP_ERROR_CHECK(llm_proxy_init());
    ESP_ERROR_CHECK(tool_registry_init());

    /* Initialize RGB LED (lazy init in tool, but try here for early boot feedback) */
    rgb_init();

    /* Initialize SSD1306 OLED if connected */
    if (ssd1306_is_connected()) {
        ssd1306_init();
        ssd1306_clear();
        ssd1306_draw_string(0, 0, "MimiClaw Ready!");
        ssd1306_update();
    }

    /* Load Lua hardware skills (must be after SPIFFS + tool_registry) */
    s_safe_mode = check_safe_mode();
    if (!s_safe_mode) {
        ESP_ERROR_CHECK(skill_engine_init());
    } else {
        ESP_LOGW(TAG, "Skipping skill_engine_init() — SAFE MODE");
    }

    ESP_ERROR_CHECK(cron_service_init());
    ESP_ERROR_CHECK(heartbeat_init());
    ESP_ERROR_CHECK(agent_loop_init());

    /* Start Serial CLI first (works without WiFi) */
    ESP_ERROR_CHECK(serial_cli_init());

    /* Start WiFi */
    esp_err_t wifi_err = wifi_manager_start();
    if (wifi_err == ESP_OK) {
        ESP_LOGI(TAG, "Scanning nearby APs on boot...");
        wifi_manager_scan_and_print();
        ESP_LOGI(TAG, "Waiting for WiFi connection...");
        if (wifi_manager_wait_connected(30000) == ESP_OK) {
            ESP_LOGI(TAG, "WiFi connected: %s", wifi_manager_get_ip());

            /* Start network-dependent services */
            ESP_LOGI(TAG, "Memory before services: %d KB free",
                     heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
            ESP_ERROR_CHECK(telegram_bot_start());
            ESP_LOGI(TAG, "Memory after telegram: %d KB free",
                     heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
            ESP_ERROR_CHECK(agent_loop_start());
            ESP_LOGI(TAG, "Memory after agent: %d KB free",
                     heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
            cron_service_start();
            heartbeat_start();
            ESP_ERROR_CHECK(ws_server_start());
            ESP_LOGI(TAG, "Memory after ws: %d KB free",
                     heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
            ESP_ERROR_CHECK(web_ui_init());
            ESP_LOGI(TAG, "Memory after web_ui: %d KB free",
                     heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);

            /* Outbound dispatch task */
            xTaskCreatePinnedToCore(
                outbound_dispatch_task, "outbound",
                MIMI_OUTBOUND_STACK, NULL,
                MIMI_OUTBOUND_PRIO, NULL, MIMI_OUTBOUND_CORE);

            ESP_LOGI(TAG, "All services started!");
        } else {
            ESP_LOGW(TAG, "WiFi connection timeout. Check MIMI_SECRET_WIFI_SSID in mimi_secrets.h");
        }
    } else {
        ESP_LOGW(TAG, "No WiFi credentials. Set MIMI_SECRET_WIFI_SSID in mimi_secrets.h");
    }

    ESP_LOGI(TAG, "MimiClaw ready. Type 'help' for CLI commands.");
}
