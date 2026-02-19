#include "tool_registry.h"
#include "tools/tool_web_search.h"
#include "tools/tool_get_time.h"
#include "tools/tool_files.h"
#include "tools/tool_cron.h"
#include "tools/tool_hardware.h"
#include "tools/tool_network.h"
#include "tools/tool_skill_create.h"
#include "tools/tool_skill_manage.h"
#include "llm/llm_proxy.h"

#include <string.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "tools";

/* Inline tool: set_streaming */
static esp_err_t tool_set_streaming_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    bool enable = true;
    if (root) {
        cJSON *val = cJSON_GetObjectItem(root, "enable");
        if (val && cJSON_IsBool(val)) enable = cJSON_IsTrue(val);
        cJSON_Delete(root);
    }
    llm_set_streaming(enable);
    snprintf(output, output_size, "Streaming %s.", enable ? "enabled" : "disabled");
    return ESP_OK;
}

#define MAX_TOOLS 32

static mimi_tool_t s_tools[MAX_TOOLS];
static int s_tool_count = 0;
static char *s_tools_json = NULL;  /* cached JSON array string */

void tool_registry_register(const mimi_tool_t *tool)
{
    for (int i = 0; i < s_tool_count; i++) {
        if (strcmp(s_tools[i].name, tool->name) == 0) {
            ESP_LOGW(TAG, "Tool already exists, skip: %s", tool->name);
            return;
        }
    }
    if (s_tool_count >= MAX_TOOLS) {
        ESP_LOGE(TAG, "Tool registry full");
        return;
    }
    s_tools[s_tool_count++] = *tool;
    ESP_LOGI(TAG, "Registered tool: %s", tool->name);
}

void tool_registry_unregister(const char *name)
{
    if (!name || !name[0]) return;

    for (int i = 0; i < s_tool_count; i++) {
        if (strcmp(s_tools[i].name, name) == 0) {
            for (int j = i; j < s_tool_count - 1; j++) {
                s_tools[j] = s_tools[j + 1];
            }
            s_tool_count--;
            ESP_LOGI(TAG, "Unregistered tool: %s", name);
            return;
        }
    }
}

void tool_registry_rebuild_json(void)
{
    cJSON *arr = cJSON_CreateArray();

    for (int i = 0; i < s_tool_count; i++) {
        cJSON *tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", s_tools[i].name);
        cJSON_AddStringToObject(tool, "description", s_tools[i].description);

        cJSON *schema = cJSON_Parse(s_tools[i].input_schema_json);
        if (schema) {
            cJSON_AddItemToObject(tool, "input_schema", schema);
        }

        cJSON_AddItemToArray(arr, tool);
    }

    free(s_tools_json);
    s_tools_json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    ESP_LOGI(TAG, "Tools JSON built (%d tools)", s_tool_count);
}

