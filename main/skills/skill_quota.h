#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/* ── Per-Skill Quota Management ─────────────────────────────────────── */

/* Default limits */
#define SKILL_QUOTA_DEFAULT_DISK_LIMIT       (64 * 1024)   /* 64KB per skill */
#define SKILL_QUOTA_DEFAULT_HEAP_LIMIT       (16 * 1024)   /* 16KB Lua heap per skill */
#define SKILL_QUOTA_DEFAULT_INSTR_LIMIT      100000        /* 100K instructions per call */
#define SKILL_QUOTA_MAX_DISK_LIMIT           (256 * 1024)  /* 256KB max per skill */
#define SKILL_QUOTA_MAX_HEAP_LIMIT           (32 * 1024)   /* 32KB max heap per skill */
#define SKILL_QUOTA_MAX_INSTR_LIMIT          500000        /* 500K max instructions */
#define SKILL_QUOTA_TOTAL_DISK_LIMIT         (256 * 1024)  /* 256KB total for all skills */
#define SKILL_QUOTA_FILE                     "/spiffs/skills/.quota.json"

typedef struct {
    char name[32];
    int32_t disk_limit;
    int32_t disk_used;
    int32_t heap_limit;
    int32_t heap_peak;
    int32_t instr_limit;
    int32_t instr_last;
} skill_quota_entry_t;

#define SKILL_QUOTA_MAX_ENTRIES  8

/**
 * Initialize the quota system.
 * Loads existing quota data from SPIFFS, or creates defaults.
 */
esp_err_t skill_quota_init(void);

/**
 * Check if a skill installation fits within disk quota.
 * @param skill_name  Name of the skill to install
 * @param required_bytes  Bytes needed for the skill package
 * @return ESP_OK if fits, ESP_ERR_NO_MEM if quota exceeded
 */
esp_err_t skill_quota_check_disk(const char *skill_name, int32_t required_bytes);

/**
 * Update disk usage tracking after install/uninstall.
 * @param skill_name  Name of the skill
 * @param bytes_used  Current disk usage (0 to clear on uninstall)
 */
void skill_quota_track_disk(const char *skill_name, int32_t bytes_used);

/**
 * Get the instruction limit for a given skill.
 * @param skill_name  Skill name to look up
 * @return instruction limit, or default if not configured
 */
int32_t skill_quota_get_instr_limit(const char *skill_name);

/**
 * Get the heap limit for a given skill.
 * @param skill_name  Skill name to look up
 * @return heap limit in bytes, or default if not configured
 */
int32_t skill_quota_get_heap_limit(const char *skill_name);

/**
 * Update peak heap usage for a skill (for tracking/reporting).
 * @param skill_name  Skill name
 * @param heap_used   Current heap usage in bytes
 */
void skill_quota_update_heap_peak(const char *skill_name, int32_t heap_used);

/**
 * Update last instruction count for a skill (for tracking/reporting).
 * @param skill_name  Skill name
 * @param instr_used  Instructions used in last call
 */
void skill_quota_update_instr(const char *skill_name, int32_t instr_used);

/**
 * Set custom limits for a skill (overrides defaults).
 * Limits are clamped to MAX values.
 * @param skill_name  Skill name
 * @param disk_limit  Disk limit in bytes (0 = use default)
 * @param heap_limit  Heap limit in bytes (0 = use default)
 * @param instr_limit Instruction limit (0 = use default)
 */
esp_err_t skill_quota_set_limits(const char *skill_name, int32_t disk_limit,
                                 int32_t heap_limit, int32_t instr_limit);

/**
 * Remove quota entry for a skill (on uninstall).
 */
void skill_quota_remove(const char *skill_name);

/**
 * Persist current quota state to SPIFFS.
 */
esp_err_t skill_quota_save(void);

/**
 * Get the full quota entry for a skill (for Web UI / reporting).
 * @param skill_name  Skill name
 * @return pointer to entry (NULL if not found)
 */
const skill_quota_entry_t *skill_quota_get(const char *skill_name);

/**
 * Calculate directory size recursively.
 * @param path  Directory path
 * @return total size in bytes, or -1 on error
 */
int32_t skill_quota_calc_dir_size(const char *path);
