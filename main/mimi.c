#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
    display_show_banner();
#endif

    button_Init();
#if MIMI_HAS_LCD
    config_screen_init();
    imu_manager_init();
    imu_manager_set_shake_callback(config_screen_toggle);
#endif

    /* 笏笏 Phase 1: Core infrastructure (pre-component manager) 笏笏笏 */
    ESP_ERROR_CHECK(init_nvs());
    
    // Initialize System Manager (Safe Mode / Boot Loop Check)
    system_manager_init();
    
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(init_spiffs());

    /* 笏笏 Phase 2: Register components 笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏 */

    /* L0: Base 窶・no deps */
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

    /* L1: Core 窶・depends on base */
    const char *core_deps[] = {"msg_bus", "memory", "session", NULL};
    comp_register("llm",         COMP_LAYER_CORE, true,  false,
                  llm_proxy_init, NULL, NULL, core_deps);
    comp_register("tool_reg",    COMP_LAYER_CORE, true,  false,
                  tool_registry_init, NULL, NULL, core_deps);

    /* Skill engine depends on tool_reg + needs safe mode check */
#if CONFIG_MIMI_ENABLE_SKILLS
    const char *skill_deps[] = {"tool_reg", NULL};
    if (!system_is_safe_mode()) {
        comp_register("skill_engine", COMP_LAYER_CORE, false, false,
                      skill_engine_init, NULL, NULL, skill_deps);
    } else {
        ESP_LOGW(TAG, "Skipping skill_engine registration 窶・SAFE MODE");
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

    /* L2: Entry 窶・depends on core, many need WiFi */
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

    /* L3: Extensions 窶・optional WiFi-dependent services */
#if CONFIG_MIMI_ENABLE_MDNS
    const char *mdns_deps[] = {"wifi", NULL};
    comp_register("mdns", COMP_LAYER_EXTENSION, false, true,
                  mdns_service_init, mdns_service_start, NULL, mdns_deps);
#endif

#if CONFIG_MIMI_ENABLE_ZIGBEE
    // MQTT Manager (Phase 15)
    const char *mqtt_deps[] = {"wifi", "tool_reg", NULL};
    comp_register("mqtt_manager", COMP_LAYER_EXTENSION, false, true,
                  mqtt_manager_init, mqtt_manager_start, NULL, mqtt_deps);
#endif

#if CONFIG_MIMI_ENABLE_MCP
    const char *mcp_deps[] = {"wifi", "tool_reg", NULL};
    comp_register("mcp_manager", COMP_LAYER_EXTENSION, false, true,
                  mcp_manager_init, mcp_manager_start, NULL, mcp_deps);
#endif

#if CONFIG_MIMI_ENABLE_HA
#if CONFIG_MIMI_ENABLE_WEB_UI && CONFIG_MIMI_ENABLE_WEBSOCKET
    const char *ha_deps[] = {"wifi", "web_ui", NULL};
    comp_register("ha_integration", COMP_LAYER_EXTENSION, false, true,
                  ha_integration_init, ha_integration_start, NULL, ha_deps);
#else
    ESP_LOGW(TAG, "Skipping ha_integration: requires Web UI HTTP server");
#endif
#endif

#if CONFIG_MIMI_ENABLE_ZIGBEE
    comp_register("zigbee_gateway", COMP_LAYER_EXTENSION, false, false,
                  zigbee_gateway_init, zigbee_gateway_start, NULL, NULL);
#endif

    const char *api_deps[] = {"wifi", "tool_reg", NULL};
    comp_register("api_manager", COMP_LAYER_EXTENSION, true, false,
                  api_manager_init, NULL, NULL, api_deps);

    /* 笏笏 Phase 3: Load config + Initialize all 笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏 */
    comp_load_config();  /* Disable components per /spiffs/config/components.json */
    ESP_ERROR_CHECK(comp_init_all());

    /* Initialize RGB LED (lazy init in tool, but try here for early boot feedback) */
    rgb_init();

#if CONFIG_MIMI_ENABLE_OLED
    /* Initialize SSD1306 OLED if connected */
    if (ssd1306_is_connected()) {
        ssd1306_init();
        ssd1306_clear();
        ssd1306_draw_string(0, 0, "Esp32Claw Ready!");
        ssd1306_update();
    }
#endif

    /* 笏笏 Phase 4: WiFi connect + start WiFi-dependents 笏笏笏笏笏笏笏笏笏笏 */
    esp_err_t wifi_err = wifi_manager_start();
    if (wifi_err == ESP_OK) {
        ESP_LOGI(TAG, "Scanning nearby APs on boot...");
        wifi_manager_scan_and_print();
        ESP_LOGI(TAG, "Waiting for WiFi connection...");
        if (wifi_manager_wait_connected(30000) == ESP_OK) {
            ESP_LOGI(TAG, "WiFi connected: %s", wifi_manager_get_ip());

            /* Start all WiFi-dependent components */
            ESP_LOGI(TAG, "Memory before services: %d KB free",
                     (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);

            comp_start_wifi_dependents();

            /* Outbound dispatch task */
            xTaskCreatePinnedToCore(
                outbound_dispatch_task, "outbound",
                MIMI_OUTBOUND_STACK, NULL,
                MIMI_OUTBOUND_PRIO, NULL, MIMI_OUTBOUND_CORE);

            ESP_LOGI(TAG, "Memory after all services: %d KB free",
                     (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
            ESP_LOGI(TAG, "All services started!");
        } else {
            ESP_LOGW(TAG, "WiFi connection timeout. Check MIMI_SECRET_WIFI_SSID in mimi_secrets.h");
        }
    } else {
        ESP_LOGW(TAG, "No WiFi credentials. Set MIMI_SECRET_WIFI_SSID in mimi_secrets.h");
    }

    ESP_LOGI(TAG, "Esp32Claw ready. Type 'help' for CLI commands.");
}
