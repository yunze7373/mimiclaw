#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Create a new skill from Agent-generated Lua code.
 * Writes main.lua + manifest.json, syntax-checks, and hot-loads.
 *
 * Input JSON: {
 *   "name": "my_sensor",
 *   "description": "Reads temperature",
 *   "category": "hardware",
 *   "type": "sensor",
 *   "bus": "i2c",
 *   "code": "SKILL = { ... }\nTOOLS = { ... }"
 * }
 */
esp_err_t tool_skill_create_execute(const char *input_json, char *output, size_t output_size);

/**
 * List available skill templates with descriptions.
 * Input JSON: {} (no parameters)
 */
esp_err_t tool_skill_list_templates_execute(const char *input_json, char *output, size_t output_size);

/**
 * Get the Lua source code for a specific skill template.
 * Input JSON: { "name": "i2c_sensor" }
 */
esp_err_t tool_skill_get_template_execute(const char *input_json, char *output, size_t output_size);
