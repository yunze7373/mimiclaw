#pragma once

#include "esp_err.h"

/* Initialize network tools (BLE stack, etc.) */
esp_err_t tool_network_init(void);

/* Tool function prototypes for LLM usage */
esp_err_t tool_wifi_scan(const char *input, char *output, size_t out_len);
esp_err_t tool_wifi_status(const char *input, char *output, size_t out_len);
esp_err_t tool_ble_scan(const char *input, char *output, size_t out_len);
