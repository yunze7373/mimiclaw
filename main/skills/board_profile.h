#pragma once

#include <stdbool.h>
#include "esp_err.h"

/**
 * Load board profile from SPIFFS, fallback to defaults when missing.
 */
esp_err_t board_profile_init(void);

/**
 * Resolve named I2C bus to pin/frequency.
 */
bool board_profile_get_i2c(const char *bus, int *sda, int *scl, int *freq_hz);

/**
 * Resolve GPIO alias (e.g. "LED") to real pin number.
 */
bool board_profile_resolve_gpio(const char *name, int *pin);

/**
 * Check whether a pin is board-reserved.
 */
bool board_profile_is_gpio_reserved(int pin);

/**
 * Board id from profile, or "default".
 */
const char *board_profile_get_id(void);

