#include "serial_cli.h"
#include "mimi_config.h"
#include "wifi/wifi_manager.h"
#include "telegram/telegram_bot.h"
#include "llm/llm_proxy.h"
#include "memory/memory_store.h"
#include "memory/session_mgr.h"
#include "proxy/http_proxy.h"
#include "tools/tool_web_search.h"
#include "tools/tool_registry.h"
#include "cron/cron_service.h"
#include "heartbeat/heartbeat.h"
#include "federation/peer_manager.h"
#include "discovery/mdns_service.h"

#include <string.h>
#include <stdio.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_console.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "argtable3/argtable3.h"

static const char *TAG = "cli";

/* --- wifi_set command --- */
static struct {
    struct arg_str *ssid;
    struct arg_str *password;
    struct arg_end *end;
} wifi_set_args;

static int cmd_wifi_set(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&wifi_set_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, wifi_set_args.end, argv[0]);
        return 1;
    }
    wifi_manager_set_credentials(wifi_set_args.ssid->sval[0],
                                  wifi_set_args.password->sval[0]);
    printf("WiFi credentials saved. Restart to apply.\n");
    return 0;
}

/* --- wifi_status command --- */
static int cmd_wifi_status(int argc, char **argv)
{
    printf("WiFi connected: %s\n", wifi_manager_is_connected() ? "yes" : "no");
    printf("IP: %s\n", wifi_manager_get_ip());
    return 0;
}

/* --- set_tg_token command --- */
static struct {
    struct arg_str *token;
    struct arg_end *end;
} tg_token_args;

static int cmd_set_tg_token(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&tg_token_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, tg_token_args.end, argv[0]);
        return 1;
    }
    telegram_set_token(tg_token_args.token->sval[0]);
    printf("Telegram bot token saved.\n");
    return 0;
}

/* --- set_api_key command --- */
static struct {
    struct arg_str *key;
    struct arg_end *end;
} api_key_args;

static int cmd_set_api_key(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&api_key_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, api_key_args.end, argv[0]);
        return 1;
    }
    llm_set_api_key(api_key_args.key->sval[0]);
    printf("API key saved.\n");
    return 0;
}

/* --- set_model command --- */
static struct {
    struct arg_str *model;
    struct arg_end *end;
} model_args;

static int cmd_set_model(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&model_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, model_args.end, argv[0]);
        return 1;
    }
    llm_set_model(model_args.model->sval[0]);
    printf("Model set.\n");
    return 0;
}

/* --- set_model_provider command --- */
static struct {
    struct arg_str *provider;
    struct arg_end *end;
} provider_args;

static int cmd_set_model_provider(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&provider_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, provider_args.end, argv[0]);
        return 1;
    }
    llm_set_provider(provider_args.provider->sval[0]);
    printf("Model provider set.\n");
    return 0;
}

/* --- set_ollama_host command --- */
static struct {
    struct arg_str *host;
    struct arg_end *end;
} ollama_host_args;

static int cmd_set_ollama_host(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&ollama_host_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, ollama_host_args.end, argv[0]);
        return 1;
    }
    llm_set_ollama_host(ollama_host_args.host->sval[0]);
    printf("Ollama host set.\n");
    return 0;
}

/* --- set_ollama_port command --- */
static struct {
    struct arg_str *port;
    struct arg_end *end;
} ollama_port_args;

static int cmd_set_ollama_port(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&ollama_port_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, ollama_port_args.end, argv[0]);
        return 1;
    }
    llm_set_ollama_port(ollama_port_args.port->sval[0]);
    printf("Ollama port set.\n");
    return 0;
}

/* --- memory_read command --- */
static int cmd_memory_read(int argc, char **argv)
{
    char *buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        printf("Out of memory.\n");
        return 1;
    }
    if (memory_read_long_term(buf, 4096) == ESP_OK && buf[0]) {
        printf("=== MEMORY.md ===\n%s\n=================\n", buf);
    } else {
        printf("MEMORY.md is empty or not found.\n");
    }
    free(buf);
    return 0;
}

/* --- memory_write command --- */
static struct {
    struct arg_str *content;
    struct arg_end *end;
} memory_write_args;

static int cmd_memory_write(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&memory_write_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, memory_write_args.end, argv[0]);
        return 1;
    }
    memory_write_long_term(memory_write_args.content->sval[0]);
    printf("MEMORY.md updated.\n");
    return 0;
}

