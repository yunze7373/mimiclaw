#include "agent/mcp_client.h"
#include "mimi_config.h"
#include "esp_log.h"
#include <string.h>

#if CONFIG_MIMI_ENABLE_MCP && __has_include("esp_websocket_client.h")
#define MIMI_MCP_IMPL_ENABLED 1
#include "tools/tool_registry.h"
#include "cJSON.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#else
#define MIMI_MCP_IMPL_ENABLED 0
#endif

#if MIMI_MCP_IMPL_ENABLED

static const char *TAG = "mcp_client";

static esp_websocket_client_handle_t s_client = NULL;
static bool s_connected = false;
static int s_request_id = 1;

/* ── Tool Trampoline System ──────────────────────────────────────── */

#define MAX_MCP_TOOLS 10

typedef struct {
    bool used;
    char name[64];
    char description[128];
    char input_schema[1024]; /* Fixed size for simplicity */
} mcp_tool_slot_t;

static mcp_tool_slot_t s_mcp_tools[MAX_MCP_TOOLS];

/* Pending request tracking */
typedef struct {
    int id;
    char *output_buf;
    size_t output_size;
    SemaphoreHandle_t ready;
    bool active;
} mcp_pending_req_t;

// Only support 1 concurrent request for now to save RAM/Complexity
static mcp_pending_req_t s_pending_req = {0};

/* Forward declaration */
static esp_err_t mcp_execute_common(int idx, const char *input_json, char *output, size_t output_size);

/* Trampolines */
#define TRAMPOLINE(N) \
    static esp_err_t mcp_trampoline_##N(const char *input, char *output, size_t len) { \
        return mcp_execute_common((N), input, output, len); \
    }

TRAMPOLINE(0) TRAMPOLINE(1) TRAMPOLINE(2) TRAMPOLINE(3) TRAMPOLINE(4)
TRAMPOLINE(5) TRAMPOLINE(6) TRAMPOLINE(7) TRAMPOLINE(8) TRAMPOLINE(9)

typedef esp_err_t (*tool_exec_fn)(const char *, char *, size_t);
static const tool_exec_fn s_trampolines[] = {
    mcp_trampoline_0, mcp_trampoline_1, mcp_trampoline_2, mcp_trampoline_3, mcp_trampoline_4,
    mcp_trampoline_5, mcp_trampoline_6, mcp_trampoline_7, mcp_trampoline_8, mcp_trampoline_9
};


/* ── WebSocket Handler ───────────────────────────────────────────── */

static void handle_tools_list_response(cJSON *result)
{
    cJSON *tools = cJSON_GetObjectItem(result, "tools");
    if (!cJSON_IsArray(tools)) return;

    int idx = 0;
    cJSON *tool = NULL;
    cJSON_ArrayForEach(tool, tools) {
        if (idx >= MAX_MCP_TOOLS) break;
        
        cJSON *name = cJSON_GetObjectItem(tool, "name");
        cJSON *desc = cJSON_GetObjectItem(tool, "description");
        cJSON *schema = cJSON_GetObjectItem(tool, "inputSchema");

        if (cJSON_IsString(name)) {
            mcp_tool_slot_t *slot = &s_mcp_tools[idx];
            snprintf(slot->name, sizeof(slot->name), "%s", name->valuestring);
            snprintf(slot->description, sizeof(slot->description), "%s", cJSON_IsString(desc) ? desc->valuestring : "");
            
            if (schema) {
                char *sstr = cJSON_PrintUnformatted(schema);
                if (sstr) {
                    snprintf(slot->input_schema, sizeof(slot->input_schema), "%s", sstr);
                    free(sstr);
                }
            } else {
                strcpy(slot->input_schema, "{}");
            }
            slot->used = true;

            /* Register to main registry */
            mimi_tool_t t = {
                .name = slot->name,
                .description = slot->description,
                .input_schema_json = slot->input_schema,
                .execute = s_trampolines[idx]
            };
            tool_registry_register(&t);
            idx++;
        }
    }
    ESP_LOGI(TAG, "Registered %d MCP tools", idx);
    tool_registry_rebuild_json();
}

static void handle_call_response(int id, cJSON *result, cJSON *error)
{
    if (!s_pending_req.active || s_pending_req.id != id) return;

    if (error) {
        char *est = cJSON_PrintUnformatted(error);
        if (est) {
            snprintf(s_pending_req.output_buf, s_pending_req.output_size, "{\"error\":%s}", est);
            free(est);
        }
    } else if (result) {
        cJSON *content = cJSON_GetObjectItem(result, "content");
        /* MCP content is array of objects {type, text/resource}. simplified: dump all text */
        if (cJSON_IsArray(content)) {
            /* Naive dump */
            char *cstr = cJSON_PrintUnformatted(content);
             if (cstr) {
                snprintf(s_pending_req.output_buf, s_pending_req.output_size, "%s", cstr);
                free(cstr);
            }
        } else {
             snprintf(s_pending_req.output_buf, s_pending_req.output_size, "{\"result\":\"ok\"}");
        }
    } else {
        snprintf(s_pending_req.output_buf, s_pending_req.output_size, "{\"error\":\"empty response\"}");
    }
    
    xSemaphoreGive(s_pending_req.ready);
}

