/*
 * skill_engine.c — Lua-based hardware skill slot engine
 *
 * Loads .lua skill scripts from SPIFFS, creates Lua VMs,
 * registers tool functions into the existing tool_registry.
 */

#include "skills/skill_engine.h"
#include "skills/skill_hw_api.h"
#include "tools/tool_registry.h"

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

static const char *TAG = "skill_engine";

#define SKILL_MAX_SLOTS   8
#define SKILL_MAX_TOOLS   4   /* max tools per skill */
#define SKILL_DIR         "/spiffs/skills"
#define SKILL_OUTPUT_BUF  2048

/* ── Slot Structures ────────────────────────────────────── */

typedef struct {
    char name[32];
    char description[128];
    char version[16];
    char filename[64];
    lua_State *L;
    int  tool_count;
    char tool_names[SKILL_MAX_TOOLS][32];
    char tool_descs[SKILL_MAX_TOOLS][128];
    char tool_schemas[SKILL_MAX_TOOLS][256];
    char tool_handlers[SKILL_MAX_TOOLS][32]; /* Lua function name */
    bool loaded;
} skill_slot_t;

static skill_slot_t s_slots[SKILL_MAX_SLOTS];
static int s_slot_count = 0;

/* ── Lua Tool Executor (called by tool_registry) ─────── */

/*
 * Each Lua tool needs a C function pointer for tool_registry.
 * We use a trampoline: store slot_index + tool_index in static
 * data and dispatch via a single executor that calls lua_pcall.
 */

typedef struct {
    int slot_idx;
    int tool_idx;
} lua_tool_ctx_t;

static lua_tool_ctx_t s_tool_ctx[SKILL_MAX_SLOTS * SKILL_MAX_TOOLS];
static int s_tool_ctx_count = 0;

static esp_err_t lua_tool_execute(int ctx_idx, const char *input_json,
                                  char *output, size_t output_size)
{
    if (ctx_idx < 0 || ctx_idx >= s_tool_ctx_count) {
        snprintf(output, output_size, "Error: invalid skill tool context");
        return ESP_OK;
    }

    lua_tool_ctx_t *ctx = &s_tool_ctx[ctx_idx];
    skill_slot_t *slot = &s_slots[ctx->slot_idx];

    if (!slot->loaded || !slot->L) {
        snprintf(output, output_size, "Error: skill '%s' not loaded", slot->name);
        return ESP_OK;
    }

    lua_State *L = slot->L;

    /* Get the handler function from TOOLS table */
    lua_getglobal(L, "TOOLS");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        snprintf(output, output_size, "Error: TOOLS table not found in skill '%s'", slot->name);
        return ESP_OK;
    }

    /* Lua arrays are 1-indexed */
    lua_rawgeti(L, -1, ctx->tool_idx + 1);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 2);
        snprintf(output, output_size, "Error: tool entry not found");
        return ESP_OK;
    }

    lua_getfield(L, -1, "handler");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 3);
        snprintf(output, output_size, "Error: handler not a function in skill '%s'", slot->name);
        return ESP_OK;
    }

    /* Push input as a Lua table (parse JSON) */
    cJSON *root = cJSON_Parse(input_json);
    if (root) {
        lua_newtable(L);
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, root) {
            if (cJSON_IsString(item)) {
                lua_pushstring(L, item->valuestring);
            } else if (cJSON_IsNumber(item)) {
                lua_pushnumber(L, item->valuedouble);
            } else if (cJSON_IsBool(item)) {
                lua_pushboolean(L, cJSON_IsTrue(item));
            } else {
                lua_pushnil(L);
            }
            lua_setfield(L, -2, item->string);
        }
        cJSON_Delete(root);
    } else {
        lua_newtable(L); /* empty params */
    }

    /* Call the Lua function */
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        snprintf(output, output_size, "Error: Lua skill error: %s",
                 err ? err : "unknown");
        lua_pop(L, 3); /* error + tool entry + TOOLS */
        return ESP_OK;
    }

    /* Convert return value to JSON string */
    if (lua_istable(L, -1)) {
        /* Build JSON from returned Lua table */
        cJSON *result = cJSON_CreateObject();
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            const char *key = lua_tostring(L, -2);
            if (key) {
                if (lua_isnumber(L, -1)) {
                    cJSON_AddNumberToObject(result, key, lua_tonumber(L, -1));
                } else if (lua_isstring(L, -1)) {
                    cJSON_AddStringToObject(result, key, lua_tostring(L, -1));
                } else if (lua_isboolean(L, -1)) {
                    cJSON_AddBoolToObject(result, key, lua_toboolean(L, -1));
                }
            }
            lua_pop(L, 1); /* pop value, keep key for next iteration */
        }
        char *json_str = cJSON_PrintUnformatted(result);
        cJSON_Delete(result);
        if (json_str) {
            snprintf(output, output_size, "%s", json_str);
            free(json_str);
        }
    } else if (lua_isstring(L, -1)) {
        snprintf(output, output_size, "%s", lua_tostring(L, -1));
    } else if (lua_isnumber(L, -1)) {
        snprintf(output, output_size, "%g", lua_tonumber(L, -1));
    } else {
        snprintf(output, output_size, "{\"status\":\"ok\"}");
    }

    lua_pop(L, 3); /* result + tool entry + TOOLS */
    return ESP_OK;
}

