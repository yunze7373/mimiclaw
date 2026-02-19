#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * API Skills — no-code JSON-defined REST API integrations.
 *
 * A skill with type:"api" in its manifest.json is loaded as an API skill.
 * Instead of Lua code, it uses a JSON config defining:
 *   - base_url: API root URL
 *   - auth: { type: "bearer"|"api_key"|"basic", token/key/user/pass }
 *   - endpoints[]: Each becomes an agent tool
 *
 * Example manifest.json:
 * {
 *   "name": "weather_api",
 *   "type": "api",
 *   "version": "1.0",
 *   "config": {
 *     "base_url": "https://api.weather.com/v1",
 *     "auth": { "type": "api_key", "key": "xxx", "header": "X-API-Key" },
 *     "endpoints": [
 *       {
 *         "name": "get_weather",
 *         "description": "Get current weather for a city",
 *         "method": "GET",
 *         "path": "/weather",
 *         "params": { "city": {"type":"string","required":true} }
 *       }
 *     ]
 *   }
 * }
 */

/**
 * Load and register an API skill from its manifest.json config.
 * Parses endpoints and registers each as a tool in the tool registry.
 *
 * @param name       Skill name
 * @param config_json  JSON string of the "config" object from manifest
 * @return ESP_OK on success
 */
esp_err_t api_skill_load(const char *name, const char *config_json);

/**
 * Unload an API skill — unregisters its tools.
 *
 * @param name  Skill name
 * @return ESP_OK on success
 */
esp_err_t api_skill_unload(const char *name);

/**
 * Check if a skill name corresponds to a loaded API skill.
 */
bool api_skill_is_loaded(const char *name);
