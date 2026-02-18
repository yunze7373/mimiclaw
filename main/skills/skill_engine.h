#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Initialize the skill engine.
 * Scans /spiffs/skills/ for *.lua files and loads each into a skill slot.
 * Each skill's TOOLS are registered into tool_registry.
 *
 * Call AFTER tool_registry_init() and AFTER SPIFFS is mounted.
 */
esp_err_t skill_engine_init(void);

/**
 * Install a skill from a URL.
 * Downloads the .lua file to /spiffs/skills/ and hot-loads it.
 *
 * @param url   URL of the .lua skill file
 * @return ESP_OK on success
 */
esp_err_t skill_engine_install(const char *url);

/**
 * Uninstall a skill by name.
 * Removes the .lua file and unloads the slot.
 *
 * @param name  Skill name (matches SKILL.name in the .lua)
 * @return ESP_OK on success
 */
esp_err_t skill_engine_uninstall(const char *name);

/**
 * Get JSON array of installed skills.
 * Caller must free() the returned string.
 *
 * @return JSON string like [{"name":"bme280","version":"1.0","tools":2}, ...]
 */
char *skill_engine_list_json(void);

/**
 * Get the number of loaded skill slots.
 */
int skill_engine_get_count(void);