/*
 * We need unique C function pointers for each Lua tool.
 * Since C doesn't have closures, we generate static trampolines
 * for each possible slot (up to SKILL_MAX_SLOTS * SKILL_MAX_TOOLS).
 */
#define TRAMPOLINE(N) \
    static esp_err_t lua_trampoline_##N(const char *input, char *output, size_t len) { \
        return lua_tool_execute(N, input, output, len); \
    }

TRAMPOLINE(0)  TRAMPOLINE(1)  TRAMPOLINE(2)  TRAMPOLINE(3)
TRAMPOLINE(4)  TRAMPOLINE(5)  TRAMPOLINE(6)  TRAMPOLINE(7)
TRAMPOLINE(8)  TRAMPOLINE(9)  TRAMPOLINE(10) TRAMPOLINE(11)
TRAMPOLINE(12) TRAMPOLINE(13) TRAMPOLINE(14) TRAMPOLINE(15)
TRAMPOLINE(16) TRAMPOLINE(17) TRAMPOLINE(18) TRAMPOLINE(19)
TRAMPOLINE(20) TRAMPOLINE(21) TRAMPOLINE(22) TRAMPOLINE(23)
TRAMPOLINE(24) TRAMPOLINE(25) TRAMPOLINE(26) TRAMPOLINE(27)
TRAMPOLINE(28) TRAMPOLINE(29) TRAMPOLINE(30) TRAMPOLINE(31)

typedef esp_err_t (*tool_exec_fn)(const char *, char *, size_t);

static const tool_exec_fn s_trampolines[] = {
    lua_trampoline_0,  lua_trampoline_1,  lua_trampoline_2,  lua_trampoline_3,
    lua_trampoline_4,  lua_trampoline_5,  lua_trampoline_6,  lua_trampoline_7,
    lua_trampoline_8,  lua_trampoline_9,  lua_trampoline_10, lua_trampoline_11,
    lua_trampoline_12, lua_trampoline_13, lua_trampoline_14, lua_trampoline_15,
    lua_trampoline_16, lua_trampoline_17, lua_trampoline_18, lua_trampoline_19,
    lua_trampoline_20, lua_trampoline_21, lua_trampoline_22, lua_trampoline_23,
    lua_trampoline_24, lua_trampoline_25, lua_trampoline_26, lua_trampoline_27,
    lua_trampoline_28, lua_trampoline_29, lua_trampoline_30, lua_trampoline_31,
};

#define MAX_TRAMPOLINES (sizeof(s_trampolines) / sizeof(s_trampolines[0]))

/* ── Load a Single Skill ─────────────────────────────── */

