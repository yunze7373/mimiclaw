#pragma once

#include "esp_err.h"

/**
 * Initialize SSDP service.
 */
esp_err_t ssdp_service_init(void);

/**
 * Start SSDP service (starts UDP listener task).
 */
esp_err_t ssdp_service_start(void);

