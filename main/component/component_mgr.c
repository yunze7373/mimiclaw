#include "component/component_mgr.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "comp_mgr";

static comp_entry_t s_components[COMP_MAX_COMPONENTS];
static int s_count = 0;
static int s_init_order[COMP_MAX_COMPONENTS];
static int s_init_order_count = 0;

/* ── Helpers ──────────────────────────────────────────────────────── */

static comp_entry_t *find_by_name(const char *name)
{
    if (!name || !name[0]) return NULL;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_components[i].name, name) == 0) return &s_components[i];
    }
    return NULL;
}

static int find_index(const char *name)
{
    if (!name || !name[0]) return -1;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_components[i].name, name) == 0) return i;
    }
    return -1;
}

/* ── Topological sort (Kahn's algorithm) ──────────────────────────── */

static bool resolve_order(void)
{
    /* in-degree for each component */
    int indeg[COMP_MAX_COMPONENTS] = {0};
    s_init_order_count = 0;

    /* Calculate in-degrees based on deps */
    for (int i = 0; i < s_count; i++) {
        comp_entry_t *c = &s_components[i];
        for (int d = 0; d < c->dep_count; d++) {
            int dep_idx = find_index(c->deps[d]);
            if (dep_idx < 0) {
                ESP_LOGW(TAG, "Component '%s' depends on unregistered '%s' — ignored",
                         c->name, c->deps[d]);
                continue;
            }
            (void)dep_idx;
            indeg[i]++;
        }
    }

    /* Collect nodes with indegree 0, preferring lower layer */
    int queue[COMP_MAX_COMPONENTS];
    int head = 0, tail = 0;

    /* Multi-pass: add layer 0 first, then 1, 2, 3 */
    for (int layer = COMP_LAYER_BASE; layer <= COMP_LAYER_EXTENSION; layer++) {
        for (int i = 0; i < s_count; i++) {
            if ((int)s_components[i].layer == layer && indeg[i] == 0) {
                queue[tail++] = i;
            }
        }
    }

    while (head < tail) {
        int idx = queue[head++];
        s_init_order[s_init_order_count++] = idx;

        /* Decrease in-degree for dependents */
        for (int i = 0; i < s_count; i++) {
            comp_entry_t *c = &s_components[i];
            for (int d = 0; d < c->dep_count; d++) {
                if (find_index(c->deps[d]) == idx) {
                    indeg[i]--;
                    if (indeg[i] == 0) {
                        queue[tail++] = i;
                    }
                }
            }
        }
    }

    if (s_init_order_count != s_count) {
        ESP_LOGE(TAG, "Circular dependency detected! Resolved %d/%d",
                 s_init_order_count, s_count);
        return false;
    }

    return true;
}

/* ── Public API ──────────────────────────────────────────────────── */

esp_err_t comp_register(const char *name, comp_layer_t layer, bool required,
                        bool needs_wifi,
                        comp_init_fn init_fn, comp_start_fn start_fn,
                        comp_deinit_fn deinit_fn,
                        const char *deps[])
{
    if (!name || !name[0]) return ESP_ERR_INVALID_ARG;
    if (!init_fn && !start_fn) return ESP_ERR_INVALID_ARG;  /* must have at least one */
    if (s_count >= COMP_MAX_COMPONENTS) return ESP_ERR_NO_MEM;
    if (find_by_name(name)) {
        ESP_LOGW(TAG, "Component '%s' already registered", name);
        return ESP_ERR_INVALID_STATE;
    }

    comp_entry_t *c = &s_components[s_count++];
    memset(c, 0, sizeof(*c));
    snprintf(c->name, sizeof(c->name), "%s", name);
    c->layer       = layer;
    c->required    = required;
    c->needs_wifi  = needs_wifi;
    c->init_fn     = init_fn;
    c->start_fn    = start_fn;
    c->deinit_fn   = deinit_fn;
    c->state       = COMP_STATE_REGISTERED;

    if (deps) {
        for (int i = 0; deps[i] && i < COMP_MAX_DEPS; i++) {
            snprintf(c->deps[i], COMP_NAME_LEN, "%s", deps[i]);
            c->dep_count++;
        }
    }

    ESP_LOGD(TAG, "Registered: %s (L%d, %s, deps=%d)",
             name, (int)layer, required ? "required" : "optional", c->dep_count);
    return ESP_OK;
}

