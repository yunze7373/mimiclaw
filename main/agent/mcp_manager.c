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
} mcp_source_t;

static mcp_source_t s_sources[MAX_SOURCES];
static int s_source_id_counter = 1;
static bool s_mcp_started = false;

typedef struct {
    int source_idx;
    char original_name[64];
    bool used;
} mcp_tool_slot_t;

static mcp_tool_slot_t s_tool_slots[MAX_MCP_TOOLS];

/* Forward decls */
static esp_err_t mcp_execute_tool(int slot_idx, const char *input_json, char *output, size_t output_size);
static void save_config(void);

/* ── Tool Trampoline System ──────────────────────────────────────── */

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

static esp_err_t (*s_trampolines[MAX_MCP_TOOLS])(const char *, char *, size_t) = {
    mcp_trampoline_0, mcp_trampoline_1, mcp_trampoline_2, mcp_trampoline_3,
    mcp_trampoline_4, mcp_trampoline_5, mcp_trampoline_6, mcp_trampoline_7,
    mcp_trampoline_8, mcp_trampoline_9, mcp_trampoline_10, mcp_trampoline_11,
    mcp_trampoline_12, mcp_trampoline_13, mcp_trampoline_14, mcp_trampoline_15,
    mcp_trampoline_16, mcp_trampoline_17, mcp_trampoline_18, mcp_trampoline_19,
    mcp_trampoline_20, mcp_trampoline_21, mcp_trampoline_22, mcp_trampoline_23,
    mcp_trampoline_24, mcp_trampoline_25, mcp_trampoline_26, mcp_trampoline_27,
    mcp_trampoline_28, mcp_trampoline_29, mcp_trampoline_30, mcp_trampoline_31
};

/* ── Tool Registration Logic ─────────────────────────────────────── */

static void handle_tools_list_response(mcp_source_t *src, const char *json_result)
{
    cJSON *root = cJSON_Parse(json_result);
    if (!root) return;
    
    cJSON *tools = cJSON_GetObjectItem(root, "tools");
    if (!cJSON_IsArray(tools)) {
        cJSON_Delete(root);
        return;
    }

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
        if (slot == -1) break;

        cJSON *name = cJSON_GetObjectItem(tool, "name");
        cJSON *desc = cJSON_GetObjectItem(tool, "description");
        cJSON *schema = cJSON_GetObjectItem(tool, "inputSchema");

        if (cJSON_IsString(name)) {
            mcp_tool_slot_t *ts = &s_tool_slots[slot];
            ts->source_idx = src_idx;
            snprintf(ts->original_name, sizeof(ts->original_name), "%s", name->valuestring);
            
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
    cJSON_Delete(root);
}

/* ── Callbacks ───────────────────────────────────────────────────── */

static void on_message(mcp_client_t *client, const char *json, size_t len)
{
    // Notifications (no ID) go here.
    // Assuming mcp_client handles Response (ID) matching internally.
    // ESP_LOGD(TAG, "Notification: %.*s", (int)len, json);
}

static void on_tools_list(void *ctx, int id, const char *json_result, esp_err_t err)
{
    mcp_source_t *src = (mcp_source_t *)ctx;
    if (err == ESP_OK && json_result) {
        handle_tools_list_response(src, json_result);
    } else {
        ESP_LOGE(TAG, "tools/list failed for %s", src->name);
    }
}

static void on_connect(mcp_client_t *client)
{
    mcp_source_t *src = (mcp_source_t *)mcp_client_get_ctx(client);
    ESP_LOGI(TAG, "Source %s connected, refreshing tools...", src->name);
    
    // Request tools/list
    mcp_client_send_request(client, "tools/list", NULL, on_tools_list, src);
}

static void on_disconnect(mcp_client_t *client)
{
    mcp_source_t *src = (mcp_source_t *)mcp_client_get_ctx(client);
    ESP_LOGI(TAG, "Source %s disconnected cleaning up tools", src->name);
    
    // Cleanup tools logic
    for (int i = 0; i < MAX_MCP_TOOLS; i++) {
        if (s_tool_slots[i].used && s_tool_slots[i].source_idx == (src - s_sources)) {
            s_tool_slots[i].used = false;
        }
    }
    tool_registry_rebuild_json();
}

/* ── Tool Execution Wrapper ──────────────────────────────────────── */

typedef struct {
    SemaphoreHandle_t sema;
    char *output_buf;
    size_t output_len;
} tool_exec_ctx_t;

static void on_tool_call_result(void *ctx, int id, const char *json_result, esp_err_t err)
{
    tool_exec_ctx_t *tctx = (tool_exec_ctx_t *)ctx;
    if (err == ESP_OK && json_result) {
        // Extract content from result
        cJSON *root = cJSON_Parse(json_result);
        if (root) {
            cJSON *content = cJSON_GetObjectItem(root, "content");
            char *s = cJSON_PrintUnformatted(content ? content : root); // Content or full result? MCP says result has content.
            if (s) {
                snprintf(tctx->output_buf, tctx->output_len, "%s", s);
                free(s);
            }
            cJSON_Delete(root);
        } else {
             snprintf(tctx->output_buf, tctx->output_len, "{}");
        }
    } else {
        snprintf(tctx->output_buf, tctx->output_len, "{\"error\":\"RPC Error\"}");
    }
    xSemaphoreGive(tctx->sema);
}

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

    tool_exec_ctx_t tctx = {
        .sema = xSemaphoreCreateBinary(),
        .output_buf = output,
        .output_len = output_size
    };
    
    cJSON *args = cJSON_Parse(input_json);
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "name", slot->original_name);
    if (args) cJSON_AddItemToObject(params, "arguments", args);
    char *params_str = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    
    esp_err_t err = mcp_client_send_request(src->client, "tools/call", params_str, on_tool_call_result, &tctx);
    free(params_str);
    
    if (err == ESP_OK) {
        xSemaphoreTake(tctx.sema, pdMS_TO_TICKS(10000)); // 10s timeout
    } else {
        snprintf(output, output_size, "{\"error\":\"Send failed\"}");
    }
    
    vSemaphoreDelete(tctx.sema);
    return ESP_OK;
}

