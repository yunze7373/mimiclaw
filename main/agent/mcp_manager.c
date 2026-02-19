#include "agent/mcp_manager.h"
#include "agent/mcp_client.h"
#include "tools/tool_registry.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "mcp_mgr";

#define CONFIG_PATH "/spiffs/config/mcp_sources.json"
#define MAX_SOURCES 4
#define MAX_MCP_TOOLS 32

typedef struct {
    int id;
    char name[32];
    char transport[16];
    char url[256];
    bool auto_connect;
    bool enabled;
    mcp_client_t *client;
    // Request tracking
    int request_id_seq;
    SemaphoreHandle_t pending_sema;
    char *pending_output;
    size_t pending_output_size;
    int pending_req_id;
    bool pending_busy;
} mcp_source_t;

static mcp_source_t s_sources[MAX_SOURCES];
static int s_source_id_counter = 1;

typedef struct {
    int source_idx;
    char original_name[64];
    bool used;
} mcp_tool_slot_t;

static mcp_tool_slot_t s_tool_slots[MAX_MCP_TOOLS];

/* ── Tool Trampoline System ──────────────────────────────────────── */

static esp_err_t mcp_execute_tool(int slot_idx, const char *input_json, char *output, size_t output_size);

#define TRAMPOLINE(N) \
    static esp_err_t mcp_trampoline_##N(const char *input, char *output, size_t len) { \
        return mcp_execute_tool((N), input, output, len); \
    }

/* Generate 32 trampolines */
TRAMPOLINE(0) TRAMPOLINE(1) TRAMPOLINE(2) TRAMPOLINE(3)
TRAMPOLINE(4) TRAMPOLINE(5) TRAMPOLINE(6) TRAMPOLINE(7)
TRAMPOLINE(8) TRAMPOLINE(9) TRAMPOLINE(10) TRAMPOLINE(11)
TRAMPOLINE(12) TRAMPOLINE(13) TRAMPOLINE(14) TRAMPOLINE(15)
TRAMPOLINE(16) TRAMPOLINE(17) TRAMPOLINE(18) TRAMPOLINE(19)
TRAMPOLINE(20) TRAMPOLINE(21) TRAMPOLINE(22) TRAMPOLINE(23)
TRAMPOLINE(24) TRAMPOLINE(25) TRAMPOLINE(26) TRAMPOLINE(27)
TRAMPOLINE(28) TRAMPOLINE(29) TRAMPOLINE(30) TRAMPOLINE(31)

static const esp_err_t (*s_trampolines[MAX_MCP_TOOLS])(const char *, char *, size_t) = {
    mcp_trampoline_0, mcp_trampoline_1, mcp_trampoline_2, mcp_trampoline_3,
    mcp_trampoline_4, mcp_trampoline_5, mcp_trampoline_6, mcp_trampoline_7,
    mcp_trampoline_8, mcp_trampoline_9, mcp_trampoline_10, mcp_trampoline_11,
    mcp_trampoline_12, mcp_trampoline_13, mcp_trampoline_14, mcp_trampoline_15,
    mcp_trampoline_16, mcp_trampoline_17, mcp_trampoline_18, mcp_trampoline_19,
    mcp_trampoline_20, mcp_trampoline_21, mcp_trampoline_22, mcp_trampoline_23,
    mcp_trampoline_24, mcp_trampoline_25, mcp_trampoline_26, mcp_trampoline_27,
    mcp_trampoline_28, mcp_trampoline_29, mcp_trampoline_30, mcp_trampoline_31
};

/* ── Callbacks ───────────────────────────────────────────────────── */

static void on_connect(mcp_client_t *client)
{
    mcp_source_t *src = (mcp_source_t *)mcp_client_get_ctx(client);
    ESP_LOGI(TAG, "Source %s connected, sending tools/list", src->name);
    
    const char *msg = "{\"jsonrpc\":\"2.0\",\"method\":\"tools/list\",\"id\":1}";
    mcp_client_send(client, msg);
}

static void on_disconnect(mcp_client_t *client)
{
    mcp_source_t *src = (mcp_source_t *)mcp_client_get_ctx(client);
    ESP_LOGI(TAG, "Source %s disconnected", src->name);
    
    /* Remove tools associated with this source */
    bool changed = false;
    for (int i = 0; i < MAX_MCP_TOOLS; i++) {
        if (s_tool_slots[i].used && s_tool_slots[i].source_idx == (src - s_sources)) {
            s_tool_slots[i].used = false;
            // Note: tool_registry doesn't have an unregister_all mechanism yet besides rebuild
            changed = true;
        }
    }
    if (changed) {
        // We really should unregister specific tools, but registry is simple. 
        // For now, rebuild global JSON. Ghost entries might remain in array until reboot if registry doesn't support removal.
        // TODO: Implement tool_registry_remove_by_prefix or similar.
    }
}