esp_err_t comp_init_all(void)
{
    ESP_LOGI(TAG, "Initializing %d components...", s_count);

    if (!resolve_order()) {
        return ESP_ERR_INVALID_STATE;
    }

    int success = 0, failed = 0, skipped = 0;

    for (int i = 0; i < s_init_order_count; i++) {
        comp_entry_t *c = &s_components[s_init_order[i]];

        if (c->state == COMP_STATE_DISABLED) {
            skipped++;
            continue;
        }

        /* Check if all dependencies are ready */
        bool deps_ok = true;
        for (int d = 0; d < c->dep_count; d++) {
            comp_entry_t *dep = find_by_name(c->deps[d]);
            if (!dep || dep->state != COMP_STATE_READY) {
                deps_ok = false;
                ESP_LOGW(TAG, "Component '%s' dependency '%s' not ready — %s",
                         c->name, c->deps[d],
                         c->required ? "ABORT" : "skip");
                break;
            }
        }

        if (!deps_ok) {
            c->state = COMP_STATE_FAILED;
            c->last_error = ESP_ERR_INVALID_STATE;
            if (c->required) {
                ESP_LOGE(TAG, "Required component '%s' cannot init (missing deps)", c->name);
                return ESP_ERR_INVALID_STATE;
            }
            failed++;
            continue;
        }

        ESP_LOGI(TAG, "Init [L%d] %s ...", (int)c->layer, c->name);
        esp_err_t ret = ESP_OK;
        if (c->init_fn) {
            ret = c->init_fn();
        }

        if (ret == ESP_OK) {
            c->state = COMP_STATE_READY;
            success++;
        } else {
            c->state = COMP_STATE_FAILED;
            c->last_error = ret;
            if (c->required) {
                ESP_LOGE(TAG, "FATAL: Required component '%s' failed: %s",
                         c->name, esp_err_to_name(ret));
                return ret;
            }
            ESP_LOGW(TAG, "Optional component '%s' failed: %s — degraded mode",
                     c->name, esp_err_to_name(ret));
            failed++;
        }
    }

    ESP_LOGI(TAG, "Init complete: %d OK, %d failed, %d skipped",
             success, failed, skipped);
    return ESP_OK;
}

esp_err_t comp_start_wifi_dependents(void)
{
    ESP_LOGI(TAG, "Starting WiFi-dependent components...");

    int started = 0, failed = 0;

    for (int i = 0; i < s_init_order_count; i++) {
        comp_entry_t *c = &s_components[s_init_order[i]];

        if (!c->needs_wifi || !c->start_fn) continue;
        if (c->state != COMP_STATE_READY) continue;

        ESP_LOGI(TAG, "Start [L%d] %s ...", (int)c->layer, c->name);
        esp_err_t ret = c->start_fn();

        if (ret == ESP_OK) {
            started++;
        } else {
            c->state = COMP_STATE_FAILED;
            c->last_error = ret;
            if (c->required) {
                ESP_LOGE(TAG, "FATAL: Required component '%s' start failed: %s",
                         c->name, esp_err_to_name(ret));
                return ret;
            }
            ESP_LOGW(TAG, "Optional component '%s' start failed: %s",
                     c->name, esp_err_to_name(ret));
            failed++;
        }
    }

    ESP_LOGI(TAG, "WiFi start: %d started, %d failed", started, failed);
    return ESP_OK;
}

void comp_deinit_all(void)
{
    /* Reverse order */
    for (int i = s_init_order_count - 1; i >= 0; i--) {
        comp_entry_t *c = &s_components[s_init_order[i]];
        if (c->state == COMP_STATE_READY && c->deinit_fn) {
            ESP_LOGI(TAG, "Deinit %s", c->name);
            c->deinit_fn();
            c->state = COMP_STATE_STOPPED;
        }
    }
}

const comp_entry_t *comp_get(const char *name)
{
    return find_by_name(name);
}

bool comp_is_ready(const char *name)
{
    const comp_entry_t *c = find_by_name(name);
    return c && c->state == COMP_STATE_READY;
}