/* ── Manager Internal ────────────────────────────────────────────── */

static int add_source_internal(const char *name, const char *transport, const char *url, bool auto_connect, int force_id)
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
    src->id = (force_id > 0) ? force_id : s_source_id_counter++;
    if (src->id >= s_source_id_counter) s_source_id_counter = src->id + 1;
    
    strncpy(src->name, name, sizeof(src->name)-1);
    strncpy(src->url, url, sizeof(src->url)-1);
    strncpy(src->transport, transport, sizeof(src->transport)-1);
    src->auto_connect = auto_connect;
    src->enabled = true;
    src->client = NULL;

    return src->id;
}

static void load_config(void)
{
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) return;
    
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char *data = malloc(len + 1);
    if (data) {
        fread(data, 1, len, f);
        data[len] = 0;
        cJSON *root = cJSON_Parse(data);
        if (root) {
            cJSON *arr = cJSON_GetObjectItem(root, "sources");
            cJSON *item = NULL;
            cJSON_ArrayForEach(item, arr) {
                cJSON *id = cJSON_GetObjectItem(item, "id");
                cJSON *name = cJSON_GetObjectItem(item, "name");
                cJSON *trans = cJSON_GetObjectItem(item, "transport");
                cJSON *url = cJSON_GetObjectItem(item, "url");
                cJSON *auto_conn = cJSON_GetObjectItem(item, "auto_connect");
                
                if (name && url && trans) {
                    add_source_internal(name->valuestring, trans->valuestring, url->valuestring, 
                                        cJSON_IsTrue(auto_conn), id ? id->valueint : 0);
                }
            }
            cJSON_Delete(root);
        }
        free(data);
    }
    fclose(f);
}

static void save_config(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "sources", arr);
    
    for (int i = 0; i < MAX_SOURCES; i++) {
        if (s_sources[i].id != 0) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "id", s_sources[i].id);
            cJSON_AddStringToObject(item, "name", s_sources[i].name);
            cJSON_AddStringToObject(item, "transport", s_sources[i].transport);
            cJSON_AddStringToObject(item, "url", s_sources[i].url);
            cJSON_AddBoolToObject(item, "auto_connect", s_sources[i].auto_connect);
            cJSON_AddItemToArray(arr, item);
        }
    }
    
    char *str = cJSON_Print(root);
    if (str) {
        FILE *f = fopen(CONFIG_PATH, "w");
        if (f) {
            fprintf(f, "%s", str);
            fclose(f);
        }
        free(str);
    }
    cJSON_Delete(root);
}

/* ── Manager API ─────────────────────────────────────────────────── */

esp_err_t mcp_manager_init(void)
{
    load_config();
    return ESP_OK;
}

esp_err_t mcp_manager_start(void)
{
    s_mcp_started = true;
    for (int i = 0; i < MAX_SOURCES; i++) {
        if (s_sources[i].id != 0 && s_sources[i].auto_connect) {
            mcp_manager_source_action(s_sources[i].id, "connect");
        }
    }
    return ESP_OK;
}

int mcp_manager_add_source(const char *name, const char *transport, const char *url, bool auto_connect)
{
    int id = add_source_internal(name, transport, url, auto_connect, 0);
    if (id > 0) {
        save_config();
        if (auto_connect && s_mcp_started) {
            mcp_manager_source_action(id, "connect");
        }
    }
    return id;
}

esp_err_t mcp_manager_remove_source(int id)
{
    for (int i = 0; i < MAX_SOURCES; i++) {
        if (s_sources[i].id == id) {
            mcp_manager_source_action(id, "disconnect");
            
            memset(&s_sources[i], 0, sizeof(mcp_source_t));
            save_config();
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

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
            
            bool connected = (s_sources[i].client != NULL);
            cJSON_AddStringToObject(item, "status", connected ? "connected" : "disconnected");
            
            /* Count tools for this source */
            int tool_count = 0;
            if (connected) {
                for (int t = 0; t < MAX_MCP_TOOLS; t++) {
                    if (s_tool_slots[t].used && s_tool_slots[t].source_idx == i) {
                        tool_count++;
                    }
                }
            }
            cJSON_AddNumberToObject(item, "tools_count", tool_count);
            
            cJSON_AddItemToArray(arr, item);
        }
    }
    
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
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
            on_disconnect(NULL); // Force cleanup visual state? on_disconnect callback usually called by client destroy/event? 
            // mcp_client_destroy calls stop, which might trigger event?
            // Safer to call logic here if event didn't fire?
            // Actually destroy might not fire event if loop stopped.
            // Let's rely on event if possible, but force cleanup too.
        }
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}
