#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Execute skill management actions (list, delete, reload).
 *
 * Input JSON:
 * {
 *   "action": "list" | "delete" | "reload",
 *   "name": "skill_name" (required for delete)
 * }
 */
esp_err_t tool_skill_manage_execute(const char *input_json, char *output, size_t output_size);
