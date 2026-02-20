#include "tool_registry.h"
#include "tools/tool_web_search.h"
#include "tools/tool_get_time.h"
#include "tools/tool_files.h"
#include "tools/tool_cron.h"
#include "tools/tool_hardware.h"
#include "tools/tool_network.h"
#include "tools/tool_skill_create.h"
#include "tools/tool_skill_manage.h"
#include "tools/tool_mcp.h"
#include "llm/llm_proxy.h"

#include <string.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "tools";

/* ── Built-in Tools Storage ────────────────────────────────────────── */

#define MAX_TOOLS 48
static mimi_tool_t s_tools[MAX_TOOLS];
static int s_tool_count = 0;

/* ── Tool Providers ────────────────────────────────────────────────── */

#define MAX_PROVIDERS 8
static tool_provider_t s_providers[MAX_PROVIDERS];
static int s_provider_count = 0;

static char *s_cached_json = NULL;

/* ── Inline tool: set_streaming ────────────────────────────────────── */
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

/* ── Built-in Provider (Legacy Wrapper) ────────────────────────────── */

static char *builtin_get_tools_json(void)
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
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return json;
}

static esp_err_t builtin_execute_tool(const char *tool_name, const char *input_json, char *output, size_t output_size)
{
    for (int i = 0; i < s_tool_count; i++) {
        if (strcmp(s_tools[i].name, tool_name) == 0) {
            return s_tools[i].execute(input_json, output, output_size);
        }
    }
    return ESP_ERR_NOT_FOUND;
}

static const tool_provider_t s_builtin_provider = {
    .name = "builtin",
    .get_tools_json = builtin_get_tools_json,
    .execute_tool = builtin_execute_tool
};

/* ── Registry API ──────────────────────────────────────────────────── */

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

esp_err_t tool_registry_register_provider(const tool_provider_t *provider)
{
    if (s_provider_count >= MAX_PROVIDERS) return ESP_ERR_NO_MEM;
    s_providers[s_provider_count++] = *provider;
    ESP_LOGI(TAG, "Registered provider: %s", provider->name);
    return ESP_OK;
}

void tool_registry_rebuild_json(void)
{
    // In provider model, we rebuild on get or invalidate cache here.
    // For now, simple clear cache.
    if (s_cached_json) {
        free(s_cached_json);
        s_cached_json = NULL;
    }
}

const char *tool_registry_get_tools_json(void)
{
    if (s_cached_json) return s_cached_json;

    cJSON *root = cJSON_CreateObject();
    cJSON *all_tools = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "type", cJSON_CreateString("function")); // Optional, for some LLM formats
    // Actually, usually we return just the array or objects. The original code returned printed array.
    // Let's stick to returning array of tools as root, unless LLM proxy expects otherwise.
    // Original implementation: returned cJSON_PrintUnformatted(arr)
    
    // So let's create a big array.
    cJSON_Delete(root);
    all_tools = cJSON_CreateArray();

    for (int i = 0; i < s_provider_count; i++) {
        char *p_json = s_providers[i].get_tools_json();
        if (p_json) {
            cJSON *p_arr = cJSON_Parse(p_json);
            if (p_arr && cJSON_IsArray(p_arr)) {
                cJSON *item = NULL;
                cJSON_ArrayForEach(item, p_arr) {
                    cJSON *clone = cJSON_Duplicate(item, true);
                    cJSON_AddItemToArray(all_tools, clone);
                }
            }
            if (p_arr) cJSON_Delete(p_arr);
            free(p_json);
        }
    }

    s_cached_json = cJSON_PrintUnformatted(all_tools);
    cJSON_Delete(all_tools);
    return s_cached_json;
}

esp_err_t tool_registry_execute(const char *name, const char *input_json,
                                char *output, size_t output_size)
{
    for (int i = 0; i < s_provider_count; i++) {
        esp_err_t ret = s_providers[i].execute_tool(name, input_json, output, output_size);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Executed tool '%s' via provider '%s'", name, s_providers[i].name);
            return ESP_OK;
        } else if (ret != ESP_ERR_NOT_FOUND) {
            // Found but failed
            return ret;
        }
    }

    ESP_LOGW(TAG, "Unknown tool: %s", name);
    snprintf(output, output_size, "Error: unknown tool '%s'", name);
    return ESP_ERR_NOT_FOUND;
}

/* ── Init ──────────────────────────────────────────────────────────── */

