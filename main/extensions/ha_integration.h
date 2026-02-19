#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * Initialize the Home Assistant Integration.
 * Registers HTTP API endpoints for HA discovery and control.
 */
esp_err_t ha_integration_init(void);

/**
 * Start the HA Integration service.
 */
esp_err_t ha_integration_start(void);
