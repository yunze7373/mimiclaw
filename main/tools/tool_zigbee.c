#include "tools/tool_registry.h"
#include "extensions/zigbee_gateway.h"
#include "cJSON.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "tool_zigbee";

/* ── Tool: zigbee_list ───────────────────────────────────────────── */
static esp_err_t tool_zigbee_list(const char *input, char *output, size_t out_len)
{
    (void)input;
    char *json = zigbee_gateway_json();
    if (json) {
        snprintf(output, out_len, "%s", json);
        free(json);
        return ESP_OK;
    } else {
        snprintf(output, out_len, "{\"error\": \"Failed to get device list\"}");
        return ESP_FAIL;
    }
}

/* ── Tool: zigbee_permit_join ────────────────────────────────────── */
static esp_err_t tool_zigbee_permit_join(const char *input, char *output, size_t out_len)
{
    bool enable = true;
    cJSON *root = cJSON_Parse(input);
    if (root) {
        cJSON *val = cJSON_GetObjectItem(root, "enable");
        if (cJSON_IsBool(val)) {
            enable = cJSON_IsTrue(val);
        }
        cJSON_Delete(root);
    }
    
    zigbee_gateway_permit_join(enable);
    snprintf(output, out_len, "Zigbee Permit Join: %s", enable ? "ENABLED" : "DISABLED");
    return ESP_OK;
}

/* ── Tool: zigbee_control ────────────────────────────────────────── */
static esp_err_t tool_zigbee_control(const char *input, char *output, size_t out_len)
{
    cJSON *root = cJSON_Parse(input);
    if (!root) {
        snprintf(output, out_len, "Error: Invalid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *addr_item = cJSON_GetObjectItem(root, "nwk_addr");
    cJSON *state_item = cJSON_GetObjectItem(root, "state"); // "on" or "off"

    if (addr_item && state_item) {
        uint16_t addr = (uint16_t)addr_item->valueint;
        bool on = (strcmp(state_item->valuestring, "on") == 0);
        
        esp_err_t err = zigbee_gateway_control_onoff(addr, on);
        if (err == ESP_OK) {
            snprintf(output, out_len, "Command sent to 0x%04X", addr);
        } else {
            snprintf(output, out_len, "Failed to send command (Error %d)", err);
        }
        cJSON_Delete(root);
        return err;
    } else {
        snprintf(output, out_len, "Error: Missing 'nwk_addr' (int) or 'state' (string: 'on'/'off')");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
}


void register_zigbee_tools(void)
{
    static const mimi_tool_t tool_list = {
        .name = "zigbee_list",
        .description = "List all paired Zigbee devices. Returns JSON array.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
        .execute = tool_zigbee_list,
    };
    static const mimi_tool_t tool_permit_join = {
        .name = "zigbee_permit_join",
        .description = "Enable or disable Zigbee joining. Input: {\"enable\": true/false}.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"enable\":{\"type\":\"boolean\"}},\"required\":[]}",
        .execute = tool_zigbee_permit_join,
    };
    static const mimi_tool_t tool_control = {
        .name = "zigbee_control",
        .description = "Control Zigbee device state. Input: {\"nwk_addr\": 1234, \"state\": \"on\"/\"off\"}.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"nwk_addr\":{\"type\":\"integer\"},\"state\":{\"type\":\"string\"}},\"required\":[\"nwk_addr\",\"state\"]}",
        .execute = tool_zigbee_control,
    };

    tool_registry_register(&tool_list);
    tool_registry_register(&tool_permit_join);
    tool_registry_register(&tool_control);
    ESP_LOGI(TAG, "Zigbee tools registered");
}
