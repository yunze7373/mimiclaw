#pragma once

#include "esp_err.h"

/**
 * Initialize resource manager lock tables.
 */
esp_err_t skill_resmgr_init(void);

/**
 * Acquire a GPIO pin for a skill.
 */
esp_err_t skill_resmgr_acquire_gpio(int skill_id, int pin);

/**
 * Acquire I2C bus with strict frequency policy.
 */
esp_err_t skill_resmgr_acquire_i2c(int skill_id, const char *bus, int freq_hz);

/**
 * Release all resources owned by a skill.
 */
void skill_resmgr_release_all(int skill_id);

