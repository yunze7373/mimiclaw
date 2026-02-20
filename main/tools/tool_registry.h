#pragma once

#include "esp_err.h"
#include <stddef.h>

/* ── Legacy Tool Struct ────────────────────────────────────────────── */

typedef struct {
    const char *name;
    const char *description;
    const char *input_schema_json;  /* JSON Schema string for input */
    esp_err_t (*execute)(const char *input_json, char *output, size_t output_size);
} mimi_tool_t;

/* ── Tool Provider Interface ────────────────────────────────────────── */

/**
 * Tool Provider Interface
 * Allows external modules (MCP, HA, Zigbee) to dynamically provide tools.
 */
typedef struct {
    const char *name;
    
    /**
     * Get JSON array string of tools provided by this provider.
     * Caller owns the returned string and must free it.
     */
    char *(*get_tools_json)(void);
    
    /**
     * Execute a tool provided by this provider.
     * @return ESP_OK if executed, ESP_ERR_NOT_FOUND if tool not owned by provider, other error if execution failed.
     */
    esp_err_t (*execute_tool)(const char *tool_name, const char *input_json, char *output, size_t output_size);
} tool_provider_t;

/* ── API ───────────────────────────────────────────────────────────── */

/**
 * Initialize tool registry and register all built-in tools.
 */
esp_err_t tool_registry_init(void);

/**
 * Register a single legacy tool into the registry (via Built-in Provider).
 * Can be called after init (e.g., by skill engine for Lua tools).
 */
void tool_registry_register(const mimi_tool_t *tool);

/**
 * Unregister a legacy tool by name.
 */
void tool_registry_unregister(const char *name);

/**
 * Register a dynamic tool provider.
 */
esp_err_t tool_registry_register_provider(const tool_provider_t *provider);

/**
 * Rebuild the cached tools JSON array.
 * Call after dynamically registering new tools.
 */
void tool_registry_rebuild_json(void);

/**
 * Get the pre-built tools JSON array string for the API request.
 * Aggregates tools from all registered providers.
 * Returns NULL if no tools are registered.
 */
const char *tool_registry_get_tools_json(void);

/**
 * Execute a tool by name.
 * Searches across all registered providers.
 *
 * @param name         Tool name (e.g. "web_search")
 * @param input_json   JSON string of tool input
 * @param output       Output buffer for tool result text
 * @param output_size  Size of output buffer
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if tool unknown
 */
esp_err_t tool_registry_execute(const char *name, const char *input_json,
                                char *output, size_t output_size);
