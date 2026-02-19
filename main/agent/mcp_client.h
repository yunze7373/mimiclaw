#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct mcp_client_t mcp_client_t;

typedef struct {
    char url[256];
    char transport[16]; /* "websocket" only for now */
    
    /* Callbacks */
    void (*on_connect)(mcp_client_t *client);
    void (*on_disconnect)(mcp_client_t *client);
    void (*on_message)(mcp_client_t *client, const char *json, size_t len);
    
    void *user_ctx;
} mcp_client_config_t;

/**
 * Create a new MCP Client instance.
 * Does not connect immediately; call mcp_client_connect().
 */
mcp_client_t *mcp_client_create(const mcp_client_config_t *config);

/**
 * Destroy client and free resources.
 */
void mcp_client_destroy(mcp_client_t *client);

/**
 * Start connection (async).
 */
esp_err_t mcp_client_connect(mcp_client_t *client);

/**
 * Close connection.
 */
esp_err_t mcp_client_disconnect(mcp_client_t *client);

/**
 * Check connection state.
 */
bool mcp_client_is_connected(mcp_client_t *client);

/**
 * Send JSON message.
 */
esp_err_t mcp_client_send(mcp_client_t *client, const char *json_data);

/**
 * Get user context.
 */
void *mcp_client_get_ctx(mcp_client_t *client);
