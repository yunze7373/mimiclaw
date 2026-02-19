#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include "skills/skill_types.h"

#define SKILL_MAX_SCHEMA_JSON 512

typedef struct {
    bool used;
    skill_state_t state;
    char name[32];
    char version[16];
    char author[32];
    char description[128];
    char root_dir[128];
    char entry[64];

    skill_permissions_t permissions;
    skill_category_t category;
    skill_type_t skill_type;
    skill_bus_t bus;
    int env_ref;

    int tool_count;
    char tool_names[SKILL_MAX_TOOLS_PER_SKILL][32];
    char tool_descs[SKILL_MAX_TOOLS_PER_SKILL][128];
    char tool_schema[SKILL_MAX_TOOLS_PER_SKILL][SKILL_MAX_SCHEMA_JSON];
    int tool_handler_ref[SKILL_MAX_TOOLS_PER_SKILL];

    int event_count;
    char event_names[SKILL_MAX_EVENTS_PER_SKILL][32];

    bool req_i2c_enabled;
    char req_i2c_bus[16];
    int req_i2c_min_freq_hz;
    int req_i2c_max_freq_hz;
} skill_slot_t;

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
 * Install skill with Ed25519 signature verification.
 * @param url Skill bundle URL
 * @param checksum_hex Optional SHA256 hex string (can be NULL)
 * @param signature_hex Ed25519 signature hex string (64 chars)
 * @return ESP_OK on success
 */
esp_err_t skill_engine_install_with_signature(const char *url, const char *checksum_hex, const char *signature_hex);

/**
 * Set trusted Ed25519 public key for signature verification.
 * @param public_key_hex 64-character hex-encoded public key
 * @return ESP_OK on success
 */
esp_err_t skill_engine_set_trusted_key(const char *public_key_hex);

/**
 * Get currently configured trusted public key (for UI display).
 * Caller must free() the returned string.
 * @return Hex string or NULL if no key set
 */
char *skill_engine_get_trusted_key(void);

/**
 * Clear trusted public key.
 */
esp_err_t skill_engine_clear_trusted_key(void);

/**
 * Check if signature verification is enabled.
 */
bool skill_engine_signature_verification_enabled(void);

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

/**
 * Number of active skill slots.
 */
int skill_engine_get_count(void);

/**
 * Get pointer to skill slot by index.
 * Returns NULL if index invalid or slot unused.
 * READ-ONLY access intended for inspections/UIs.
 */
const skill_slot_t *skill_engine_get_slot(int idx);