static void handle_tools_list_response(mcp_source_t *src, cJSON *result)
{
    cJSON *tools = cJSON_GetObjectItem(result, "tools");
    if (!cJSON_IsArray(tools)) return;

    int src_idx = (int)(src - s_sources);
    int tools_added = 0;

    cJSON *tool = NULL;
    cJSON_ArrayForEach(tool, tools) {
        /* Find free slot */
        int slot = -1;
        for (int i = 0; i < MAX_MCP_TOOLS; i++) {
            if (!s_tool_slots[i].used) {
                slot = i;
                break;
            }
        }
        if (slot == -1) {
            ESP_LOGW(TAG, "Max MCP tools limit reached (%d)", MAX_MCP_TOOLS);
            break;
        }

        cJSON *name = cJSON_GetObjectItem(tool, "name");
        cJSON *desc = cJSON_GetObjectItem(tool, "description");
        cJSON *schema = cJSON_GetObjectItem(tool, "inputSchema");

        if (cJSON_IsString(name)) {
            mcp_tool_slot_t *ts = &s_tool_slots[slot];
            ts->source_idx = src_idx;
            snprintf(ts->original_name, sizeof(ts->original_name), "%s", name->valuestring);
            
            /* Register with registry */
            /* We construct a unique name if needed? For now allow collision or prefix? */
            /* Let's prefix to avoid collision: "source_name:tool_name" ?? */
            /* No, just use name for now, simpler for Agent */
            
            char schema_str[1024] = "{}";
            if (schema) {
                char *s = cJSON_PrintUnformatted(schema);
                if (s) {
                    snprintf(schema_str, sizeof(schema_str), "%s", s);
                    free(s);
                }
            }

            mimi_tool_t t = {
                .name = ts->original_name,
                .description = cJSON_IsString(desc) ? desc->valuestring : "",
                .input_schema_json = schema_str,
                .execute = s_trampolines[slot]
            };
            tool_registry_register(&t);
            ts->used = true;
            tools_added++;
        }
    }
    ESP_LOGI(TAG, "Added %d tools from %s", tools_added, src->name);
    tool_registry_rebuild_json();
}

static void on_message(mcp_client_t *client, const char *json, size_t len)
{
    mcp_source_t *src = (mcp_source_t *)mcp_client_get_ctx(client);
    
    // cJSON_ParseWithLength is better but we might have old cJSON
    // Use parse buffer copy to be safe or ensure null term
    cJSON *root = cJSON_Parse(json); // Assumes null-terminated? websocket data usually isn't!
    // esp_websocket_client data is NOT null terminated.
    // We must handle this.
    char *buf = malloc(len + 1);
    if (!buf) return;
    memcpy(buf, json, len);
    buf[len] = '\0';
    
    cJSON *json_root = cJSON_Parse(buf);
    free(buf);
    
    if (!json_root) return;

    cJSON *id = cJSON_GetObjectItem(json_root, "id");
    cJSON *result = cJSON_GetObjectItem(json_root, "result");
    cJSON *error = cJSON_GetObjectItem(json_root, "error");

    if (cJSON_IsNumber(id)) {
        if (id->valueint == 1) { /* Init handshake */
            handle_tools_list_response(src, result);
        } 
        else if (src->pending_busy && src->pending_req_id == id->valueint) {
             /* Handle execution result */
             if (error) {
                 char *est = cJSON_PrintUnformatted(error);
                 snprintf(src->pending_output, src->pending_output_size, "{\"error\":%s}", est ? est : "unknown");
                 if(est) free(est);
             } else if (result) {
                 // Simplified content extraction
                 cJSON *content = cJSON_GetObjectItem(result, "content");
                 char *cstr = cJSON_PrintUnformatted(content ? content : result);
                 snprintf(src->pending_output, src->pending_output_size, "%s", cstr ? cstr : "{}");
                 if(cstr) free(cstr);
             }
             xSemaphoreGive(src->pending_sema);
        }
    }
    cJSON_Delete(json_root);
}

/* ── Trampoline Logic ────────────────────────────────────────────── */

