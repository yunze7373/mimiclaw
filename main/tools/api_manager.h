#pragma once

#include "esp_err.h"

/**
 * Initialize the API Manager.
 * Loads /spiffs/config/api_skills.json and registers as a tool provider.
 */
esp_err_t api_manager_init(void);