static esp_err_t load_skill(const char *filepath, int slot_idx)
{
    skill_slot_t *slot = &s_slots[slot_idx];
    memset(slot, 0, sizeof(skill_slot_t));

    /* Extract filename */
    const char *fname = strrchr(filepath, '/');
    fname = fname ? fname + 1 : filepath;
    strncpy(slot->filename, fname, sizeof(slot->filename) - 1);

    /* Create Lua state */
    lua_State *L = luaL_newstate();
    if (!L) {
        ESP_LOGE(TAG, "Failed to create Lua state for %s", fname);
        return ESP_ERR_NO_MEM;
    }

    /* Open safe standard libs (no io/os for security) */
    luaL_requiref(L, "_G", luaopen_base, 1);        lua_pop(L, 1);
    luaL_requiref(L, "table", luaopen_table, 1);     lua_pop(L, 1);
    luaL_requiref(L, "string", luaopen_string, 1);   lua_pop(L, 1);
    luaL_requiref(L, "math", luaopen_math, 1);       lua_pop(L, 1);
    luaL_requiref(L, "utf8", luaopen_utf8, 1);       lua_pop(L, 1);

    /* Register hw.* API */
    skill_hw_api_register(L);

    /* Load and execute the script */
    if (luaL_dofile(L, filepath) != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        ESP_LOGE(TAG, "Failed to load skill %s: %s", fname, err ? err : "unknown");
        lua_close(L);
        return ESP_FAIL;
    }

    /* Read SKILL table */
    lua_getglobal(L, "SKILL");
    if (!lua_istable(L, -1)) {
        ESP_LOGE(TAG, "Skill %s: missing SKILL table", fname);
        lua_close(L);
        return ESP_FAIL;
    }

    lua_getfield(L, -1, "name");
    if (lua_isstring(L, -1)) {
        strncpy(slot->name, lua_tostring(L, -1), sizeof(slot->name) - 1);
    }
    lua_pop(L, 1);

    lua_getfield(L, -1, "desc");
    if (lua_isstring(L, -1)) {
        strncpy(slot->description, lua_tostring(L, -1), sizeof(slot->description) - 1);
    }
    lua_pop(L, 1);

    lua_getfield(L, -1, "version");
    if (lua_isstring(L, -1)) {
        strncpy(slot->version, lua_tostring(L, -1), sizeof(slot->version) - 1);
    } else {
        strcpy(slot->version, "1.0");
    }
    lua_pop(L, 1);

    lua_pop(L, 1); /* pop SKILL table */

    /* Call init(config) if present */
    lua_getglobal(L, "init");
    if (lua_isfunction(L, -1)) {
        lua_newtable(L); /* empty config for now */
        if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
            const char *err = lua_tostring(L, -1);
            ESP_LOGW(TAG, "Skill %s: init() failed: %s", slot->name, err ? err : "unknown");
            lua_pop(L, 1);
            /* Non-fatal: continue loading tools */
        } else {
            lua_pop(L, 1); /* pop return value */
        }
    } else {
        lua_pop(L, 1); /* pop non-function */
    }

    /* Read TOOLS table */
    lua_getglobal(L, "TOOLS");
    if (!lua_istable(L, -1)) {
        ESP_LOGW(TAG, "Skill %s: no TOOLS table, loaded without tools", slot->name);
        lua_pop(L, 1);
        slot->L = L;
        slot->loaded = true;
        return ESP_OK;
    }

    int tools_len = (int)lua_rawlen(L, -1);
    if (tools_len > SKILL_MAX_TOOLS) {
        tools_len = SKILL_MAX_TOOLS;
    }

    for (int i = 0; i < tools_len; i++) {
        lua_rawgeti(L, -1, i + 1); /* 1-indexed */
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            continue;
        }

        /* Read tool definition */
        lua_getfield(L, -1, "name");
        if (lua_isstring(L, -1)) {
            strncpy(slot->tool_names[i], lua_tostring(L, -1),
                    sizeof(slot->tool_names[i]) - 1);
        }
        lua_pop(L, 1);

        lua_getfield(L, -1, "desc");
        if (lua_isstring(L, -1)) {
            strncpy(slot->tool_descs[i], lua_tostring(L, -1),
                    sizeof(slot->tool_descs[i]) - 1);
        }
        lua_pop(L, 1);

        lua_getfield(L, -1, "schema");
        if (lua_isstring(L, -1)) {
            strncpy(slot->tool_schemas[i], lua_tostring(L, -1),
                    sizeof(slot->tool_schemas[i]) - 1);
        } else {
            strcpy(slot->tool_schemas[i],
                   "{\"type\":\"object\",\"properties\":{}}");
        }
        lua_pop(L, 1);

        /* Verify handler exists */
        lua_getfield(L, -1, "handler");
        bool has_handler = lua_isfunction(L, -1);
        lua_pop(L, 1);

        if (has_handler && slot->tool_names[i][0]) {
            /* Register into tool_registry via trampoline */
            int tramp_idx = s_tool_ctx_count;
            if (tramp_idx < (int)MAX_TRAMPOLINES) {
                s_tool_ctx[tramp_idx].slot_idx = slot_idx;
                s_tool_ctx[tramp_idx].tool_idx = i;

                mimi_tool_t t = {
                    .name = slot->tool_names[i],
                    .description = slot->tool_descs[i],
                    .input_schema_json = slot->tool_schemas[i],
                    .execute = s_trampolines[tramp_idx],
                };
                tool_registry_register(&t);
                s_tool_ctx_count++;
                slot->tool_count++;

                ESP_LOGI(TAG, "  Tool: %s (slot %d, trampoline %d)",
                         slot->tool_names[i], slot_idx, tramp_idx);
            } else {
                ESP_LOGW(TAG, "  Trampoline limit reached, skipping tool %s",
                         slot->tool_names[i]);
            }
        }

        lua_pop(L, 1); /* pop tool entry */
    }

    lua_pop(L, 1); /* pop TOOLS table */

    slot->L = L;
    slot->loaded = true;

    ESP_LOGI(TAG, "Skill '%s' v%s loaded (%d tools) from %s",
             slot->name, slot->version, slot->tool_count, slot->filename);
    return ESP_OK;
}

/* ── Public API ───────────────────────────────────────── */

