#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/* Initialize hardware tools (if any hardware needs init) */
esp_err_t tool_hardware_init(void);

/* Register Web API handlers (/api/hardware/...) */
void tool_hardware_register_handlers(httpd_handle_t server);

/* Tool function prototypes for LLM usage */
esp_err_t tool_system_status(const char *input, char *output, size_t out_len);
esp_err_t tool_gpio_control(const char *input, char *output, size_t out_len);
esp_err_t tool_i2c_scan(const char *input, char *output, size_t out_len);
