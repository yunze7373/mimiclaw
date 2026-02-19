#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * Start the Web UI HTTP server.
 */
esp_err_t web_ui_init(void);

/**
 * Stop the Web UI HTTP server.
 */
esp_err_t web_ui_stop(void);

/**
 * Get current Web UI HTTP server handle.
 */
httpd_handle_t web_ui_get_server(void);
