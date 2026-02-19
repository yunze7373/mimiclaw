#include "agent/mcp_client.h"
#include "mimi_config.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include "cJSON.h"

#if CONFIG_MIMI_ENABLE_MCP && __has_include("esp_websocket_client.h")
#define MIMI_MCP_IMPL_ENABLED 1
#include "esp_websocket_client.h"
#else
#define MIMI_MCP_IMPL_ENABLED 0
#endif

static const char *TAG = "mcp_client";

#if MIMI_MCP_IMPL_ENABLED

typedef struct mcp_pending_req_t {
    int id;
    mcp_result_cb_t cb;
    void *ctx;
    struct mcp_pending_req_t *next;
} mcp_pending_req_t;

struct mcp_client_t {
    mcp_client_config_t config;
    esp_websocket_client_handle_t ws_handle;
    bool connected;
    int next_id;
    mcp_pending_req_t *pending_reqs;
};

static void mcp_handle_response(mcp_client_t *client, int id, cJSON *root)
{
    mcp_pending_req_t *prev = NULL;
    mcp_pending_req_t *curr = client->pending_reqs;

    while (curr) {
        if (curr->id == id) {
            /* Found request */
            if (prev) prev->next = curr->next;
            else client->pending_reqs = curr->next;
            
            cJSON *result = cJSON_GetObjectItem(root, "result");
            cJSON *error = cJSON_GetObjectItem(root, "error");
            
            /* Construct response string subset for callback */
            char *res_str = NULL;
            if (error) res_str = cJSON_PrintUnformatted(error);
            else if (result) res_str = cJSON_PrintUnformatted(result);
            else res_str = strdup("{}");

            if (curr->cb) {
                curr->cb(curr->ctx, id, res_str ? res_str : "{}", error ? ESP_FAIL : ESP_OK);
            }
            
            if (res_str) free(res_str);
            free(curr);
            return;
        }
        prev = curr;
        curr = curr->next;
    }
    ESP_LOGW(TAG, "Response for unknown ID: %d", id);
}

static void ws_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    mcp_client_t *client = (mcp_client_t *)handler_args;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Connected to %s", client->config.url);
            client->connected = true;
            if (client->config.on_connect) {
                client->config.on_connect(client);
            }
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "Disconnected from %s", client->config.url);
            client->connected = false;
            /* Clear pending requests with error */
            while (client->pending_reqs) {
                mcp_pending_req_t *req = client->pending_reqs;
                client->pending_reqs = req->next;
                if (req->cb) req->cb(req->ctx, req->id, NULL, ESP_FAIL);
                free(req);
            }
            if (client->config.on_disconnect) {
                client->config.on_disconnect(client);
            }
            break;
        case WEBSOCKET_EVENT_DATA:
            if (data->op_code == WS_TRANSPORT_OPCODES_TEXT) {
                 /* 1. Try parsing as JSON-RPC response */
                 char *text = strndup(data->data_ptr, data->data_len);
                 if (text) {
                     cJSON *root = cJSON_Parse(text);
                     if (root) {
                         cJSON *id_item = cJSON_GetObjectItem(root, "id");
                         if (id_item && cJSON_IsNumber(id_item)) {
                             mcp_handle_response(client, id_item->valueint, root);
                         } else {
                             /* Notification or Request from Server? */
                             /* For now, forward to raw handler */
                             if (client->config.on_message) {
                                 client->config.on_message(client, text, strlen(text));
                             }
                         }
                         cJSON_Delete(root);
                     } else {
                         ESP_LOGW(TAG, "Failed to parse JSON: %s", text);
                     }
                     free(text);
                 }
            }
            break;
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket Error on %s", client->config.url);
            break;
    }
}

mcp_client_t *mcp_client_create(const mcp_client_config_t *config)
{
    if (!config || !config->url[0]) return NULL;

    mcp_client_t *client = calloc(1, sizeof(mcp_client_t));
    if (!client) return NULL;

    client->config = *config;
    client->next_id = 1;
    
    /* Config WS Client */
    esp_websocket_client_config_t ws_cfg = {
        .uri = client->config.url,
        .disable_auto_reconnect = false,
        .reconnect_timeout_ms = MIMI_MCP_RECONNECT_MS,
        .network_timeout_ms = 5000,
    };

    client->ws_handle = esp_websocket_client_init(&ws_cfg);
    if (!client->ws_handle) {
        free(client);
        return NULL;
    }

    esp_websocket_register_events(client->ws_handle, WEBSOCKET_EVENT_ANY, ws_event_handler, (void *)client);
    
    return client;
}

