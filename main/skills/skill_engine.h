#pragma once

#include "esp_err.h"

/**
 * Initialize single-VM Lua skill runtime and load bundles from /spiffs/skills.
 */
esp_err_t skill_engine_init(void);

/**
 * Install skill bundle from URL (placeholder API; currently supports direct .lua download).
 */
esp_err_t skill_engine_install(const char *url);
esp_err_t skill_engine_install_with_checksum(const char *url, const char *checksum_hex);

/**
 * Uninstall a skill by name.
 */
esp_err_t skill_engine_uninstall(const char *name);

/**
 * Return installed skill metadata as JSON string.
 * Caller must free().
 */
char *skill_engine_list_json(void);
char *skill_engine_install_status_json(void);
char *skill_engine_install_capabilities_json(void);
char *skill_engine_install_history_json(void);

/**
 * Number of active skill slots.
 */
int skill_engine_get_count(void);
