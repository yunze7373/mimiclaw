#pragma once

#include "esp_err.h"

/**
 * Backup the current version of a skill before overwriting.
 * @param skill_name Name of the skill to backup
 * @return ESP_OK on success
 */
esp_err_t skill_rollback_backup(const char *skill_name);

/**
 * Restore a skill from a specific backup version.
 * @param skill_name Name of the skill
 * @param version Version identifier (timestamp or hash) to restore
 * @return ESP_OK on success
 */
esp_err_t skill_rollback_restore(const char *skill_name, const char *version);

/**
 * List available backups for a skill as JSON.
 * Caller must free the returned string.
 * @param skill_name Name of the skill
 * @return JSON string or NULL on error
 */
char *skill_rollback_list_json(const char *skill_name);