char *comp_status_json(void)
{
    cJSON *root = cJSON_CreateArray();
    if (!root) return NULL;

    for (int i = 0; i < s_count; i++) {
        comp_entry_t *c = &s_components[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", c->name);
        cJSON_AddNumberToObject(item, "layer", (int)c->layer);

        const char *state_str = "unknown";
        switch (c->state) {
            case COMP_STATE_REGISTERED: state_str = "registered"; break;
            case COMP_STATE_READY:      state_str = "ready";      break;
            case COMP_STATE_FAILED:     state_str = "failed";     break;
            case COMP_STATE_DISABLED:   state_str = "disabled";   break;
            case COMP_STATE_STOPPED:    state_str = "stopped";    break;
        }
        cJSON_AddStringToObject(item, "state", state_str);
        cJSON_AddBoolToObject(item, "required", c->required);
        cJSON_AddBoolToObject(item, "needs_wifi", c->needs_wifi);

        if (c->last_error != ESP_OK) {
            cJSON_AddStringToObject(item, "error", esp_err_to_name(c->last_error));
        }

        if (c->dep_count > 0) {
            cJSON *deps = cJSON_CreateArray();
            for (int d = 0; d < c->dep_count; d++) {
                cJSON_AddItemToArray(deps, cJSON_CreateString(c->deps[d]));
            }
            cJSON_AddItemToObject(item, "deps", deps);
        }

        cJSON_AddItemToArray(root, item);
    }

    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return str;
}

int comp_get_count(void)
{
    return s_count;
}

/* ── Runtime Config ──────────────────────────────────────────────── */

esp_err_t comp_load_config(void)
{
    FILE *f = fopen(COMP_CONFIG_FILE, "r");
    if (!f) {
        ESP_LOGI(TAG, "No component config file, all enabled by default");
        return ESP_ERR_NOT_FOUND;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len <= 0 || len > 2048) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }

    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse component config JSON");
        return ESP_ERR_INVALID_STATE;
    }

    /* Format: { "disabled": ["telegram", "websocket"] } */
    cJSON *disabled = cJSON_GetObjectItem(root, "disabled");
    if (cJSON_IsArray(disabled)) {
        int disabled_count = 0;
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, disabled) {
            if (!cJSON_IsString(item)) continue;
            comp_entry_t *c = find_by_name(item->valuestring);
            if (c) {
                if (c->required) {
                    ESP_LOGW(TAG, "Cannot disable required component '%s'", c->name);
                    continue;
                }
                c->state = COMP_STATE_DISABLED;
                disabled_count++;
                ESP_LOGI(TAG, "Component '%s' disabled by config", c->name);
            }
        }
        ESP_LOGI(TAG, "Config loaded: %d components disabled", disabled_count);
    }

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t comp_save_config(void)
{

    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;

    cJSON *disabled = cJSON_CreateArray();
    for (int i = 0; i < s_count; i++) {
        if (s_components[i].state == COMP_STATE_DISABLED) {
            cJSON_AddItemToArray(disabled, cJSON_CreateString(s_components[i].name));
        }
    }
    cJSON_AddItemToObject(root, "disabled", disabled);

    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!str) return ESP_ERR_NO_MEM;

    FILE *f = fopen(COMP_CONFIG_FILE, "w");
    if (!f) {
        free(str);
        ESP_LOGE(TAG, "Failed to write component config");
        return ESP_FAIL;
    }

    fputs(str, f);
    fclose(f);
    free(str);

    ESP_LOGI(TAG, "Component config saved");
    return ESP_OK;
}

esp_err_t comp_set_enabled(const char *name, bool enabled)
{
    comp_entry_t *c = find_by_name(name);
    if (!c) return ESP_ERR_NOT_FOUND;

    if (!enabled && c->required) {
        ESP_LOGW(TAG, "Cannot disable required component '%s'", name);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (enabled) {
        /* Mark as registered — will be initialized on next boot */
        if (c->state == COMP_STATE_DISABLED) {
            c->state = COMP_STATE_REGISTERED;
            ESP_LOGI(TAG, "Component '%s' enabled (takes effect on next boot)", name);
        }
    } else {
        c->state = COMP_STATE_DISABLED;
        ESP_LOGI(TAG, "Component '%s' disabled (takes effect on next boot)", name);
    }

    return comp_save_config();
}