/* --- session_list command --- */
static int cmd_session_list(int argc, char **argv)
{
    printf("Sessions:\n");
    session_list();
    return 0;
}

/* --- session_clear command --- */
static struct {
    struct arg_str *chat_id;
    struct arg_end *end;
} session_clear_args;

static int cmd_session_clear(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&session_clear_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, session_clear_args.end, argv[0]);
        return 1;
    }
    if (session_clear(session_clear_args.chat_id->sval[0]) == ESP_OK) {
        printf("Session cleared.\n");
    } else {
        printf("Session not found.\n");
    }
    return 0;
}

/* --- heap_info command --- */
static int cmd_heap_info(int argc, char **argv)
{
    printf("Internal free: %d bytes\n",
           (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    printf("PSRAM free:    %d bytes\n",
           (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    printf("Total free:    %d bytes\n",
           (int)esp_get_free_heap_size());
    return 0;
}

/* --- set_proxy command --- */
static struct {
    struct arg_str *host;
    struct arg_int *port;
    struct arg_end *end;
} proxy_args;

static int cmd_set_proxy(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&proxy_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, proxy_args.end, argv[0]);
        return 1;
    }
    http_proxy_set(proxy_args.host->sval[0], (uint16_t)proxy_args.port->ival[0]);
    printf("Proxy set. Restart to apply.\n");
    return 0;
}

/* --- clear_proxy command --- */
static int cmd_clear_proxy(int argc, char **argv)
{
    http_proxy_clear();
    printf("Proxy cleared. Restart to apply.\n");
    return 0;
}

/* --- set_search_key command --- */
static struct {
    struct arg_str *key;
    struct arg_end *end;
} search_key_args;

static int cmd_set_search_key(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&search_key_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, search_key_args.end, argv[0]);
        return 1;
    }
    tool_web_search_set_key(search_key_args.key->sval[0]);
    printf("Search API key saved.\n");
    return 0;
}

/* --- wifi_scan command --- */
static int cmd_wifi_scan(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    wifi_manager_scan_and_print();
    return 0;
}

