#pragma once

#include <stdbool.h>
#include "esp_err.h"

/**
 * Initialize async callback runtime (timer queue/worker).
 */
esp_err_t skill_runtime_init(void);

/**
 * Register a periodic/oneshot timer callback.
 * lua_cb_ref must be a valid Lua registry reference.
 * Returns timer id via out_timer_id.
 */
esp_err_t skill_runtime_register_timer(int skill_id, int period_ms, bool periodic, int lua_cb_ref, int *out_timer_id);

/**
 * Cancel a timer by id. Safe to call multiple times.
 */
esp_err_t skill_runtime_cancel_timer(int timer_id);

/**
 * Cancel and cleanup all async resources owned by a skill.
 */
void skill_runtime_release_skill(int skill_id);