esp_err_t tool_registry_init(void)
{
    s_tool_count = 0;
    s_provider_count = 0;

    /* Register Built-in Provider first */
    tool_registry_register_provider(&s_builtin_provider);

    /* Register web_search */
    tool_web_search_init();
    mimi_tool_t ws = {
        .name = "web_search",
        .description = "Search the web for current information. Use this when you need up-to-date facts, news, weather, or anything beyond your training data.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"The search query\"}},\"required\":[\"query\"]}",
        .execute = tool_web_search_execute,
    };
    tool_registry_register(&ws);

    /* Register get_current_time */
    tool_time_init();
    mimi_tool_t gt = {
        .name = "get_current_time",
        .description = "Get the current date and time. Also sets the system clock. Call this when you need to know what time or date it is.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
        .execute = tool_get_time_execute,
    };
    tool_registry_register(&gt);

    /* Register set_timezone */
    mimi_tool_t stz = {
        .name = "set_timezone",
        .description = "Set the system timezone.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"timezone\":{\"type\":\"string\",\"description\":\"Timezone string (e.g. 'CST-8', 'EST5EDT', 'UTC')\"}},\"required\":[\"timezone\"]}",
        .execute = tool_set_timezone_execute,
    };
    tool_registry_register(&stz);

    /* Register set_streaming */
    mimi_tool_t stm = {
        .name = "set_streaming",
        .description = "Enable or disable streaming mode.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"enable\":{\"type\":\"boolean\"}},\"required\":[\"enable\"]}",
        .execute = tool_set_streaming_execute,
    };
    tool_registry_register(&stm);

    /* Register read_file */
    mimi_tool_t rf = {
        .name = "read_file",
        .description = "Read a file from SPIFFS storage.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}",
        .execute = tool_read_file_execute,
    };
    tool_registry_register(&rf);

    /* Register write_file */
    mimi_tool_t wf = {
        .name = "write_file",
        .description = "Write a file to SPIFFS storage.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}",
        .execute = tool_write_file_execute,
    };
    tool_registry_register(&wf);

    /* Register edit_file */
    mimi_tool_t ef = {
        .name = "edit_file",
        .description = "Find and replace text in a file.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"old_string\":{\"type\":\"string\"},\"new_string\":{\"type\":\"string\"}},\"required\":[\"path\",\"old_string\",\"new_string\"]}",
        .execute = tool_edit_file_execute,
    };
    tool_registry_register(&ef);

    /* Register list_dir */
    mimi_tool_t ld = {
        .name = "list_dir",
        .description = "List files on SPIFFS storage.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"prefix\":{\"type\":\"string\"}},\"required\":[]}",
        .execute = tool_list_dir_execute,
    };
    tool_registry_register(&ld);

    /* Register cron tools */
    mimi_tool_t ca = {
        .name = "cron_add",
        .description = "Schedule a recurring or one-shot task.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"schedule_type\":{\"type\":\"string\"},\"interval_s\":{\"type\":\"integer\"},\"at_epoch\":{\"type\":\"integer\"},\"message\":{\"type\":\"string\"},\"channel\":{\"type\":\"string\"},\"chat_id\":{\"type\":\"string\"}},\"required\":[\"name\",\"schedule_type\",\"message\"]}",
        .execute = tool_cron_add_execute,
    };
    tool_registry_register(&ca);

    mimi_tool_t cl = {
        .name = "cron_list",
        .description = "List all active cron jobs.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
        .execute = tool_cron_list_execute,
    };
    tool_registry_register(&cl);

     mimi_tool_t cr = {
        .name = "cron_remove",
        .description = "Remove a cron job by ID.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"}},\"required\":[\"id\"]}",
        .execute = tool_cron_remove_execute,
    };
    tool_registry_register(&cr);

    /* Register system_status */
    mimi_tool_t ss = {
        .name = "system_status",
        .description = "Get current system status.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
        .execute = tool_system_status,
    };
    tool_registry_register(&ss);

    /* Register gpio_control */
    mimi_tool_t gc = {
        .name = "gpio_control",
        .description = "Control a GPIO pin.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"pin\":{\"type\":\"integer\"},\"state\":{\"type\":\"boolean\"}},\"required\":[\"pin\",\"state\"]}",
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

    /* Register ADC/PWM/RGB/UART */
    mimi_tool_t ar = { "adc_read", "Read an ADC channel (0-9).", "{\"type\":\"object\",\"properties\":{\"channel\":{\"type\":\"integer\"}},\"required\":[\"channel\"]}", tool_adc_read };
    tool_registry_register(&ar);

    mimi_tool_t pwm = { "pwm_control", "Control PWM output.", "{\"type\":\"object\",\"properties\":{\"pin\":{\"type\":\"integer\"},\"freq_hz\":{\"type\":\"integer\"},\"duty_percent\":{\"type\":\"number\"},\"stop\":{\"type\":\"boolean\"}},\"required\":[\"pin\"]}", tool_pwm_control };
    tool_registry_register(&pwm);

    mimi_tool_t rgb = { "rgb_control", "Set RGB LED color.", "{\"type\":\"object\",\"properties\":{\"r\":{\"type\":\"integer\"},\"g\":{\"type\":\"integer\"},\"b\":{\"type\":\"integer\"}},\"required\":[\"r\",\"g\",\"b\"]}", tool_rgb_control };
    tool_registry_register(&rgb);

    tool_network_init();
    mimi_tool_t wscan = { "wifi_scan", "Scan for WiFi APs.", "{\"type\":\"object\",\"properties\":{},\"required\":[]}", tool_wifi_scan };
    tool_registry_register(&wscan);

    mimi_tool_t wstat = { "wifi_status", "Get WiFi status.", "{\"type\":\"object\",\"properties\":{},\"required\":[]}", tool_wifi_status };
    tool_registry_register(&wstat);

#ifdef CONFIG_BT_ENABLED
    mimi_tool_t bscan = { "ble_scan", "Scan for BLE devices.", "{\"type\":\"object\",\"properties\":{},\"required\":[]}", tool_ble_scan };
    tool_registry_register(&bscan);
#endif

    mimi_tool_t us = { "uart_send", "Send data via UART.", "{\"type\":\"object\",\"properties\":{\"data\":{\"type\":\"string\"},\"port\":{\"type\":\"integer\"}},\"required\":[\"data\"]}", tool_uart_send };
    tool_registry_register(&us);

    mimi_tool_t ir = { "i2s_read", "Read I2S audio.", "{\"type\":\"object\",\"properties\":{\"bytes\":{\"type\":\"integer\"}},\"required\":[]}", tool_i2s_read };
    tool_registry_register(&ir);

    mimi_tool_t iw = { "i2s_write", "Write I2S audio.", "{\"type\":\"object\",\"properties\":{\"data_base64\":{\"type\":\"string\"}},\"required\":[\"data_base64\"]}", tool_i2s_write };
    tool_registry_register(&iw);

    mimi_tool_t sr = { "system_restart", "Restart system.", "{\"type\":\"object\",\"properties\":{},\"required\":[]}", tool_system_restart };
    tool_registry_register(&sr);

    mimi_tool_t sc = { "skill_create", "Create a skill.", "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"code\":{\"type\":\"string\"}},\"required\":[\"name\",\"code\"]}", tool_skill_create_execute };
    tool_registry_register(&sc);

    mimi_tool_t slt = { "skill_list_templates", "List skill templates.", "{\"type\":\"object\",\"properties\":{},\"required\":[]}", tool_skill_list_templates_execute };
    tool_registry_register(&slt);

    mimi_tool_t sgt = { "skill_get_template", "Get skill template code.", "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"]}", tool_skill_get_template_execute };
    tool_registry_register(&sgt);

    mimi_tool_t sm = { "skill_manage", "Manage skills.", "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"}},\"required\":[\"action\"]}", tool_skill_manage_execute };
    tool_registry_register(&sm);

#if CONFIG_MIMI_ENABLE_MCP
    mimi_tool_t mcp_add = { "mcp_add", "Add MCP source.", "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"url\":{\"type\":\"string\"}},\"required\":[\"name\",\"url\"]}", tool_mcp_add };
    tool_registry_register(&mcp_add);

    mimi_tool_t mcp_list = { "mcp_list", "List MCP sources.", "{\"type\":\"object\",\"properties\":{},\"required\":[]}", tool_mcp_list };
    tool_registry_register(&mcp_list);

    mimi_tool_t mcp_remove = { "mcp_remove", "Remove MCP source.", "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\"}},\"required\":[\"id\"]}", tool_mcp_remove };
    tool_registry_register(&mcp_remove);

    mimi_tool_t mcp_action = { "mcp_action", "Connect/Disconnect MCP.", "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\"},\"action\":{\"type\":\"string\"}},\"required\":[\"id\",\"action\"]}", tool_mcp_action };
    tool_registry_register(&mcp_action);
#endif

    ESP_LOGI(TAG, "Tool registry initialized");
    return ESP_OK;
}
