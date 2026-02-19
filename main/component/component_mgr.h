#pragma once

#include "esp_err.h"
#include <stdbool.h>

/* ── Component Manager ─────────────────────────────────────────────
 *
 * Provides order-aware initialization with dependency resolution
 * and graceful degradation. Components register themselves with
 * init/deinit callbacks and dependency lists. The manager resolves
 * the order and handles failures.
 *
 * Layers:
 *   L0 (Base)      : NVS, SPIFFS, WiFi, MsgBus
 *   L1 (Core)      : Agent, ToolRegistry, SkillEngine, LLM
 *   L2 (Entry)     : WebUI, Telegram, WS, CLI, MQTT
 *   L3 (Extension) : Zigbee, MCP, HA, OTA
 * ──────────────────────────────────────────────────────────────── */

#define COMP_MAX_COMPONENTS  32
#define COMP_MAX_DEPS         8
#define COMP_NAME_LEN        32

typedef enum {
    COMP_LAYER_BASE      = 0,   /* L0: Must init first */
    COMP_LAYER_CORE      = 1,   /* L1: Agent + supporting services */
    COMP_LAYER_ENTRY     = 2,   /* L2: User-facing entry points */
    COMP_LAYER_EXTENSION = 3,   /* L3: Optional extensions */
} comp_layer_t;

typedef enum {
    COMP_STATE_REGISTERED  = 0,
    COMP_STATE_READY       = 1,
    COMP_STATE_FAILED      = 2,
    COMP_STATE_DISABLED    = 3,
    COMP_STATE_STOPPED     = 4,
} comp_state_t;

typedef esp_err_t (*comp_init_fn)(void);
typedef void      (*comp_deinit_fn)(void);

/**
 * Start phase callback — called after WiFi is connected.
 * Components may return ESP_OK, or ESP_ERR_* to indicate failure.
 * Components that don't need a start phase can pass NULL.
 */
typedef esp_err_t (*comp_start_fn)(void);

typedef struct {
    char         name[COMP_NAME_LEN];
    comp_layer_t layer;
    comp_state_t state;
    bool         required;         /* true = error aborts boot; false = degraded mode */
    bool         needs_wifi;       /* true = start_fn only called after WiFi connects */
    comp_init_fn   init_fn;
    comp_start_fn  start_fn;       /* optional: called after WiFi, NULL = skip */
    comp_deinit_fn deinit_fn;      /* optional: called on shutdown, NULL = skip */
    int          dep_count;
    char         deps[COMP_MAX_DEPS][COMP_NAME_LEN];
    esp_err_t    last_error;
} comp_entry_t;

/**
 * Register a component with the manager.
 * @param name       Unique name (e.g. "nvs", "agent", "telegram")
 * @param layer      Priority layer (L0-L3)
 * @param required   If true, init failure aborts boot
 * @param needs_wifi If true, start_fn is deferred until WiFi
 * @param init_fn    Initialization function (called during init_all)
 * @param start_fn   Start function (called after WiFi, or NULL)
 * @param deinit_fn  Deinit function (or NULL)
 * @param deps       Null-terminated array of dependency names, or NULL
 */
esp_err_t comp_register(const char *name, comp_layer_t layer, bool required,
                        bool needs_wifi,
                        comp_init_fn init_fn, comp_start_fn start_fn,
                        comp_deinit_fn deinit_fn,
                        const char *deps[]);

/**
 * Initialize all registered components in dependency order.
 * Components with needs_wifi=true will have init_fn called but
 * start_fn deferred until comp_start_wifi_dependents() is called.
 *
 * On failure of a required component: returns the error immediately.
 * On failure of an optional component: logs warning, marks FAILED, continues.
 */
esp_err_t comp_init_all(void);

/**
 * Call start_fn for all components that need WiFi.
 * Should be called after WiFi is connected.
 */
esp_err_t comp_start_wifi_dependents(void);

/**
 * Deinitialize all components in reverse order.
 */
void comp_deinit_all(void);

/**
 * Get the state of a component by name.
 * @return pointer to entry, or NULL if not found
 */
const comp_entry_t *comp_get(const char *name);

/**
 * Check if a component is ready (init succeeded).
 */
bool comp_is_ready(const char *name);

/**
 * Get component status as JSON string (for Web UI / CLI).
 * Caller must free().
 */
char *comp_status_json(void);

/**
 * Get total number of registered components.
 */
int comp_get_count(void);