static void on_message(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) return;

    cJSON *id = cJSON_GetObjectItem(root, "id");
    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *error = cJSON_GetObjectItem(root, "error");
    
    if (cJSON_IsNumber(id)) {
        if (id->valueint == 1) { /* Assuming hardcoded ID 1 for tools/list */
            handle_tools_list_response(result);
        } else {
            handle_call_response(id->valueint, result, error);
        }
    }
    cJSON_Delete(root);
}

static void ws_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Connected to MCP Server");
            s_connected = true;
            /* Send tools/list */
            const char *msg = "{\"jsonrpc\":\"2.0\",\"method\":\"tools/list\",\"id\":1}";
            esp_websocket_client_send_text(s_client, msg, strlen(msg), pdMS_TO_TICKS(1000));
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "Disconnected from MCP Server");
            s_connected = false;
            break;
        case WEBSOCKET_EVENT_DATA:
            if (data->op_code == WS_TRANSPORT_OPCODES_TEXT) {
                on_message(data->data_ptr, data->data_len);
            }
            break;
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket Error");
            break;
    }
}

/* ── Tool Execution Wrapper ──────────────────────────────────────── */

static esp_err_t mcp_execute_common(int idx, const char *input_json, char *output, size_t output_size)
{
    if (!s_connected) {
        snprintf(output, output_size, "{\"error\":\"MCP server not connected\"}");
        return ESP_OK; /* Not sys error */
    }
    if (idx < 0 || idx >= MAX_MCP_TOOLS || !s_mcp_tools[idx].used) {
        snprintf(output, output_size, "{\"error\":\"Invalid tool index\"}");
        return ESP_OK;
    }

    if (s_pending_req.active) {
        snprintf(output, output_size, "{\"error\":\"MCP busy\"}");
        return ESP_OK;
    }

    /* Prepare pending req */
    s_request_id++;
    if (s_request_id <= 1) s_request_id = 2; /* 1 reserved for list */
    
    if (!s_pending_req.ready) s_pending_req.ready = xSemaphoreCreateBinary();
    s_pending_req.id = s_request_id;
    s_pending_req.output_buf = output;
    s_pending_req.output_size = output_size;
    s_pending_req.active = true;
    xSemaphoreTake(s_pending_req.ready, 0); /* Clear */

    /* Build JSON-RPC */
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddStringToObject(req, "method", "tools/call");
    cJSON_AddNumberToObject(req, "id", s_request_id);
    
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "name", s_mcp_tools[idx].name);
    cJSON *args = cJSON_Parse(input_json);
    if (args) cJSON_AddItemToObject(params, "arguments", args);
    else cJSON_AddObjectToObject(params, "arguments");
    
    cJSON_AddItemToObject(req, "params", params);
    
    char *req_str = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    
    if (req_str) {
        esp_websocket_client_send_text(s_client, req_str, strlen(req_str), pdMS_TO_TICKS(1000));
        free(req_str);
    } else {
        s_pending_req.active = false;
        return ESP_ERR_NO_MEM;
    }

    /* Wait for response with timeout */
    if (xSemaphoreTake(s_pending_req.ready, pdMS_TO_TICKS(10000)) != pdTRUE) {
        snprintf(output, output_size, "{\"error\":\"MCP timeout\"}");
    }
    
    s_pending_req.active = false;
    return ESP_OK;
}


/* ── Init ────────────────────────────────────────────────────────── */

esp_err_t mcp_client_init(void)
{
    const char *url = MIMI_MCP_SERVER_URL;
    if (!url || !url[0]) {
        ESP_LOGW(TAG, "No MCP Server URL configured");
        return ESP_ERR_INVALID_ARG;
    }

    esp_websocket_client_config_t websocket_cfg = {};
    websocket_cfg.uri = url;

    s_client = esp_websocket_client_init(&websocket_cfg);
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, ws_event_handler, (void *)s_client);

    esp_websocket_client_start(s_client);
    ESP_LOGI(TAG, "Starting MCP Client -> %s", url);

    return ESP_OK;
}

bool mcp_client_is_connected(void)
{
    return s_connected;
}

esp_err_t mcp_client_send(const char *json_data) 
{
    if (!s_connected || !s_client) return ESP_FAIL;
    return esp_websocket_client_send_text(s_client, json_data, strlen(json_data), pdMS_TO_TICKS(1000));
}

#else

/* Dummy impl if disabled or esp_websocket_client is unavailable */
esp_err_t mcp_client_init(void)
{
#if CONFIG_MIMI_ENABLE_MCP
    ESP_LOGW("mcp_client", "MCP enabled, but esp_websocket_client is unavailable; running disabled");
#endif
    return ESP_OK;
}
bool mcp_client_is_connected(void) { return false; }
esp_err_t mcp_client_send(const char *json_data)
{
    (void)json_data;
    return ESP_FAIL;
}

#endif