void mcp_client_destroy(mcp_client_t *client)
{
    if (!client) return;
    if (client->ws_handle) {
        esp_websocket_client_stop(client->ws_handle);
        esp_websocket_client_destroy(client->ws_handle);
    }
    /* Free pending */
    while (client->pending_reqs) {
        mcp_pending_req_t *req = client->pending_reqs;
        client->pending_reqs = req->next;
        free(req);
    }
    free(client);
}

esp_err_t mcp_client_connect(mcp_client_t *client)
{
    if (!client || !client->ws_handle) return ESP_FAIL;
    return esp_websocket_client_start(client->ws_handle);
}

esp_err_t mcp_client_disconnect(mcp_client_t *client)
{
    if (!client || !client->ws_handle) return ESP_FAIL;
    return esp_websocket_client_stop(client->ws_handle);
}

bool mcp_client_is_connected(mcp_client_t *client)
{
    return client && client->connected;
}

esp_err_t mcp_client_send(mcp_client_t *client, const char *json_data)
{
    if (!client || !client->ws_handle || !client->connected) return ESP_FAIL;
    int len = strlen(json_data);
    return esp_websocket_client_send_text(client->ws_handle, json_data, len, pdMS_TO_TICKS(1000) > 0 ? len : -1);
}

esp_err_t mcp_client_send_request(mcp_client_t *client, const char *method, const char *params, mcp_result_cb_t cb, void *ctx)
{
    if (!client || !client->connected) return ESP_FAIL;

    int id = client->next_id++;
    
    /* Create Pending Req */
    mcp_pending_req_t *req = calloc(1, sizeof(mcp_pending_req_t));
    if (!req) return ESP_ERR_NO_MEM;
    
    req->id = id;
    req->cb = cb;
    req->ctx = ctx;
    
    /* Prepend to list */
    req->next = client->pending_reqs;
    client->pending_reqs = req;

    /* Build JSON */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(root, "id", id);
    cJSON_AddStringToObject(root, "method", method);
    if (params) {
        cJSON *p_obj = cJSON_Parse(params);
        if (p_obj) cJSON_AddItemToObject(root, "params", p_obj);
        else cJSON_AddStringToObject(root, "params", params); // Fallback string?
    }
    
    char *json_str = cJSON_PrintUnformatted(root);
    esp_err_t err = mcp_client_send(client, json_str);
    
    free(json_str);
    cJSON_Delete(root);
    
    if (err != ESP_OK) {
        /* Remove from list if send failed */
        if (client->pending_reqs == req) {
            client->pending_reqs = req->next;
        } else {
            /* This simple removal logic assumes single thread or no race during immediate fail.
               Realistically, need lock. For now, on ESP32 single core task, acceptable. */
        }
        free(req);
    }
    
    return err;
}

esp_err_t mcp_client_send_notification(mcp_client_t *client, const char *method, const char *params)
{
     if (!client || !client->connected) return ESP_FAIL;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddStringToObject(root, "method", method);
    if (params) {
        cJSON *p_obj = cJSON_Parse(params);
        if (p_obj) cJSON_AddItemToObject(root, "params", p_obj);
    }
    
    char *json_str = cJSON_PrintUnformatted(root);
    esp_err_t err = mcp_client_send(client, json_str);
    
    free(json_str);
    cJSON_Delete(root);
    return err;   
}

void *mcp_client_get_ctx(mcp_client_t *client)
{
    return client ? client->config.user_ctx : NULL;
}

#else

/* Dummy impl */
struct mcp_client_t { int dummy; };
mcp_client_t *mcp_client_create(const mcp_client_config_t *config) { return NULL; }
void mcp_client_destroy(mcp_client_t *client) {}
esp_err_t mcp_client_connect(mcp_client_t *client) { return ESP_FAIL; }
esp_err_t mcp_client_disconnect(mcp_client_t *client) { return ESP_FAIL; }
bool mcp_client_is_connected(mcp_client_t *client) { return false; }
esp_err_t mcp_client_send(mcp_client_t *client, const char *json_data) { return ESP_FAIL; }
void *mcp_client_get_ctx(mcp_client_t *client) { return NULL; }
esp_err_t mcp_client_send_request(mcp_client_t *client, const char *method, const char *params, mcp_result_cb_t cb, void *ctx) { return ESP_FAIL; }
esp_err_t mcp_client_send_notification(mcp_client_t *client, const char *method, const char *params) { return ESP_FAIL; }

#endif