static esp_err_t mcp_execute_tool(int slot_idx, const char *input_json, char *output, size_t output_size)
{
    if (slot_idx < 0 || slot_idx >= MAX_MCP_TOOLS || !s_tool_slots[slot_idx].used) {
        return ESP_ERR_INVALID_STATE;
    }
    
    mcp_tool_slot_t *slot = &s_tool_slots[slot_idx];
    mcp_source_t *src = &s_sources[slot->source_idx];
    
    if (!src->client || !mcp_client_is_connected(src->client)) {
         snprintf(output, output_size, "{\"error\":\"Server disconnected\"}");
         return ESP_OK;
    }
    
    /* Lock source for request */
    if (src->pending_busy) {
        snprintf(output, output_size, "{\"error\":\"Server busy\"}");
        return ESP_OK;
    }
    
    src->pending_busy = true;
    src->pending_req_id = ++src->request_id_seq;
    if (src->request_id_seq < 10) src->request_id_seq = 10;
    
    src->pending_output = output;
    src->pending_output_size = output_size;
    
    if (!src->pending_sema) src->pending_sema = xSemaphoreCreateBinary();
    xSemaphoreTake(src->pending_sema, 0);

    /* Construct JSON-RPC */
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddStringToObject(req, "method", "tools/call");
    cJSON_AddNumberToObject(req, "id", src->pending_req_id);
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "name", slot->original_name);
    cJSON *args = cJSON_Parse(input_json);
    if (args) cJSON_AddItemToObject(params, "arguments", args);
    cJSON_AddItemToObject(req, "params", params);
    
    char *req_str = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    
    if (req_str) {
        mcp_client_send(src->client, req_str);
        free(req_str);
    }
    
    /* Wait */
    if (xSemaphoreTake(src->pending_sema, pdMS_TO_TICKS(10000)) != pdTRUE) {
        snprintf(output, output_size, "{\"error\":\"Timeout\"}");
    }
    
    src->pending_busy = false;
    return ESP_OK;
}

/* ── Manager API ─────────────────────────────────────────────────── */

char *mcp_manager_get_sources_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "sources", arr);

    for (int i = 0; i < MAX_SOURCES; i++) {
        if (s_sources[i].id != 0) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "id", s_sources[i].id);
            cJSON_AddStringToObject(item, "name", s_sources[i].name);
            cJSON_AddStringToObject(item, "url", s_sources[i].url);
            bool connected = s_sources[i].client && mcp_client_is_connected(s_sources[i].client);
            cJSON_AddStringToObject(item, "status", connected ? "connected" : "disconnected");
            cJSON_AddItemToArray(arr, item);
        }
    }
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

esp_err_t mcp_manager_init(void)
{
    // TODO: Load from JSON
    // For now, init manual source if needed
    // mcp_manager_add_source("Filesystem", "websocket", "ws://192.168.31.50:8080/mcp", true);
    return ESP_OK;
}

int mcp_manager_add_source(const char *name, const char *transport, const char *url, bool auto_connect)
{
    int idx = -1;
    for (int i = 0; i < MAX_SOURCES; i++) {
        if (s_sources[i].id == 0) {
            idx = i;
            break;
        }
    }
    if (idx == -1) return -1;

    mcp_source_t *src = &s_sources[idx];
    src->id = s_source_id_counter++;
    strncpy(src->name, name, sizeof(src->name)-1);
    strncpy(src->url, url, sizeof(src->url)-1);
    strncpy(src->transport, transport, sizeof(src->transport)-1);
    src->auto_connect = auto_connect;
    src->enabled = true;

    if (auto_connect) {
        mcp_manager_source_action(src->id, "connect");
    }
    return src->id;
}

esp_err_t mcp_manager_source_action(int id, const char *action)
{
    mcp_source_t *src = NULL;
    for (int i = 0; i < MAX_SOURCES; i++) {
        if (s_sources[i].id == id) {
            src = &s_sources[i];
            break;
        }
    }
    if (!src) return ESP_ERR_NOT_FOUND;

    if (strcmp(action, "connect") == 0) {
        if (src->client) mcp_client_destroy(src->client);
        
        mcp_client_config_t cfg = {
            .on_connect = on_connect,
            .on_disconnect = on_disconnect,
            .on_message = on_message,
            .user_ctx = src,
        };
        strncpy(cfg.url, src->url, sizeof(cfg.url)-1);
        strncpy(cfg.transport, src->transport, sizeof(cfg.transport)-1);

        src->client = mcp_client_create(&cfg);
        return mcp_client_connect(src->client);
    } 
    else if (strcmp(action, "disconnect") == 0) {
        if (src->client) {
            mcp_client_disconnect(src->client);
            mcp_client_destroy(src->client);
            src->client = NULL;
        }
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}