/* --- config_show command --- */
static void print_config(const char *label, const char *ns, const char *key,
                         const char *build_val, bool mask)
{
    char nvs_val[128] = {0};
    const char *source = "not set";
    const char *display = "(empty)";

    /* NVS takes highest priority */
    nvs_handle_t nvs;
    if (nvs_open(ns, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(nvs_val);
        if (nvs_get_str(nvs, key, nvs_val, &len) == ESP_OK && nvs_val[0]) {
            source = "NVS";
            display = nvs_val;
        }
        nvs_close(nvs);
    }

    /* Fall back to build-time value */
    if (strcmp(source, "not set") == 0 && build_val[0] != '\0') {
        source = "build";
        display = build_val;
    }

    if (mask && strlen(display) > 6 && strcmp(display, "(empty)") != 0) {
        printf("  %-14s: %.4s****  [%s]\n", label, display, source);
    } else {
        printf("  %-14s: %s  [%s]\n", label, display, source);
    }
}

static int cmd_config_show(int argc, char **argv)
{
    printf("=== Current Configuration ===\n");
    print_config("WiFi SSID",  MIMI_NVS_WIFI,   MIMI_NVS_KEY_SSID,     MIMI_SECRET_WIFI_SSID,  false);
    print_config("WiFi Pass",  MIMI_NVS_WIFI,   MIMI_NVS_KEY_PASS,     MIMI_SECRET_WIFI_PASS,  true);
    print_config("TG Token",   MIMI_NVS_TG,     MIMI_NVS_KEY_TG_TOKEN, MIMI_SECRET_TG_TOKEN,   true);
    print_config("API Key",    MIMI_NVS_LLM,    MIMI_NVS_KEY_API_KEY,  MIMI_SECRET_API_KEY,    true);
    print_config("Model",      MIMI_NVS_LLM,    MIMI_NVS_KEY_MODEL,    MIMI_SECRET_MODEL,      false);
    print_config("Provider",   MIMI_NVS_LLM,    MIMI_NVS_KEY_PROVIDER, MIMI_SECRET_MODEL_PROVIDER, false);
    print_config("Proxy Host", MIMI_NVS_PROXY,  MIMI_NVS_KEY_PROXY_HOST, MIMI_SECRET_PROXY_HOST, false);
    print_config("Proxy Port", MIMI_NVS_PROXY,  MIMI_NVS_KEY_PROXY_PORT, MIMI_SECRET_PROXY_PORT, false);
    print_config("Search Key", MIMI_NVS_SEARCH, MIMI_NVS_KEY_API_KEY,  MIMI_SECRET_SEARCH_KEY, true);
    printf("=============================\n");
    return 0;
}

/* --- config_reset command --- */
static int cmd_config_reset(int argc, char **argv)
{
    const char *namespaces[] = {
        MIMI_NVS_WIFI, MIMI_NVS_TG, MIMI_NVS_LLM, MIMI_NVS_PROXY, MIMI_NVS_SEARCH
    };
    for (int i = 0; i < 5; i++) {
        nvs_handle_t nvs;
        if (nvs_open(namespaces[i], NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_erase_all(nvs);
            nvs_commit(nvs);
            nvs_close(nvs);
        }
    }
    printf("All NVS config cleared. Build-time defaults will be used on restart.\n");
    return 0;
}

/* --- heartbeat_trigger command --- */
static int cmd_heartbeat_trigger(int argc, char **argv)
{
    printf("Checking HEARTBEAT.md...\n");
    if (heartbeat_trigger()) {
        printf("Heartbeat: agent prompted with pending tasks.\n");
    } else {
        printf("Heartbeat: no actionable tasks found.\n");
    }
    return 0;
}

/* --- cron_start command --- */
static int cmd_cron_start(int argc, char **argv)
{
    esp_err_t err = cron_service_start();
    if (err == ESP_OK) {
        printf("Cron service started.\n");
        return 0;
    }

    printf("Failed to start cron service: %s\n", esp_err_to_name(err));
    return 1;
}

static int cmd_tool_exec(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: tool_exec <name> [json]\n");
        return 1;
    }

    const char *tool_name = argv[1];
    const char *input_json = (argc >= 3) ? argv[2] : "{}";

    char *output = heap_caps_calloc(1, 4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!output) {
        printf("Out of memory.\n");
        return 1;
    }

    esp_err_t err = tool_registry_execute(tool_name, input_json, output, 4096);
    printf("tool_exec status: %s\n", esp_err_to_name(err));
    printf("%s\n", output[0] ? output : "(empty)");
    free(output);
    return (err == ESP_OK) ? 0 : 1;
}

/* --- restart command --- */
static int cmd_restart(int argc, char **argv)
{
    printf("Restarting...\n");
    esp_restart();
    return 0;  /* unreachable */
}

/* --- safe_reset command --- */
static int cmd_safe_reset(int argc, char **argv)
{
    nvs_handle_t nvs;
    if (nvs_open("safe_mode", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "boot_cnt", 0);
        nvs_commit(nvs);
        nvs_close(nvs);
        printf("Safe mode boot counter cleared. Restarting normally...\n");
        esp_restart();
    } else {
        printf("Failed to open NVS.\n");
    }
    return 0;
}

/* --- safe_status command --- */
extern bool mimi_is_safe_mode(void);

static int cmd_safe_status(int argc, char **argv)
{
    nvs_handle_t nvs;
    uint8_t boot_cnt = 0;
    if (nvs_open("safe_mode", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, "boot_cnt", &boot_cnt);
        nvs_close(nvs);
    }
    printf("Safe mode: %s\n", mimi_is_safe_mode() ? "ACTIVE" : "inactive");
    printf("Boot counter: %d/3\n", (int)boot_cnt);
    return 0;
}

/* --- comp_status command --- */
#include "component/component_mgr.h"

static int cmd_comp_status(int argc, char **argv)
{
    char *json = comp_status_json();
    if (json) {
        printf("%s\n", json);
        free(json);
    } else {
        printf("Failed to generate status.\n");
    }
    return 0;
}

/* --- config_comp command --- */
static int cmd_config_comp(int argc, char **argv)
{
    if (argc < 3) {
        printf("Usage: config_comp <enable|disable> <name>\n");
        printf("  Example: config_comp disable telegram\n");
        printf("  Changes take effect on next boot.\n");
        return 1;
    }

    bool enable = (strcmp(argv[1], "enable") == 0);
    bool disable = (strcmp(argv[1], "disable") == 0);
    if (!enable && !disable) {
        printf("Unknown action '%s'. Use 'enable' or 'disable'.\n", argv[1]);
        return 1;
    }

    esp_err_t err = comp_set_enabled(argv[2], enable);
    if (err == ESP_ERR_NOT_FOUND) {
        printf("Component '%s' not found.\n", argv[2]);
        return 1;
    } else if (err == ESP_ERR_NOT_SUPPORTED) {
        printf("Cannot disable required component '%s'.\n", argv[2]);
        return 1;
    } else if (err != ESP_OK) {
        printf("Error: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Component '%s' %s. Restart to apply.\n", argv[2], enable ? "enabled" : "disabled");
    return 0;
}

/* --- OTA commands --- */
#include "ota/ota_manager.h"

static int cmd_ota_status(int argc, char **argv)
{
    char *json = ota_status_json();
    if (json) {
        printf("%s\n", json);
        free(json);
    } else {
        printf("Failed to generate OTA status.\n");
    }
    return 0;
}

static struct {
    struct arg_str *url;
    struct arg_end *end;
} ota_check_args;

static int cmd_ota_check(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&ota_check_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, ota_check_args.end, argv[0]);
        return 1;
    }
    esp_err_t err = ota_check_for_update(ota_check_args.url->sval[0]);
    if (err == ESP_OK) {
        printf("Update available: %s\n", ota_get_pending_version());
        printf("URL: %s\n", ota_get_pending_url());
    } else if (err == ESP_ERR_NOT_FOUND) {
        printf("Already up to date (%s)\n", ota_get_current_version());
    } else {
        printf("Check failed: %s\n", esp_err_to_name(err));
    }
    return 0;
}

