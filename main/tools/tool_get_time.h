#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Execute get_current_time tool.
 * Fetches current time via HTTP Date header, sets system clock, returns time string.
 */
esp_err_t tool_get_time_execute(const char *input_json, char *output, size_t output_size);

/* Set system timezone (input: {"timezone": "CST-8"}) */
esp_err_t tool_set_timezone_execute(const char *input_json, char *output, size_t output_size);

/* Init timezone from NVS */
void tool_time_init(void);
