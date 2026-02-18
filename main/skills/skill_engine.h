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
esp_err_t skill_engine_install_from_market(const char *url,
                                           const char *checksum_hex,
                                           const char *source_id,
                                           const char *source_version);
esp_err_t skill_engine_upgrade_from_market_offer(const char *url,
                                                 const char *checksum_hex,
                                                 const char *source_id,
                                                 const char *source_version);

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
void skill_engine_install_history_clear(void);
char *skill_engine_check_updates_json(const char *offers_json);
char *skill_engine_framework_status_json(void);

/**
 * Number of active skill slots.
 */
int skill_engine_get_count(void);
