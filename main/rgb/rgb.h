#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t rgb_init(void);
void rgb_set(uint8_t r, uint8_t g, uint8_t b);

/* Breathing effect controls */
void rgb_start_breathing(uint8_t r, uint8_t g, uint8_t b, uint32_t period_ms);
void rgb_stop_breathing(void);

#ifdef __cplusplus
}
#endif
