#pragma once

#include "esp_err.h"

/**
 * Initialize MCP Manager.
 * Loads configuration from /spiffs/config/mcp_sources.json and auto-connects enabled sources.
 */
esp_err_t mcp_manager_init(void);

/**
 * Add a new MCP source.
 * @param name Display name
 * @param transport Protocol (currently only "websocket")
 * @param url Connection URL
 * @param auto_connect if true, connect immediately and on boot
 * @return Source ID (int) or negative error
 */
int mcp_manager_add_source(const char *name, const char *transport, const char *url, bool auto_connect);

/**
 * Remove an MCP source by ID.
 * Disconnects if active.
 */
esp_err_t mcp_manager_remove_source(int id);

/**
 * Get JSON string of all configured sources.
 * Caller must free.
 */
char *mcp_manager_get_sources_json(void);

/**
 * Connect/Disconnect/Action on a source.
 * action: "connect", "disconnect"
 */
esp_err_t mcp_manager_source_action(int id, const char *action);