static int cmd_ota_confirm(int argc, char **argv)
{
    esp_err_t err = ota_confirm_running_firmware();
    if (err == ESP_OK) {
        printf("Firmware confirmed as valid.\n");
    } else {
        printf("Error: %s\n", esp_err_to_name(err));
    }
    return 0;
}

static int cmd_ota_rollback(int argc, char **argv)
{
    printf("Rolling back to previous firmware...\n");
    esp_err_t err = ota_rollback();
    printf("Rollback failed: %s\n", esp_err_to_name(err));
    return 1;
}

/* --- scan_audio command --- */
#include "../audio/audio.h"

static int cmd_scan_audio(int argc, char **argv)
{
    int pins[] = {4, 5, 6, 8, 9, 10, 11, 12, 13, 14, 17, 18, 19, 20, 21, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48};
    printf("Scanning audio pins... Listen for 400Hz tone. (Total: %d pins)\n", (int)(sizeof(pins)/sizeof(pins[0])));
    
    for (int i = 0; i < sizeof(pins)/sizeof(pins[0]); i++) {
        printf("Testing GPIO %d...\n", pins[i]);
        audio_test_pin(pins[i]);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    printf("Scan complete. If you heard a beep, note the GPIO number and tell the developer.\n");
    return 0;
}

/* --- skill_rollback commands --- */
#include "skills/skill_rollback.h"

static struct {
    struct arg_str *name;
    struct arg_end *end;
} skill_rb_args;

static int cmd_skill_rollback(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&skill_rb_args);
    if (nerrors) {
        arg_print_errors(stderr, skill_rb_args.end, argv[0]);
        return 1;
    }
    const char *name = skill_rb_args.name->sval[0];
    if (!skill_rollback_exists(name)) {
        printf("No rollback backup for '%s'\n", name);
        return 1;
    }
    esp_err_t err = skill_rollback_restore(name);
    printf("%s\n", err == ESP_OK ? "Skill restored." : "Restore failed.");
    return err == ESP_OK ? 0 : 1;
}

static int cmd_skill_rollback_list(int argc, char **argv)
{
    char *json = skill_rollback_list_json();
    if (json) {
        printf("%s\n", json);
        free(json);
    }
    return 0;
}

/* --- peer_list command --- */
static int cmd_peer_list(int argc, char **argv)
{
    int count = 0;
    const peer_t *peers = peer_manager_get_list(&count);
    printf("Active Peers:\n");
    printf("%-20s %-16s %-6s %s\n", "Hostname", "IP", "Port", "Last Seen");
    printf("----------------------------------------------------------\n");
    bool found = false;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t now = tv.tv_sec;

    for (int i = 0; i < count; i++) {
        if (peers[i].active) {
            found = true;
            printf("%-20s %-16s %-6d %lds ago\n", 
                   peers[i].hostname, peers[i].ip_addr, peers[i].port, (long)(now - peers[i].last_seen));
        }
    }
    if (!found) printf("(none)\n");
    return 0;
}

