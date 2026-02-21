#include <stdio.h>
#include <string.h>
#include "tool_mcp.h"
#include "agent/mcp_manager.h"
#include "cJSON.h"

#if CONFIG_MIMI_ENABLE_MCP

esp_err_t tool_mcp_add(const char *input_json, char *output, size_t output_size)
{
    // Parse input: {"name": "filesystem", "url": "ws://192.168.1.100:8080/mcp", "auto_connect": true}
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "{\"error\": \"Invalid JSON\"}");
        return ESP_OK;
    }

    cJSON *name = cJSON_GetObjectItem(root, "name");
    cJSON *url = cJSON_GetObjectItem(root, "url");
    cJSON *auto_connect = cJSON_GetObjectItem(root, "auto_connect");

    if (!cJSON_IsString(name) || !cJSON_IsString(url)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "{\"error\": \"Missing required fields: name, url\"}");
        return ESP_OK;
    }

    const char *transport = "websocket";  // Default to websocket
    cJSON *t = cJSON_GetObjectItem(root, "transport");
    if (cJSON_IsString(t)) {
        transport = t->valuestring;
    }

    bool auto_conn = cJSON_IsTrue(auto_connect);

    int id = mcp_manager_add_source(name->valuestring, transport, url->valuestring, auto_conn);

    cJSON_Delete(root);

    if (id > 0) {
        snprintf(output, output_size, "{\"success\": true, \"id\": %d, \"message\": \"MCP source added\"}", id);
    } else {
        snprintf(output, output_size, "{\"success\": false, \"error\": \"Failed to add MCP source\"}");
    }

    return ESP_OK;
}

esp_err_t tool_mcp_list(const char *input_json, char *output, size_t output_size)
{
    // Parse input: {} (no parameters needed)
    char *json = mcp_manager_get_sources_json();

    if (json) {
        snprintf(output, output_size, "%s", json);
        free(json);
    } else {
        snprintf(output, output_size, "{\"sources\": []}");
    }

    return ESP_OK;
}

esp_err_t tool_mcp_remove(const char *input_json, char *output, size_t output_size)
{
    // Parse input: {"id": 1}
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "{\"error\": \"Invalid JSON\"}");
        return ESP_OK;
    }

    cJSON *id_item = cJSON_GetObjectItem(root, "id");
    if (!cJSON_IsNumber(id_item)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "{\"error\": \"Missing required field: id\"}");
        return ESP_OK;
    }

    int id = id_item->valueint;
    esp_err_t ret = mcp_manager_remove_source(id);

    cJSON_Delete(root);

    if (ret == ESP_OK) {
        snprintf(output, output_size, "{\"success\": true, \"message\": \"MCP source removed\"}");
    } else {
        snprintf(output, output_size, "{\"success\": false, \"error\": \"Failed to remove MCP source\"}");
    }

    return ESP_OK;
}

esp_err_t tool_mcp_action(const char *input_json, char *output, size_t output_size)
{
    // Parse input: {"id": 1, "action": "connect"}
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "{\"error\": \"Invalid JSON\"}");
        return ESP_OK;
    }

    cJSON *id_item = cJSON_GetObjectItem(root, "id");
    cJSON *action_item = cJSON_GetObjectItem(root, "action");

    if (!cJSON_IsNumber(id_item) || !cJSON_IsString(action_item)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "{\"error\": \"Missing required fields: id, action\"}");
        return ESP_OK;
    }

    int id = id_item->valueint;
    esp_err_t ret = mcp_manager_source_action(id, action_item->valuestring);

    cJSON_Delete(root);

    if (ret == ESP_OK) {
        snprintf(output, output_size, "{\"success\": true, \"message\": \"Action '%s' performed\", \"id\": %d}",
                 action_item->valuestring, id);
    } else {
        snprintf(output, output_size, "{\"success\": false, \"error\": \"Failed to perform action\"}");
    }

    return ESP_OK;
}

#else

// Stub implementations when MCP is disabled
esp_err_t tool_mcp_add(const char *input_json, char *output, size_t output_size)
{
    snprintf(output, output_size, "{\"error\": \"MCP is not enabled\"}");
    return ESP_OK;
}

esp_err_t tool_mcp_list(const char *input_json, char *output, size_t output_size)
{
    snprintf(output, output_size, "{\"error\": \"MCP is not enabled\"}");
    return ESP_OK;
}

esp_err_t tool_mcp_remove(const char *input_json, char *output, size_t output_size)
{
    snprintf(output, output_size, "{\"error\": \"MCP is not enabled\"}");
    return ESP_OK;
}

esp_err_t tool_mcp_action(const char *input_json, char *output, size_t output_size)
{
    snprintf(output, output_size, "{\"error\": \"MCP is not enabled\"}");
    return ESP_OK;
}

#endif