esp_err_t skill_engine_init(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    s_slot_count = 0;
    s_tool_ctx_count = 0;

    /* Ensure skills directory exists */
    struct stat st;
    if (stat(SKILL_DIR, &st) != 0) {
        ESP_LOGI(TAG, "Creating %s directory", SKILL_DIR);
        mkdir(SKILL_DIR, 0755);
    }

    /* Scan for .lua files */
    DIR *dir = opendir(SKILL_DIR);
    if (!dir) {
        ESP_LOGI(TAG, "No skills directory, engine idle (0 skills)");
        return ESP_OK;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && s_slot_count < SKILL_MAX_SLOTS) {
        size_t nlen = strlen(ent->d_name);
        if (nlen < 5 || strcmp(ent->d_name + nlen - 4, ".lua") != 0) {
            continue;
        }

        char path[128];
        snprintf(path, sizeof(path), "%s/%s", SKILL_DIR, ent->d_name);

        ESP_LOGI(TAG, "Loading skill: %s", ent->d_name);
        if (load_skill(path, s_slot_count) == ESP_OK) {
            s_slot_count++;
        }
    }
    closedir(dir);

    /* Rebuild tool registry JSON to include Lua tools */
    if (s_slot_count > 0) {
        tool_registry_rebuild_json();
    }

    ESP_LOGI(TAG, "Skill engine: %d skills loaded, %d Lua tools registered",
             s_slot_count, s_tool_ctx_count);
    return ESP_OK;
}

esp_err_t skill_engine_install(const char *url)
{
    if (!url || !url[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_slot_count >= SKILL_MAX_SLOTS) {
        ESP_LOGE(TAG, "All %d skill slots are full", SKILL_MAX_SLOTS);
        return ESP_ERR_NO_MEM;
    }

    /* Extract filename from URL */
    const char *fname = strrchr(url, '/');
    fname = fname ? fname + 1 : url;

    char path[128];
    snprintf(path, sizeof(path), "%s/%s", SKILL_DIR, fname);

    /* Download the file */
    ESP_LOGI(TAG, "Downloading skill from %s", url);

    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s for writing", path);
        return ESP_FAIL;
    }

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        fclose(f);
        esp_http_client_cleanup(client);
        return ret;
    }

    int content_len = esp_http_client_fetch_headers(client);
    (void)content_len;

    char buf[512];
    int read_len;
    while ((read_len = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
        fwrite(buf, 1, read_len, f);
    }

    fclose(f);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    /* Hot-load into next available slot */
    ret = load_skill(path, s_slot_count);
    if (ret == ESP_OK) {
        s_slot_count++;
        tool_registry_rebuild_json();
        ESP_LOGI(TAG, "Skill installed and hot-loaded: %s", fname);
    } else {
        /* Remove the failed file */
        remove(path);
    }

    return ret;
}

esp_err_t skill_engine_uninstall(const char *name)
{
    for (int i = 0; i < s_slot_count; i++) {
        if (s_slots[i].loaded && strcmp(s_slots[i].name, name) == 0) {
            /* Close Lua state */
            if (s_slots[i].L) {
                lua_close(s_slots[i].L);
                s_slots[i].L = NULL;
            }

            /* Delete the file */
            char path[128];
            snprintf(path, sizeof(path), "%s/%s", SKILL_DIR, s_slots[i].filename);
            remove(path);

            s_slots[i].loaded = false;
            ESP_LOGI(TAG, "Skill '%s' uninstalled", name);

            /* Note: tools remain in registry until next reboot.
             * They will return "skill not loaded" errors if called. */
            return ESP_OK;
        }
    }

    ESP_LOGW(TAG, "Skill '%s' not found", name);
    return ESP_ERR_NOT_FOUND;
}

char *skill_engine_list_json(void)
{
    cJSON *arr = cJSON_CreateArray();

    for (int i = 0; i < s_slot_count; i++) {
        if (!s_slots[i].loaded) continue;

        cJSON *skill = cJSON_CreateObject();
        cJSON_AddStringToObject(skill, "name", s_slots[i].name);
        cJSON_AddStringToObject(skill, "version", s_slots[i].version);
        cJSON_AddStringToObject(skill, "description", s_slots[i].description);
        cJSON_AddStringToObject(skill, "file", s_slots[i].filename);
        cJSON_AddNumberToObject(skill, "tools", s_slots[i].tool_count);

        /* List tool names */
        cJSON *tools = cJSON_CreateArray();
        for (int t = 0; t < s_slots[i].tool_count; t++) {
            cJSON_AddItemToArray(tools, cJSON_CreateString(s_slots[i].tool_names[t]));
        }
        cJSON_AddItemToObject(skill, "tool_names", tools);

        cJSON_AddItemToArray(arr, skill);
    }

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return json;
}

int skill_engine_get_count(void)
{
    return s_slot_count;
}
