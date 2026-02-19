#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * Initialize the MCP (Model Context Protocol) Client.
 * Starts the WebSocket connection to the configured MCP server.
 */
esp_err_t mcp_client_init(void);

/**
 * Check if connected to MCP server.
 */
bool mcp_client_is_connected(void);

/**
 * Send a raw JSON-RPC message (internal use mostly).
 */
esp_err_t mcp_client_send(const char *json_data);
