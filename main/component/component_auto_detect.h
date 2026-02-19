#pragma once

#include <stdbool.h>

/**
 * Run hardware auto-detection logic.
 * This should be called after loading configuration but before component initialization.
 * It checks hardware capabilities (PSRAM, Display, etc.) and enables/disables components accordingly.
 */
void comp_auto_detect_apply(void);

/**
 * Check if auto-detection is enabled in configuration.
 */
bool comp_auto_detect_is_enabled(void);
