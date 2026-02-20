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

typedef struct {
    int id;
    char name[32];
    char transport[16];
    char url[256];
    bool auto_connect;
    bool enabled;
    mcp_client_t *client;
    
    /* Phase 10: Dynamic Tools Support */
    char *cached_tools_json; /* The JSON array of tools string [{},{}] */
    int cached_tools_count;
} mcp_source_t;

static mcp_source_t s_sources[MAX_SOURCES];
static int s_source_id_counter = 1;
static bool s_mcp_started = false;

/* Forward decls */
static void save_config(void);
static void mcp_source_clear_tools(mcp_source_t *src);

/* ── Tool Provider Implementation ────────────────────────────────── */

static char *mcp_provider_get_tools_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    
    for (int i = 0; i < MAX_SOURCES; i++) {
        mcp_source_t *src = &s_sources[i];
        if (src->id != 0 && src->client && src->cached_tools_json) {
            cJSON *src_tools = cJSON_Parse(src->cached_tools_json);
            if (src_tools) {
                if (cJSON_IsArray(src_tools)) {
                     cJSON *item = NULL;
                     cJSON_ArrayForEach(item, src_tools) {
                         cJSON_AddItemToArray(arr, cJSON_Duplicate(item, true));
                     }
                }
                cJSON_Delete(src_tools);
            }
        }
    }
    
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return json;
}

typedef struct {
    SemaphoreHandle_t sema;
    char *output_buf;
    size_t output_len;
} tool_exec_ctx_t;

static void on_tool_call_result(void *ctx, int id, const char *json_result, esp_err_t err)
{
    tool_exec_ctx_t *tctx = (tool_exec_ctx_t *)ctx;
    if (err == ESP_OK && json_result) {
        cJSON *root = cJSON_Parse(json_result);
        if (root) {
            cJSON *content = cJSON_GetObjectItem(root, "content");
            char *s = cJSON_PrintUnformatted(content ? content : root);
            if (s) {
                snprintf(tctx->output_buf, tctx->output_len, "%s", s);
                free(s);
            }
            cJSON_Delete(root);
        } else {
             snprintf(tctx->output_buf, tctx->output_len, "{}");
        }
    } else {
        snprintf(tctx->output_buf, tctx->output_len, "{\"error\":\"RPC Error or Timeout\"}");
    }
    xSemaphoreGive(tctx->sema);
}

static esp_err_t mcp_provider_execute_tool(const char *tool_name, const char *input_json, char *output, size_t output_size)
{
    /* Find which source has this tool */
    mcp_source_t *target_src = NULL;

    for (int i = 0; i < MAX_SOURCES; i++) {
        mcp_source_t *src = &s_sources[i];
        if (src->id != 0 && src->client && src->cached_tools_json) {
            /* Quick check if tool is in this source's cache */
            /* We have to parse to be sure, or string search? String search might match partial args */
            /* Let's parse. Optimization: keep a list of names? For now parse. Phase 10 MVP */
            cJSON *tools = cJSON_Parse(src->cached_tools_json);
            if (tools) {
                bool found = false;
                cJSON *item = NULL;
                cJSON_ArrayForEach(item, tools) {
                    cJSON *nm = cJSON_GetObjectItem(item, "name");
                    if (nm && cJSON_IsString(nm) && strcmp(nm->valuestring, tool_name) == 0) {
                        found = true;
                        break;
                    }
                }
                cJSON_Delete(tools);
                if (found) {
                    target_src = src;
                    break;
                }
            }
        }
    }

    if (!target_src) {
        return ESP_ERR_NOT_FOUND;
    }

    /* Execute on target source */
    tool_exec_ctx_t tctx = {
        .sema = xSemaphoreCreateBinary(),
        .output_buf = output,
        .output_len = output_size
    };
    
    cJSON *args = cJSON_Parse(input_json);
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "name", tool_name);
    if (args) cJSON_AddItemToObject(params, "arguments", args);
    char *params_str = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    
    ESP_LOGI(TAG, "Calling tool '%s' on source '%s'", tool_name, target_src->name);
    esp_err_t err = mcp_client_send_request(target_src->client, "tools/call", params_str, on_tool_call_result, &tctx);
    free(params_str);
    
    if (err == ESP_OK) {
        if (xSemaphoreTake(tctx.sema, pdMS_TO_TICKS(15000)) != pdTRUE) { // 15s timeout
             snprintf(output, output_size, "{\"error\":\"Timeout waiting for tool response\"}");
        }
    } else {
        snprintf(output, output_size, "{\"error\":\"Failed to send request\"}");
    }
    
    vSemaphoreDelete(tctx.sema);
    return ESP_OK;
}

static const tool_provider_t s_mcp_provider = {
    .name = "mcp",
    .get_tools_json = mcp_provider_get_tools_json,
    .execute_tool = mcp_provider_execute_tool
};

/* ── Tool Registration Logic ─────────────────────────────────────── */

static void mcp_source_clear_tools(mcp_source_t *src)
{
    if (src->cached_tools_json) {
        free(src->cached_tools_json);
        src->cached_tools_json = NULL;
    }
    src->cached_tools_count = 0;
}

static void handle_tools_list_response(mcp_source_t *src, const char *json_result)
{
    cJSON *root = cJSON_Parse(json_result);
    if (!root) return;
    
    cJSON *tools = cJSON_GetObjectItem(root, "tools");
    if (!cJSON_IsArray(tools)) {
        cJSON_Delete(root);
        return;
    }

    /* Update Cache */
    mcp_source_clear_tools(src);
    src->cached_tools_json = cJSON_PrintUnformatted(tools);
    src->cached_tools_count = cJSON_GetArraySize(tools);

    ESP_LOGI(TAG, "Cached %d tools from %s", src->cached_tools_count, src->name);
    
    tool_registry_rebuild_json();
    cJSON_Delete(root);
}

/* ── Callbacks ───────────────────────────────────────────────────── */

static void on_message(mcp_client_t *client, const char *json, size_t len)
{
    // Notifications (no ID) go here.
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
    mcp_client_send_request(client, "tools/list", NULL, on_tools_list, src);
}

static void on_disconnect(mcp_client_t *client)
{
    mcp_source_t *src = (mcp_source_t *)mcp_client_get_ctx(client);
    ESP_LOGI(TAG, "Source %s disconnected, clearing tools", src->name);
    mcp_source_clear_tools(src);
    tool_registry_rebuild_json();
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
    src->cached_tools_json = NULL;
    src->cached_tools_count = 0;

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
    /* Register MCP Provider during init */
    tool_registry_register_provider(&s_mcp_provider);
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
            /* Clear config */
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
            cJSON_AddNumberToObject(item, "tools_count", s_sources[i].cached_tools_count);
            
            cJSON_AddItemToArray(arr, item);
        }
    }
    
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

char *mcp_manager_get_status_json(void)
{
    cJSON *root = cJSON_CreateObject();
    int connected = 0;
    int total = 0;
    int tools = 0;

    for (int i = 0; i < MAX_SOURCES; i++) {
        if (s_sources[i].id != 0) {
            total++;
            if (s_sources[i].client) connected++;
            tools += s_sources[i].cached_tools_count;
        }
    }

    cJSON_AddNumberToObject(root, "connected", connected);
    cJSON_AddNumberToObject(root, "total", total);
    cJSON_AddNumberToObject(root, "tools", tools);

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
            on_disconnect(NULL); /* Cleanup */
        }
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}