/* --- peer_scan command --- */
static int cmd_peer_scan(int argc, char **argv)
{
    printf("Scanning for peers...\n");
    mdns_service_query_peers();
    return 0;
}

esp_err_t serial_cli_init(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "mimi> ";
    repl_config.max_cmdline_length = 256;

    /* USB Serial JTAG */
    esp_console_dev_usb_serial_jtag_config_t hw_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl));

    /* Register commands */
    esp_console_register_help_command();

    /* wifi_set */
    wifi_set_args.ssid = arg_str1(NULL, NULL, "<ssid>", "WiFi SSID");
    wifi_set_args.password = arg_str1(NULL, NULL, "<password>", "WiFi password");
    wifi_set_args.end = arg_end(2);
    esp_console_cmd_t wifi_set_cmd = {
        .command = "wifi_set",
        .help = "Set WiFi SSID and password",
        .func = &cmd_wifi_set,
        .argtable = &wifi_set_args,
    };
    esp_console_cmd_register(&wifi_set_cmd);

    /* wifi_status */
    esp_console_cmd_t wifi_status_cmd = {
        .command = "wifi_status",
        .help = "Show WiFi connection status",
        .func = &cmd_wifi_status,
    };
    esp_console_cmd_register(&wifi_status_cmd);

    /* wifi_scan */
    esp_console_cmd_t wifi_scan_cmd = {
        .command = "wifi_scan",
        .help = "Scan and list nearby WiFi APs",
        .func = &cmd_wifi_scan,
    };
    esp_console_cmd_register(&wifi_scan_cmd);

    /* set_tg_token */
    tg_token_args.token = arg_str1(NULL, NULL, "<token>", "Telegram bot token");
    tg_token_args.end = arg_end(1);
    esp_console_cmd_t tg_token_cmd = {
        .command = "set_tg_token",
        .help = "Set Telegram bot token",
        .func = &cmd_set_tg_token,
        .argtable = &tg_token_args,
    };
    esp_console_cmd_register(&tg_token_cmd);

    /* set_api_key */
    api_key_args.key = arg_str1(NULL, NULL, "<key>", "LLM API key");
    api_key_args.end = arg_end(1);
    esp_console_cmd_t api_key_cmd = {
        .command = "set_api_key",
        .help = "Set LLM API key",
        .func = &cmd_set_api_key,
        .argtable = &api_key_args,
    };
    esp_console_cmd_register(&api_key_cmd);

    /* set_model */
    model_args.model = arg_str1(NULL, NULL, "<model>", "Model identifier");
    model_args.end = arg_end(1);
    esp_console_cmd_t model_cmd = {
        .command = "set_model",
        .help = "Set LLM model (default: " MIMI_LLM_DEFAULT_MODEL ")",
        .func = &cmd_set_model,
        .argtable = &model_args,
    };
    esp_console_cmd_register(&model_cmd);

    /* set_model_provider */
    provider_args.provider = arg_str1(NULL, NULL, "<provider>", "Model provider (anthropic|openai|minimax|minimax_coding|ollama)");
    provider_args.end = arg_end(1);
    esp_console_cmd_t provider_cmd = {
        .command = "set_model_provider",
        .help = "Set LLM model provider (default: " MIMI_LLM_PROVIDER_DEFAULT ")",
        .func = &cmd_set_model_provider,
        .argtable = &provider_args,
    };
    esp_console_cmd_register(&provider_cmd);

    /* set_ollama_host */
    ollama_host_args.host = arg_str1(NULL, NULL, "<host>", "Ollama server IP or hostname");
    ollama_host_args.end = arg_end(1);
    esp_console_cmd_t ollama_host_cmd = {
        .command = "set_ollama_host",
        .help = "Set Ollama server host (e.g. 192.168.1.100)",
        .func = &cmd_set_ollama_host,
        .argtable = &ollama_host_args,
    };
    esp_console_cmd_register(&ollama_host_cmd);

    /* set_ollama_port */
    ollama_port_args.port = arg_str1(NULL, NULL, "<port>", "Ollama server port (default: 11434)");
    ollama_port_args.end = arg_end(1);
    esp_console_cmd_t ollama_port_cmd = {
        .command = "set_ollama_port",
        .help = "Set Ollama server port",
        .func = &cmd_set_ollama_port,
        .argtable = &ollama_port_args,
    };
    esp_console_cmd_register(&ollama_port_cmd);

    /* memory_read */
    esp_console_cmd_t mem_read_cmd = {
        .command = "memory_read",
        .help = "Read MEMORY.md",
        .func = &cmd_memory_read,
    };
    esp_console_cmd_register(&mem_read_cmd);

    /* memory_write */
    memory_write_args.content = arg_str1(NULL, NULL, "<content>", "Content to write");
    memory_write_args.end = arg_end(1);
    esp_console_cmd_t mem_write_cmd = {
        .command = "memory_write",
        .help = "Write to MEMORY.md",
        .func = &cmd_memory_write,
        .argtable = &memory_write_args,
    };
    esp_console_cmd_register(&mem_write_cmd);

    /* session_list */
    esp_console_cmd_t sess_list_cmd = {
        .command = "session_list",
        .help = "List all sessions",
        .func = &cmd_session_list,
    };
    esp_console_cmd_register(&sess_list_cmd);

    /* session_clear */
    session_clear_args.chat_id = arg_str1(NULL, NULL, "<chat_id>", "Chat ID to clear");
    session_clear_args.end = arg_end(1);
    esp_console_cmd_t sess_clear_cmd = {
        .command = "session_clear",
        .help = "Clear a session",
        .func = &cmd_session_clear,
        .argtable = &session_clear_args,
    };
    esp_console_cmd_register(&sess_clear_cmd);

    /* heap_info */
    esp_console_cmd_t heap_cmd = {
        .command = "heap_info",
        .help = "Show heap memory usage",
        .func = &cmd_heap_info,
    };
    esp_console_cmd_register(&heap_cmd);

    /* set_search_key */
    search_key_args.key = arg_str1(NULL, NULL, "<key>", "Brave Search API key");
    search_key_args.end = arg_end(1);
    esp_console_cmd_t search_key_cmd = {
        .command = "set_search_key",
        .help = "Set Brave Search API key for web_search tool",
        .func = &cmd_set_search_key,
        .argtable = &search_key_args,
    };
    esp_console_cmd_register(&search_key_cmd);

    /* set_proxy */
    proxy_args.host = arg_str1(NULL, NULL, "<host>", "Proxy host/IP");
    proxy_args.port = arg_int1(NULL, NULL, "<port>", "Proxy port");
    proxy_args.end = arg_end(2);
    esp_console_cmd_t proxy_cmd = {
        .command = "set_proxy",
        .help = "Set HTTP proxy (e.g. set_proxy 192.168.1.1 7897)",
        .func = &cmd_set_proxy,
        .argtable = &proxy_args,
    };
    esp_console_cmd_register(&proxy_cmd);

    /* clear_proxy */
    esp_console_cmd_t clear_proxy_cmd = {
        .command = "clear_proxy",
        .help = "Remove proxy configuration",
        .func = &cmd_clear_proxy,
    };
    esp_console_cmd_register(&clear_proxy_cmd);

    /* config_show */
    esp_console_cmd_t config_show_cmd = {
        .command = "config_show",
        .help = "Show current configuration (build-time + NVS)",
        .func = &cmd_config_show,
    };
    esp_console_cmd_register(&config_show_cmd);

    /* config_reset */
    esp_console_cmd_t config_reset_cmd = {
        .command = "config_reset",
        .help = "Clear all NVS overrides, revert to build-time defaults",
        .func = &cmd_config_reset,
    };
    esp_console_cmd_register(&config_reset_cmd);

    /* heartbeat_trigger */
    esp_console_cmd_t heartbeat_cmd = {
        .command = "heartbeat_trigger",
        .help = "Manually trigger a heartbeat check",
        .func = &cmd_heartbeat_trigger,
    };
    esp_console_cmd_register(&heartbeat_cmd);

    /* cron_start */
    esp_console_cmd_t cron_start_cmd = {
        .command = "cron_start",
        .help = "Start cron scheduler timer now",
        .func = &cmd_cron_start,
    };
    esp_console_cmd_register(&cron_start_cmd);

    /* tool_exec */
    esp_console_cmd_t tool_exec_cmd = {
        .command = "tool_exec",
        .help = "Execute a registered tool: tool_exec <name> '{...json...}'",
        .func = &cmd_tool_exec,
    };
    esp_console_cmd_register(&tool_exec_cmd);

    /* scan_audio */
    esp_console_cmd_t scan_audio_cmd = {
        .command = "scan_audio",
        .help = "Scan GPIOs to find speaker pin (plays tone)",
        .func = &cmd_scan_audio,
    };
    esp_console_cmd_register(&scan_audio_cmd);

    /* restart */
    esp_console_cmd_t restart_cmd = {
        .command = "restart",
        .help = "Restart the device",
        .func = &cmd_restart,
    };
    esp_console_cmd_register(&restart_cmd);

    /* safe_reset */
    esp_console_cmd_t safe_reset_cmd = {
        .command = "safe_reset",
        .help = "Clear safe mode boot counter and restart normally",
        .func = &cmd_safe_reset,
    };
    esp_console_cmd_register(&safe_reset_cmd);

    /* safe_status */
    esp_console_cmd_t safe_status_cmd = {
        .command = "safe_status",
        .help = "Show safe mode status and boot counter",
        .func = &cmd_safe_status,
    };
    esp_console_cmd_register(&safe_status_cmd);

    /* comp_status */
    esp_console_cmd_t comp_status_cmd = {
        .command = "comp_status",
        .help = "Show all component states (JSON)",
        .func = &cmd_comp_status,
    };
    esp_console_cmd_register(&comp_status_cmd);

    /* config_comp */
    esp_console_cmd_t config_comp_cmd = {
        .command = "config_comp",
        .help = "Enable/disable component: config_comp <enable|disable> <name>",
        .func = &cmd_config_comp,
    };
    esp_console_cmd_register(&config_comp_cmd);

    /* ota_status */
    esp_console_cmd_t ota_status_cmd = {
        .command = "ota_status",
        .help = "Show OTA/firmware status (JSON)",
        .func = &cmd_ota_status,
    };
    esp_console_cmd_register(&ota_status_cmd);

    /* ota_check */
    ota_check_args.url = arg_str1(NULL, NULL, "<url>", "Version check URL");
    ota_check_args.end = arg_end(1);
    esp_console_cmd_t ota_check_cmd = {
        .command = "ota_check",
        .help = "Check for firmware update from version URL",
        .func = &cmd_ota_check,
        .argtable = &ota_check_args,
    };
    esp_console_cmd_register(&ota_check_cmd);

    /* ota_confirm */
    esp_console_cmd_t ota_confirm_cmd = {
        .command = "ota_confirm",
        .help = "Confirm running firmware as valid (prevents rollback)",
        .func = &cmd_ota_confirm,
    };
    esp_console_cmd_register(&ota_confirm_cmd);

    /* ota_rollback */
    esp_console_cmd_t ota_rollback_cmd = {
        .command = "ota_rollback",
        .help = "Rollback to previous firmware and reboot",
        .func = &cmd_ota_rollback,
    };
    esp_console_cmd_register(&ota_rollback_cmd);

    /* skill_rollback */
    skill_rb_args.name = arg_str1(NULL, NULL, "<name>", "Skill name to rollback");
    skill_rb_args.end = arg_end(1);
    esp_console_cmd_t skill_rb_cmd = {
        .command = "skill_rollback",
        .help = "Restore a skill to its previous version",
        .func = &cmd_skill_rollback,
        .argtable = &skill_rb_args,
    };
    esp_console_cmd_register(&skill_rb_cmd);

    /* skill_rollback_list */
    esp_console_cmd_t skill_rb_list_cmd = {
        .command = "skill_rollback_list",
        .help = "List skills with rollback backups available",
        .func = &cmd_skill_rollback_list,
    };
    esp_console_cmd_register(&skill_rb_list_cmd);

    /* peer_list */
    esp_console_cmd_t peer_list_cmd = {
        .command = "peer_list",
        .help = "List discovered peers",
        .func = &cmd_peer_list,
    };
    esp_console_cmd_register(&peer_list_cmd);

    /* peer_scan */
    esp_console_cmd_t peer_scan_cmd = {
        .command = "peer_scan",
        .help = "Trigger mDNS peer scan",
        .func = &cmd_peer_scan,
    };
    esp_console_cmd_register(&peer_scan_cmd);

    /* Start REPL */
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "Serial CLI started");

    return ESP_OK;
}
