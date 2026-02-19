#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * Per-skill API rate limiter using token bucket algorithm.
 * Limits the rate of hardware API calls (GPIO, I2C, HTTP, etc.)
 * to prevent any single skill from monopolizing resources.
 */

typedef enum {
    RATE_LIMIT_GPIO = 0,      /* GPIO read/write ops */
    RATE_LIMIT_I2C,           /* I2C transactions */
    RATE_LIMIT_HTTP,          /* HTTP requests */
    RATE_LIMIT_UART,          /* UART sends */
    RATE_LIMIT_MAX
} rate_limit_type_t;

/**
 * Initialize rate limiter for a skill slot.
 * Must be called when a skill is loaded.
 */
void skill_rate_limit_init(int skill_id);

/**
 * Check and consume a rate-limited operation.
 * @param skill_id  Skill slot index
 * @param type      Operation type to rate limit
 * @return true if allowed, false if rate exceeded
 */
bool skill_rate_limit_check(int skill_id, rate_limit_type_t type);

/**
 * Reset rate limiter for a skill (e.g., on unload).
 */
void skill_rate_limit_reset(int skill_id);
