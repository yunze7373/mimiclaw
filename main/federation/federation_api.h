#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * Register Federation API endpoints.
 * - GET /api/federation/peers
 * - POST /api/federation/command
 */
void federation_api_register(httpd_handle_t server);

/**
 * Broadcast a JSON command to all peers in the same group.
 * @param command_name  Name of the command (e.g. "install_skill")
 * @param args          JSON object with arguments
 * @return              ESP_OK if broadcast initiated (async)
 */
esp_err_t federation_broadcast_command(const char *command_name, const char *args_json);
