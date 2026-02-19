#include "agent/mcp_client.h"
#include "mimi_config.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

#if CONFIG_MIMI_ENABLE_MCP && __has_include("esp_websocket_client.h")
#define MIMI_MCP_IMPL_ENABLED 1
#include "esp_websocket_client.h"
#else
#define MIMI_MCP_IMPL_ENABLED 0
#endif

static const char *TAG = "mcp_client";

#if MIMI_MCP_IMPL_ENABLED

struct mcp_client_t {
    mcp_client_config_t config;
    esp_websocket_client_handle_t ws_handle;
    bool connected;
};

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
            if (client->config.on_disconnect) {
                client->config.on_disconnect(client);
            }
            break;
        case WEBSOCKET_EVENT_DATA:
            if (data->op_code == WS_TRANSPORT_OPCODES_TEXT && client->config.on_message) {
                client->config.on_message(client, data->data_ptr, data->data_len);
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
    
    /* Config WS Client */
    esp_websocket_client_config_t ws_cfg = {
        .uri = client->config.url,
        .disable_auto_reconnect = false,
        .reconnect_timeout_ms = 5000,
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
    return esp_websocket_client_send_text(client->ws_handle, json_data, len, pdMS_TO_TICKS(1000) > 0 ? len : -1); // Bug fix: send_text rets len on success, but here checking err code
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

#endif
