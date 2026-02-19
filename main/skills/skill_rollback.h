#pragma once

#include "esp_err.h"

/**
 * Backup a skill's current files before overwriting.
 * Copies main.lua + manifest.json to .rollback/<name>/
 *
 * @param name  Skill name (directory name under /spiffs/skills/)
 * @return ESP_OK on success
 */
esp_err_t skill_rollback_backup(const char *name);

/**
 * Restore a skill from its backup (.rollback/<name>/).
 * Copies backed-up files back and re-initializes the skill engine.
 *
 * @param name  Skill name
 * @return ESP_OK on success
 */
esp_err_t skill_rollback_restore(const char *name);

/**
 * Check if a rollback backup exists for the given skill.
 *
 * @param name  Skill name
 * @return true if backup exists
 */
bool skill_rollback_exists(const char *name);

/**
 * List all skills that have rollback backups.
 * @return JSON string (caller must free), e.g. ["skill_a","skill_b"]
 */
char *skill_rollback_list_json(void);