esp_err_t tool_registry_init(void)
{
    s_tool_count = 0;

    /* Register web_search */
    tool_web_search_init();

    mimi_tool_t ws = {
        .name = "web_search",
        .description = "Search the web for current information. Use this when you need up-to-date facts, news, weather, or anything beyond your training data.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"The search query\"}},"
            "\"required\":[\"query\"]}",
        .execute = tool_web_search_execute,
    };
    tool_registry_register(&ws);

    /* Register get_current_time */
    tool_time_init();

    mimi_tool_t gt = {
        .name = "get_current_time",
        .description = "Get the current date and time. Also sets the system clock. Call this when you need to know what time or date it is.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{},"
            "\"required\":[]}",
        .execute = tool_get_time_execute,
    };
    tool_registry_register(&gt);

    /* Register set_timezone */
    mimi_tool_t stz = {
        .name = "set_timezone",
        .description = "Set the system timezone. Use this when the user asks to change the timezone or location.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"timezone\":{\"type\":\"string\",\"description\":\"Timezone string (e.g. 'CST-8', 'EST5EDT', 'UTC')\"}},"
            "\"required\":[\"timezone\"]}",
        .execute = tool_set_timezone_execute,
    };
    tool_registry_register(&stz);

    /* Register set_streaming */
    mimi_tool_t stm = {
        .name = "set_streaming",
        .description = "Enable or disable streaming mode. When streaming is enabled, responses appear token by token in real-time. When disabled, the full response is sent at once (faster overall but no live typing effect).",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"enable\":{\"type\":\"boolean\",\"description\":\"true to enable streaming, false to disable\"}},"
            "\"required\":[\"enable\"]}",
        .execute = tool_set_streaming_execute,
    };
    tool_registry_register(&stm);

    /* Register read_file */
    mimi_tool_t rf = {
        .name = "read_file",
        .description = "Read a file from SPIFFS storage. Path must start with /spiffs/.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path starting with /spiffs/\"}},"
            "\"required\":[\"path\"]}",
        .execute = tool_read_file_execute,
    };
    tool_registry_register(&rf);

    /* Register write_file */
    mimi_tool_t wf = {
        .name = "write_file",
        .description = "Write or overwrite a file on SPIFFS storage. Path must start with /spiffs/.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path starting with /spiffs/\"},"
            "\"content\":{\"type\":\"string\",\"description\":\"File content to write\"}},"
            "\"required\":[\"path\",\"content\"]}",
        .execute = tool_write_file_execute,
    };
    tool_registry_register(&wf);

    /* Register edit_file */
    mimi_tool_t ef = {
        .name = "edit_file",
        .description = "Find and replace text in a file on SPIFFS. Replaces first occurrence of old_string with new_string.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path starting with /spiffs/\"},"
            "\"old_string\":{\"type\":\"string\",\"description\":\"Text to find\"},"
            "\"new_string\":{\"type\":\"string\",\"description\":\"Replacement text\"}},"
            "\"required\":[\"path\",\"old_string\",\"new_string\"]}",
        .execute = tool_edit_file_execute,
    };
    tool_registry_register(&ef);

    /* Register list_dir */
    mimi_tool_t ld = {
        .name = "list_dir",
        .description = "List files on SPIFFS storage, optionally filtered by path prefix.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"prefix\":{\"type\":\"string\",\"description\":\"Optional path prefix filter, e.g. /spiffs/memory/\"}},"
            "\"required\":[]}",
        .execute = tool_list_dir_execute,
    };
    tool_registry_register(&ld);

    /* Register cron_add */
    mimi_tool_t ca = {
        .name = "cron_add",
        .description = "Schedule a recurring or one-shot task. The message will trigger an agent turn when the job fires.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"name\":{\"type\":\"string\",\"description\":\"Short name for the job\"},"
            "\"schedule_type\":{\"type\":\"string\",\"description\":\"'every' for recurring interval or 'at' for one-shot at a unix timestamp\"},"
            "\"interval_s\":{\"type\":\"integer\",\"description\":\"Interval in seconds (required for 'every')\"},"
            "\"at_epoch\":{\"type\":\"integer\",\"description\":\"Unix timestamp to fire at (required for 'at')\"},"
            "\"message\":{\"type\":\"string\",\"description\":\"Message to inject when the job fires, triggering an agent turn\"},"
            "\"channel\":{\"type\":\"string\",\"description\":\"Optional reply channel (e.g. 'telegram'). Defaults to 'system'\"},"
            "\"chat_id\":{\"type\":\"string\",\"description\":\"Optional reply chat_id. Defaults to 'cron'\"}"
            "},"
            "\"required\":[\"name\",\"schedule_type\",\"message\"]}",
        .execute = tool_cron_add_execute,
    };
    tool_registry_register(&ca);

    /* Register cron_list */
    mimi_tool_t cl = {
        .name = "cron_list",
        .description = "List all active cron jobs.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
        .execute = tool_cron_list_execute,
    };
    tool_registry_register(&cl);

     /* Register cron_remove */
    mimi_tool_t cr = {
        .name = "cron_remove",
        .description = "Remove a cron job by ID.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"id\":{\"type\":\"string\",\"description\":\"Job ID\"}},"
            "\"required\":[\"id\"]}",
        .execute = tool_cron_remove_execute,
    };
    tool_registry_register(&cr);

    /* --- Power / Hardware --- */
    /* Register system_status */
    mimi_tool_t ss = {
        .name = "system_status",
        .description = "Get current system status (CPU, Memory, Temp, Uptime).",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
        .execute = tool_system_status,
    };
    tool_registry_register(&ss);

    /* Register gpio_control */
    mimi_tool_t gc = {
        .name = "gpio_control",
        .description = "Control a GPIO pin (High/Low). Returns error if pin is restricted.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"pin\":{\"type\":\"integer\"},\"state\":{\"type\":\"boolean\"}},"
            "\"required\":[\"pin\",\"state\"]}",
        .execute = tool_gpio_control,
    };
    tool_registry_register(&gc);

    /* Register i2c_scan */
    mimi_tool_t is = {
        .name = "i2c_scan",
        .description = "Scan for connected I2C devices.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
        .execute = tool_i2c_scan,
    };
    tool_registry_register(&is);

    /* --- Phase 1: New Hardware Tools --- */

    /* Register adc_read */
    mimi_tool_t ar = {
        .name = "adc_read",
        .description = "Read an ADC channel (0-9 on ADC1). Returns raw value and voltage in mV. Use to measure analog sensor values.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"channel\":{\"type\":\"integer\",\"description\":\"ADC channel number (0-9)\"}},"
            "\"required\":[\"channel\"]}",
        .execute = tool_adc_read,
    };
    tool_registry_register(&ar);

    /* Register pwm_control */
    mimi_tool_t pwm = {
        .name = "pwm_control",
        .description = "Control PWM output on a GPIO pin using LEDC. Set frequency and duty cycle, or stop PWM. Up to 4 simultaneous channels.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"pin\":{\"type\":\"integer\",\"description\":\"GPIO pin number\"},"
            "\"freq_hz\":{\"type\":\"integer\",\"description\":\"PWM frequency in Hz (default 5000)\"},"
            "\"duty_percent\":{\"type\":\"number\",\"description\":\"Duty cycle 0-100% (default 50)\"},"
            "\"stop\":{\"type\":\"boolean\",\"description\":\"Set true to stop PWM on this pin\"}"
            "},"
            "\"required\":[\"pin\"]}",
        .execute = tool_pwm_control,
    };
    tool_registry_register(&pwm);

    /* Register rgb_control */
    mimi_tool_t rgb = {
        .name = "rgb_control",
        .description = "Set the on-board WS2812 RGB LED color. Each color channel is 0-255. Use {r:0,g:0,b:0} to turn off.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"r\":{\"type\":\"integer\",\"description\":\"Red (0-255)\"},"
            "\"g\":{\"type\":\"integer\",\"description\":\"Green (0-255)\"},"
            "\"b\":{\"type\":\"integer\",\"description\":\"Blue (0-255)\"}"
            "},"
            "\"required\":[\"r\",\"g\",\"b\"]}",
        .execute = tool_rgb_control,
    };
    tool_registry_register(&rgb);

    /* --- Phase 2: Network Tools --- */
    tool_network_init();

    /* Register wifi_scan */
    mimi_tool_t wscan = {
        .name = "wifi_scan",
        .description = "Scan for nearby WiFi access points. Returns SSID, RSSI, channel, and auth type for each AP found.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
        .execute = tool_wifi_scan,
    };
    tool_registry_register(&wscan);

    /* Register wifi_status */
    mimi_tool_t wstat = {
        .name = "wifi_status",
        .description = "Get current WiFi connection status including SSID, IP address, RSSI, MAC, and gateway.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
        .execute = tool_wifi_status,
    };
    tool_registry_register(&wstat);

#ifdef CONFIG_BT_ENABLED
    /* Register ble_scan */
    mimi_tool_t bscan = {
        .name = "ble_scan",
        .description = "Scan for nearby Bluetooth Low Energy (BLE) devices. Returns device name, MAC address, and RSSI. Scan takes ~5 seconds.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
        .execute = tool_ble_scan,
    };
    tool_registry_register(&bscan);
#endif

    /* --- Phase 3: System Tools --- */

    /* Register uart_send */
    mimi_tool_t us = {
        .name = "uart_send",
        .description = "Send data string via UART port. Default port is UART1. Use for serial communication with external devices.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"data\":{\"type\":\"string\",\"description\":\"Data string to send\"},"
            "\"port\":{\"type\":\"integer\",\"description\":\"UART port number (default 1)\"}"
            "},"
            "\"required\":[\"data\"]}",
        .execute = tool_uart_send,
    };
    tool_registry_register(&us);

    /* Phase 4: I2S Tools */
    mimi_tool_t ir = {
        .name = "i2s_read",
        .description = "Read raw PCM audio from Microphone (I2S0). Input: {bytes: int}. Output: Base64 encoded PCM data. Default 4096 bytes.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"bytes\":{\"type\":\"integer\"}},\"required\":[]}",
        .execute = tool_i2s_read,
    };
    tool_registry_register(&ir);

    mimi_tool_t iw = {
        .name = "i2s_write",
        .description = "Write raw PCM audio to Amplifier (I2S1). Input: {data_base64: string}. Output: 'OK'.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"data_base64\":{\"type\":\"string\"}},\"required\":[\"data_base64\"]}",
        .execute = tool_i2s_write,
    };
    tool_registry_register(&iw);

    /* Register system_restart (re-registering properly) */
    mimi_tool_t sr = {
        .name = "system_restart",
        .description = "Restart the ESP32 system. Use only when necessary (e.g. after config changes). The device will reboot after a 500ms delay.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
        .execute = tool_system_restart,
    };
    tool_registry_register(&sr);

    /* --- Phase 6: Agent Skill Creation --- */
    mimi_tool_t sc = {
        .name = "skill_create",
        .description = "Create a new hardware or software skill from Lua code. The skill is saved to SPIFFS and hot-loaded into the engine. Use skill_list_templates first to see available templates for reference.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"name\":{\"type\":\"string\",\"description\":\"Skill name (lowercase, alphanumeric, underscores)\"},"
            "\"description\":{\"type\":\"string\",\"description\":\"Human-readable description\"},"
            "\"category\":{\"type\":\"string\",\"description\":\"hardware or software\"},"
            "\"type\":{\"type\":\"string\",\"description\":\"sensor, actuator, communication, utility, or service\"},"
            "\"bus\":{\"type\":\"string\",\"description\":\"i2c, gpio, spi, pwm, uart, rmt, or none\"},"
            "\"code\":{\"type\":\"string\",\"description\":\"Complete Lua skill code with SKILL and TOOLS tables\"}"
            "},"
            "\"required\":[\"name\",\"code\"]}",
        .execute = tool_skill_create_execute,
    };
    tool_registry_register(&sc);

    mimi_tool_t slt = {
        .name = "skill_list_templates",
        .description = "List available skill templates for creating new skills. Returns template names, descriptions, and categories. Use the template_path with read_file to see the full template code.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
        .execute = tool_skill_list_templates_execute,
    };
    tool_registry_register(&slt);

    mimi_tool_t sgt = {
        .name = "skill_get_template",
        .description = "Get the Lua source code for a skill template. Use this to start writing a new skill.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"Template name (from skill_list_templates)\"}},"
            "\"required\":[\"name\"]}",
        .execute = tool_skill_get_template_execute,
    };
    tool_registry_register(&sgt);

    mimi_tool_t sm = {
        .name = "skill_manage",
        .description = "Manage installed skills: list, delete, or reload. Use delete to uninstall a skill by name. Use reload after manual file changes.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"action\":{\"type\":\"string\",\"description\":\"list, delete, or reload\"},"
            "\"name\":{\"type\":\"string\",\"description\":\"Skill name (required for delete)\"}"
            "},"
            "\"required\":[\"action\"]}",
        .execute = tool_skill_manage_execute,
    };
    tool_registry_register(&sm);

    tool_registry_rebuild_json();

    ESP_LOGI(TAG, "Tool registry initialized");
    return ESP_OK;
}

const char *tool_registry_get_tools_json(void)
{
    return s_tools_json;
}

esp_err_t tool_registry_execute(const char *name, const char *input_json,
                                char *output, size_t output_size)
{
    for (int i = 0; i < s_tool_count; i++) {
        if (strcmp(s_tools[i].name, name) == 0) {
            ESP_LOGI(TAG, "Executing tool: %s", name);
            return s_tools[i].execute(input_json, output, output_size);
        }
    }

    ESP_LOGW(TAG, "Unknown tool: %s", name);
    snprintf(output, output_size, "Error: unknown tool '%s'", name);
    return ESP_ERR_NOT_FOUND;
}
