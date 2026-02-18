#include "skills/skill_engine.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "mbedtls/sha256.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "skills/skill_types.h"
#include "skills/skill_hw_api.h"
#include "skills/skill_runtime.h"
#include "skills/skill_resource_manager.h"
#include "skills/board_profile.h"
#include "tools/tool_registry.h"
#include "bus/message_bus.h"

static const char *TAG = "skill_engine";

#define SKILL_DIR                 "/spiffs/skills"
#define SKILL_MAX_SCHEMA_JSON     512
#define SKILL_INSTALL_MAX_BYTES   (1024 * 1024)
#define SKILL_EXEC_INSTR_BUDGET   200000
#define SKILL_EXEC_TIME_BUDGET_MS 200
#define LUA_HOOK_STRIDE           1000
#define SKILL_MAX_TIMERS          24
#define SKILL_MAX_GPIO_INTR       16
#define SKILL_CB_QUEUE_DEPTH      32

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

typedef struct {
    int slot_idx;
    int tool_idx;
    bool used;
} lua_tool_ctx_t;

typedef struct {
    bool active;
    int64_t started_us;
    int instr_budget;
    int instr_used;
    int time_budget_ms;
} exec_guard_t;

static lua_State *s_L = NULL;
static int s_safe_stdlib_ref = LUA_NOREF;
static skill_slot_t s_slots[SKILL_MAX_SLOTS];
static int s_slot_count = 0;
static lua_tool_ctx_t s_tool_ctx[SKILL_MAX_SLOTS * SKILL_MAX_TOOLS_PER_SKILL];
static int s_tool_ctx_count = 0;
static exec_guard_t s_guard = {0};
static SemaphoreHandle_t s_lua_lock = NULL;
static SemaphoreHandle_t s_install_lock = NULL;
static uint32_t s_install_seq = 0;
typedef struct {
    bool in_progress;
    uint32_t seq;
    int64_t started_us;
    int64_t finished_us;
    int64_t total_bytes;
    int64_t downloaded_bytes;
    char stage[32];
    char package_type[8];
    char url[256];
    char last_error[64];
} install_status_t;
static install_status_t s_install_status = {0};

typedef struct {
    uint32_t seq;
    int64_t started_us;
    int64_t finished_us;
    char stage[32];
    char url[256];
    char error[64];
    bool success;
} install_history_entry_t;

#define INSTALL_HISTORY_MAX 8
static install_history_entry_t s_install_history[INSTALL_HISTORY_MAX];
static int s_install_history_count = 0;
static int s_install_history_next = 0;

typedef struct {
    bool used;
    int timer_id;
    int skill_id;
    bool periodic;
    int period_ms;
    int lua_cb_ref;
    esp_timer_handle_t handle;
} skill_timer_t;

typedef struct {
    int type;
    int timer_id;
    int intr_id;
    int pin;
} skill_cb_event_t;

typedef struct {
    bool used;
    int intr_id;
    int skill_id;
    int pin;
    int lua_cb_ref;
} skill_gpio_intr_t;

static skill_timer_t s_timers[SKILL_MAX_TIMERS];
static skill_gpio_intr_t s_gpio_intr[SKILL_MAX_GPIO_INTR];
static int s_next_timer_id = 1;
static int s_next_intr_id = 1;
static QueueHandle_t s_cb_queue = NULL;
static TaskHandle_t s_cb_task = NULL;
static void remove_path_recursive(const char *path);

static bool slot_has_declared_event(int slot_idx, const char *event_name)
{
    if (!event_name || !event_name[0] || slot_idx < 0 || slot_idx >= SKILL_MAX_SLOTS) return false;
    skill_slot_t *slot = &s_slots[slot_idx];
    for (int i = 0; i < slot->event_count; i++) {
        if (strcmp(slot->event_names[i], event_name) == 0) return true;
    }
    return false;
}

static void install_status_begin(const char *url)
{
    s_install_status.in_progress = true;
    s_install_status.seq++;
    s_install_status.started_us = esp_timer_get_time();
    s_install_status.finished_us = 0;
    s_install_status.total_bytes = 0;
    s_install_status.downloaded_bytes = 0;
    snprintf(s_install_status.stage, sizeof(s_install_status.stage), "%s", "prepare");
    snprintf(s_install_status.package_type, sizeof(s_install_status.package_type), "%s", "");
    snprintf(s_install_status.url, sizeof(s_install_status.url), "%s", url ? url : "");
    s_install_status.last_error[0] = '\0';
}

static void install_status_step(const char *stage)
{
    if (!stage || !stage[0]) return;
    snprintf(s_install_status.stage, sizeof(s_install_status.stage), "%s", stage);
}

static void install_status_finish(esp_err_t err)
{
    s_install_status.in_progress = false;
    s_install_status.finished_us = esp_timer_get_time();
    if (err == ESP_OK) {
        snprintf(s_install_status.stage, sizeof(s_install_status.stage), "%s", "done");
        s_install_status.last_error[0] = '\0';
    } else {
        snprintf(s_install_status.stage, sizeof(s_install_status.stage), "%s", "failed");
        snprintf(s_install_status.last_error, sizeof(s_install_status.last_error), "%s", esp_err_to_name(err));
    }

    install_history_entry_t *e = &s_install_history[s_install_history_next];
    memset(e, 0, sizeof(*e));
    e->seq = s_install_status.seq;
    e->started_us = s_install_status.started_us;
    e->finished_us = s_install_status.finished_us;
    snprintf(e->stage, sizeof(e->stage), "%s", s_install_status.stage);
    snprintf(e->url, sizeof(e->url), "%s", s_install_status.url);
    snprintf(e->error, sizeof(e->error), "%s", s_install_status.last_error);
    e->success = (err == ESP_OK);

    s_install_history_next = (s_install_history_next + 1) % INSTALL_HISTORY_MAX;
    if (s_install_history_count < INSTALL_HISTORY_MAX) s_install_history_count++;
}

static void install_status_set_total_bytes(int64_t n)
{
    if (n > 0) s_install_status.total_bytes = n;
}

static void install_status_add_downloaded(int64_t n)
{
    if (n <= 0) return;
    s_install_status.downloaded_bytes += n;
    if (s_install_status.total_bytes > 0 &&
        s_install_status.downloaded_bytes > s_install_status.total_bytes) {
        s_install_status.downloaded_bytes = s_install_status.total_bytes;
    }
}

static void install_status_set_package_type(const char *t)
{
    if (!t || !t[0]) return;
    snprintf(s_install_status.package_type, sizeof(s_install_status.package_type), "%s", t);
}

static cJSON *lua_value_to_cjson(lua_State *L, int idx);

static cJSON *lua_table_to_cjson(lua_State *L, int idx)
{
    idx = lua_absindex(L, idx);
    size_t len = lua_rawlen(L, idx);
    bool is_array = true;

    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        if (!lua_isinteger(L, -2)) {
            is_array = false;
            lua_pop(L, 2);
            break;
        }
        lua_Integer k = lua_tointeger(L, -2);
        if (k < 1 || k > (lua_Integer)len) is_array = false;
        lua_pop(L, 1);
        if (!is_array) break;
    }

    if (is_array) {
        cJSON *arr = cJSON_CreateArray();
        for (size_t i = 1; i <= len; i++) {
            lua_rawgeti(L, idx, (lua_Integer)i);
            cJSON_AddItemToArray(arr, lua_value_to_cjson(L, -1));
            lua_pop(L, 1);
        }
        return arr;
    }

    cJSON *obj = cJSON_CreateObject();
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        const char *key = lua_tostring(L, -2);
        if (key) {
            cJSON_AddItemToObject(obj, key, lua_value_to_cjson(L, -1));
        }
        lua_pop(L, 1);
    }
    return obj;
}

static cJSON *lua_value_to_cjson(lua_State *L, int idx)
{
    int t = lua_type(L, idx);
    switch (t) {
        case LUA_TNIL: return cJSON_CreateNull();
        case LUA_TBOOLEAN: return cJSON_CreateBool(lua_toboolean(L, idx));
        case LUA_TNUMBER: return cJSON_CreateNumber(lua_tonumber(L, idx));
        case LUA_TSTRING: return cJSON_CreateString(lua_tostring(L, idx));
        case LUA_TTABLE: return lua_table_to_cjson(L, idx);
        default: return cJSON_CreateString("<unsupported>");
    }
}

static void cjson_to_lua(lua_State *L, const cJSON *node)
{
    if (!node) {
        lua_pushnil(L);
        return;
    }
    if (cJSON_IsObject(node)) {
        lua_newtable(L);
        cJSON *it = NULL;
        cJSON_ArrayForEach(it, node) {
            cjson_to_lua(L, it);
            lua_setfield(L, -2, it->string);
        }
        return;
    }
    if (cJSON_IsArray(node)) {
        lua_newtable(L);
        int i = 1;
        cJSON *it = NULL;
        cJSON_ArrayForEach(it, node) {
            cjson_to_lua(L, it);
            lua_rawseti(L, -2, i++);
        }
        return;
    }
    if (cJSON_IsString(node)) lua_pushstring(L, node->valuestring);
    else if (cJSON_IsNumber(node)) lua_pushnumber(L, node->valuedouble);
    else if (cJSON_IsBool(node)) lua_pushboolean(L, cJSON_IsTrue(node));
    else lua_pushnil(L);
}

static bool lua_table_to_json_buf(lua_State *L, int idx, char *out, size_t out_size)
{
    cJSON *root = lua_value_to_cjson(L, idx);
    if (!root) return false;
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) return false;
    snprintf(out, out_size, "%s", s);
    free(s);
    return true;
}

static void limit_hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    if (!s_guard.active) return;
    s_guard.instr_used += LUA_HOOK_STRIDE;
    int64_t elapsed_ms = (esp_timer_get_time() - s_guard.started_us) / 1000;
    if (s_guard.instr_used > s_guard.instr_budget || elapsed_ms > s_guard.time_budget_ms) {
        luaL_error(L, "skill execution limit exceeded");
    }
}

static void guard_begin(void)
{
    s_guard.active = true;
    s_guard.started_us = esp_timer_get_time();
    s_guard.instr_budget = SKILL_EXEC_INSTR_BUDGET;
    s_guard.time_budget_ms = SKILL_EXEC_TIME_BUDGET_MS;
    s_guard.instr_used = 0;
    lua_sethook(s_L, limit_hook, LUA_MASKCOUNT, LUA_HOOK_STRIDE);
}

static void guard_end(void)
{
    s_guard.active = false;
    lua_sethook(s_L, NULL, 0, 0);
}

static bool lua_lock_take(TickType_t ticks)
{
    if (!s_lua_lock) return false;
    return xSemaphoreTakeRecursive(s_lua_lock, ticks) == pdTRUE;
}

static void lua_lock_give(void)
{
    if (s_lua_lock) xSemaphoreGiveRecursive(s_lua_lock);
}

static int l_console_log(lua_State *L)
{
    int slot_idx = (int)lua_tointeger(L, lua_upvalueindex(1));
    const char *level = luaL_optstring(L, 1, "info");
    const char *message = luaL_optstring(L, 2, "");
    ESP_LOGI(TAG, "[skill=%s][%s] %s", s_slots[slot_idx].name, level, message);
    return 0;
}

static int l_agent_emit_event(lua_State *L)
{
    int slot_idx = (int)lua_tointeger(L, lua_upvalueindex(1));
    const char *name = luaL_checkstring(L, 1);
    if (!slot_has_declared_event(slot_idx, name)) {
        return luaL_error(L, "event '%s' not declared in manifest", name);
    }

    cJSON *payload = cJSON_CreateObject();
    if (lua_gettop(L) >= 2 && lua_istable(L, 2)) {
        cJSON_Delete(payload);
        payload = lua_value_to_cjson(L, 2);
    }

    cJSON *evt = cJSON_CreateObject();
    cJSON_AddStringToObject(evt, "type", "skill_event");
    cJSON_AddStringToObject(evt, "skill", s_slots[slot_idx].name);
    cJSON_AddStringToObject(evt, "event", name);
    cJSON_AddItemToObject(evt, "payload", payload);

    char *json = cJSON_PrintUnformatted(evt);
    cJSON_Delete(evt);
    if (!json) {
        lua_pushboolean(L, 0);
        return 1;
    }

    mimi_msg_t msg = {0};
    snprintf(msg.channel, sizeof(msg.channel), "%s", MIMI_CHAN_SYSTEM);
    snprintf(msg.chat_id, sizeof(msg.chat_id), "skill_event");
    msg.content = json;
    esp_err_t ret = message_bus_push_inbound(&msg);
    if (ret != ESP_OK) free(json);
    lua_pushboolean(L, ret == ESP_OK);
    return 1;
}

static int struct_field_size(char fmt)
{
    switch (fmt) {
        case 'b':
        case 'B': return 1;
        case 'h':
        case 'H': return 2;
        case 'i':
        case 'I':
        case 'l':
        case 'L': return 4;
        default: return 0;
    }
}

static uint32_t read_be(const uint8_t *p, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 8) | p[i];
    return v;
}

static uint32_t read_le(const uint8_t *p, int n)
{
    uint32_t v = 0;
    for (int i = n - 1; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static void write_be(uint8_t *p, uint32_t v, int n)
{
    for (int i = n - 1; i >= 0; i--) {
        p[i] = (uint8_t)(v & 0xFF);
        v >>= 8;
    }
}

static void write_le(uint8_t *p, uint32_t v, int n)
{
    for (int i = 0; i < n; i++) {
        p[i] = (uint8_t)(v & 0xFF);
        v >>= 8;
    }
}

static int l_struct_unpack(lua_State *L)
{
    const char *fmt = luaL_checkstring(L, 1);
    size_t len = 0;
    const uint8_t *data = (const uint8_t *)luaL_checklstring(L, 2, &len);
    bool le = true;
    size_t pos = 0;

    if (fmt[0] == '<') { le = true; fmt++; }
    else if (fmt[0] == '>') { le = false; fmt++; }

    int fields = 0;
    for (const char *p = fmt; *p; p++) if (struct_field_size(*p) > 0) fields++;
    bool single = (fields == 1);
    if (!single) lua_newtable(L);

    int ret_idx = 0;
    for (const char *p = fmt; *p; p++) {
        int n = struct_field_size(*p);
        if (n <= 0) continue;
        if (pos + (size_t)n > len) return luaL_error(L, "struct.unpack out of bounds");
        uint32_t u = le ? read_le(data + pos, n) : read_be(data + pos, n);
        pos += n;

        switch (*p) {
            case 'b': lua_pushinteger(L, (int8_t)u); break;
            case 'h': lua_pushinteger(L, (int16_t)u); break;
            case 'i':
            case 'l': lua_pushinteger(L, (int32_t)u); break;
            default: lua_pushinteger(L, (lua_Integer)u); break;
        }

        if (!single) {
            lua_rawseti(L, -2, ++ret_idx);
        }
    }
    return 1;
}

static int l_struct_pack(lua_State *L)
{
    const char *fmt = luaL_checkstring(L, 1);
    bool le = true;
    if (fmt[0] == '<') { le = true; fmt++; }
    else if (fmt[0] == '>') { le = false; fmt++; }

    int total = 0;
    for (const char *p = fmt; *p; p++) total += struct_field_size(*p);
    uint8_t *buf = malloc(total);
    if (!buf) return luaL_error(L, "no memory");

    int arg = 2;
    int pos = 0;
    for (const char *p = fmt; *p; p++) {
        int n = struct_field_size(*p);
        if (n <= 0) continue;
        uint32_t v = (uint32_t)luaL_checkinteger(L, arg++);
        if (le) write_le(buf + pos, v, n);
        else write_be(buf + pos, v, n);
        pos += n;
    }

    lua_pushlstring(L, (const char *)buf, total);
    free(buf);
    return 1;
}

static skill_timer_t *find_timer_by_id(int timer_id)
{
    for (int i = 0; i < SKILL_MAX_TIMERS; i++) {
        if (s_timers[i].used && s_timers[i].timer_id == timer_id) return &s_timers[i];
    }
    return NULL;
}

static skill_gpio_intr_t *find_intr_by_id(int intr_id)
{
    for (int i = 0; i < SKILL_MAX_GPIO_INTR; i++) {
        if (s_gpio_intr[i].used && s_gpio_intr[i].intr_id == intr_id) return &s_gpio_intr[i];
    }
    return NULL;
}

static skill_gpio_intr_t *find_intr_by_skill_pin(int skill_id, int pin)
{
    for (int i = 0; i < SKILL_MAX_GPIO_INTR; i++) {
        if (s_gpio_intr[i].used && s_gpio_intr[i].skill_id == skill_id && s_gpio_intr[i].pin == pin) return &s_gpio_intr[i];
    }
    return NULL;
}

static void timer_fire_isr(void *arg)
{
    int timer_id = (int)(intptr_t)arg;
    if (!s_cb_queue) return;
    skill_cb_event_t evt = {.type = 1, .timer_id = timer_id};
    xQueueSend(s_cb_queue, &evt, 0);
}

static void gpio_isr_handler(void *arg)
{
    int intr_id = (int)(intptr_t)arg;
    if (!s_cb_queue) return;
    skill_gpio_intr_t *intr = find_intr_by_id(intr_id);
    if (!intr || !intr->used) return;
    skill_cb_event_t evt = {.type = 2, .intr_id = intr_id, .pin = intr->pin};
    xQueueSendFromISR(s_cb_queue, &evt, NULL);
}

static void timer_cleanup_locked(skill_timer_t *t)
{
    if (!t || !t->used) return;
    if (t->handle) {
        esp_timer_stop(t->handle);
        esp_timer_delete(t->handle);
        t->handle = NULL;
    }
    if (t->lua_cb_ref != LUA_NOREF && s_L) {
        luaL_unref(s_L, LUA_REGISTRYINDEX, t->lua_cb_ref);
        t->lua_cb_ref = LUA_NOREF;
    }
    memset(t, 0, sizeof(*t));
}

static void intr_cleanup_locked(skill_gpio_intr_t *intr)
{
    if (!intr || !intr->used) return;
    gpio_isr_handler_remove(intr->pin);
    if (intr->lua_cb_ref != LUA_NOREF && s_L) {
        luaL_unref(s_L, LUA_REGISTRYINDEX, intr->lua_cb_ref);
        intr->lua_cb_ref = LUA_NOREF;
    }
    memset(intr, 0, sizeof(*intr));
}

static void callback_worker_task(void *arg)
{
    (void)arg;
    skill_cb_event_t evt = {0};
    while (1) {
        if (xQueueReceive(s_cb_queue, &evt, portMAX_DELAY) != pdTRUE) continue;

        if (!lua_lock_take(pdMS_TO_TICKS(200))) continue;
        if (evt.type == 1) {
            skill_timer_t *t = find_timer_by_id(evt.timer_id);
            if (!t || !t->used || t->lua_cb_ref == LUA_NOREF) {
                lua_lock_give();
                continue;
            }

            lua_rawgeti(s_L, LUA_REGISTRYINDEX, t->lua_cb_ref);
            guard_begin();
            int rc = lua_pcall(s_L, 0, 0, 0);
            guard_end();
            if (rc != LUA_OK) {
                const char *err = lua_tostring(s_L, -1);
                ESP_LOGE(TAG, "Timer callback failed (skill=%d,timer=%d): %s",
                         t->skill_id, t->timer_id, err ? err : "unknown");
                lua_pop(s_L, 1);
                if (t->skill_id >= 0 && t->skill_id < SKILL_MAX_SLOTS && s_slots[t->skill_id].used) {
                    s_slots[t->skill_id].state = SKILL_STATE_ERROR;
                }
                timer_cleanup_locked(t);
                lua_lock_give();
                continue;
            }

            if (!t->periodic) {
                timer_cleanup_locked(t);
            }
        } else if (evt.type == 2) {
            skill_gpio_intr_t *intr = find_intr_by_id(evt.intr_id);
            if (!intr || !intr->used || intr->lua_cb_ref == LUA_NOREF) {
                lua_lock_give();
                continue;
            }
            lua_rawgeti(s_L, LUA_REGISTRYINDEX, intr->lua_cb_ref);
            lua_pushinteger(s_L, evt.pin);
            guard_begin();
            int rc = lua_pcall(s_L, 1, 0, 0);
            guard_end();
            if (rc != LUA_OK) {
                const char *err = lua_tostring(s_L, -1);
                ESP_LOGE(TAG, "GPIO callback failed (skill=%d,pin=%d): %s",
                         intr->skill_id, intr->pin, err ? err : "unknown");
                lua_pop(s_L, 1);
                if (intr->skill_id >= 0 && intr->skill_id < SKILL_MAX_SLOTS && s_slots[intr->skill_id].used) {
                    s_slots[intr->skill_id].state = SKILL_STATE_ERROR;
                }
                intr_cleanup_locked(intr);
            }
        }
        lua_lock_give();
    }
}

esp_err_t skill_runtime_init(void)
{
    if (!s_cb_queue) {
        s_cb_queue = xQueueCreate(SKILL_CB_QUEUE_DEPTH, sizeof(skill_cb_event_t));
        if (!s_cb_queue) return ESP_ERR_NO_MEM;
    }
    if (!s_cb_task) {
        BaseType_t ok = xTaskCreatePinnedToCore(callback_worker_task, "skill_cb",
                                                4096, NULL, 4, &s_cb_task, 0);
        if (ok != pdPASS) return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t skill_runtime_register_timer(int skill_id, int period_ms, bool periodic, int lua_cb_ref, int *out_timer_id)
{
    if (skill_id < 0 || skill_id >= SKILL_MAX_SLOTS || period_ms <= 0 || !out_timer_id) return ESP_ERR_INVALID_ARG;
    if (!lua_lock_take(pdMS_TO_TICKS(200))) return ESP_ERR_TIMEOUT;

    int slot = -1;
    for (int i = 0; i < SKILL_MAX_TIMERS; i++) {
        if (!s_timers[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        lua_lock_give();
        return ESP_ERR_NO_MEM;
    }

    int timer_id = s_next_timer_id++;
    if (s_next_timer_id <= 0) s_next_timer_id = 1;

    skill_timer_t *t = &s_timers[slot];
    memset(t, 0, sizeof(*t));
    t->used = true;
    t->timer_id = timer_id;
    t->skill_id = skill_id;
    t->periodic = periodic;
    t->period_ms = period_ms;
    t->lua_cb_ref = lua_cb_ref;

    esp_timer_create_args_t args = {
        .callback = timer_fire_isr,
        .arg = (void *)(intptr_t)timer_id,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "skill_tmr",
    };
    esp_err_t ret = esp_timer_create(&args, &t->handle);
    if (ret == ESP_OK) {
        if (periodic) ret = esp_timer_start_periodic(t->handle, (uint64_t)period_ms * 1000ULL);
        else ret = esp_timer_start_once(t->handle, (uint64_t)period_ms * 1000ULL);
    }
    if (ret != ESP_OK) {
        timer_cleanup_locked(t);
        lua_lock_give();
        return ret;
    }

    *out_timer_id = timer_id;
    lua_lock_give();
    return ESP_OK;
}

esp_err_t skill_runtime_cancel_timer(int timer_id)
{
    if (timer_id <= 0) return ESP_ERR_INVALID_ARG;
    if (!lua_lock_take(pdMS_TO_TICKS(200))) return ESP_ERR_TIMEOUT;
    skill_timer_t *t = find_timer_by_id(timer_id);
    if (!t) {
        lua_lock_give();
        return ESP_ERR_NOT_FOUND;
    }
    timer_cleanup_locked(t);
    lua_lock_give();
    return ESP_OK;
}

esp_err_t skill_runtime_register_gpio_interrupt(int skill_id, int pin, const char *edge, int lua_cb_ref)
{
    if (skill_id < 0 || skill_id >= SKILL_MAX_SLOTS || pin < 0 || !edge || !edge[0]) return ESP_ERR_INVALID_ARG;
    if (!lua_lock_take(pdMS_TO_TICKS(300))) return ESP_ERR_TIMEOUT;

    if (find_intr_by_skill_pin(skill_id, pin)) {
        lua_lock_give();
        return ESP_ERR_INVALID_STATE;
    }

    int slot = -1;
    for (int i = 0; i < SKILL_MAX_GPIO_INTR; i++) {
        if (!s_gpio_intr[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        lua_lock_give();
        return ESP_ERR_NO_MEM;
    }

    gpio_int_type_t intr_type = GPIO_INTR_ANYEDGE;
    if (strcmp(edge, "rising") == 0) intr_type = GPIO_INTR_POSEDGE;
    else if (strcmp(edge, "falling") == 0) intr_type = GPIO_INTR_NEGEDGE;
    else if (strcmp(edge, "both") == 0) intr_type = GPIO_INTR_ANYEDGE;
    else {
        lua_lock_give();
        return ESP_ERR_INVALID_ARG;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = intr_type,
    };
    esp_err_t ret = gpio_config(&cfg);
    if (ret != ESP_OK) {
        lua_lock_give();
        return ret;
    }

    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        lua_lock_give();
        return ret;
    }

    int intr_id = s_next_intr_id++;
    if (s_next_intr_id <= 0) s_next_intr_id = 1;
    skill_gpio_intr_t *intr = &s_gpio_intr[slot];
    memset(intr, 0, sizeof(*intr));
    intr->used = true;
    intr->intr_id = intr_id;
    intr->skill_id = skill_id;
    intr->pin = pin;
    intr->lua_cb_ref = lua_cb_ref;

    ret = gpio_isr_handler_add(pin, gpio_isr_handler, (void *)(intptr_t)intr_id);
    if (ret != ESP_OK) {
        intr_cleanup_locked(intr);
        lua_lock_give();
        return ret;
    }

    lua_lock_give();
    return ESP_OK;
}

esp_err_t skill_runtime_detach_gpio_interrupt(int skill_id, int pin)
{
    if (skill_id < 0 || skill_id >= SKILL_MAX_SLOTS || pin < 0) return ESP_ERR_INVALID_ARG;
    if (!lua_lock_take(pdMS_TO_TICKS(300))) return ESP_ERR_TIMEOUT;
    skill_gpio_intr_t *intr = find_intr_by_skill_pin(skill_id, pin);
    if (!intr) {
        lua_lock_give();
        return ESP_ERR_NOT_FOUND;
    }
    intr_cleanup_locked(intr);
    lua_lock_give();
    return ESP_OK;
}

void skill_runtime_release_skill(int skill_id)
{
    if (!lua_lock_take(pdMS_TO_TICKS(300))) return;
    for (int i = 0; i < SKILL_MAX_TIMERS; i++) {
        if (!s_timers[i].used) continue;
        if (s_timers[i].skill_id != skill_id) continue;
        timer_cleanup_locked(&s_timers[i]);
    }
    for (int i = 0; i < SKILL_MAX_GPIO_INTR; i++) {
        if (!s_gpio_intr[i].used) continue;
        if (s_gpio_intr[i].skill_id != skill_id) continue;
        intr_cleanup_locked(&s_gpio_intr[i]);
    }
    lua_lock_give();
}

static void build_safe_stdlib(void)
{
    lua_newtable(s_L);
    const char *funcs[] = {
        "assert", "error", "ipairs", "next", "pairs", "pcall",
        "select", "tonumber", "tostring", "type", "xpcall", NULL
    };
    for (int i = 0; funcs[i]; i++) {
        lua_getglobal(s_L, funcs[i]);
        lua_setfield(s_L, -2, funcs[i]);
    }
    lua_getglobal(s_L, "math"); lua_setfield(s_L, -2, "math");
    lua_getglobal(s_L, "string"); lua_setfield(s_L, -2, "string");
    lua_getglobal(s_L, "table"); lua_setfield(s_L, -2, "table");
    lua_getglobal(s_L, "utf8"); lua_setfield(s_L, -2, "utf8");
    s_safe_stdlib_ref = luaL_ref(s_L, LUA_REGISTRYINDEX);
}

static void push_console_table(int slot_idx)
{
    lua_newtable(s_L);
    lua_pushinteger(s_L, slot_idx);
    lua_pushcclosure(s_L, l_console_log, 1);
    lua_setfield(s_L, -2, "log");
}

static void push_agent_table(int slot_idx)
{
    lua_newtable(s_L);
    lua_pushinteger(s_L, slot_idx);
    lua_pushcclosure(s_L, l_agent_emit_event, 1);
    lua_setfield(s_L, -2, "emit_event");
}

static void push_struct_table(void)
{
    lua_newtable(s_L);
    lua_pushcfunction(s_L, l_struct_pack);
    lua_setfield(s_L, -2, "pack");
    lua_pushcfunction(s_L, l_struct_unpack);
    lua_setfield(s_L, -2, "unpack");
}

static int create_sandbox_env(int slot_idx)
{
    lua_newtable(s_L);
    int env_idx = lua_gettop(s_L);

    lua_newtable(s_L);
    lua_rawgeti(s_L, LUA_REGISTRYINDEX, s_safe_stdlib_ref);
    lua_setfield(s_L, -2, "__index");
    lua_setmetatable(s_L, env_idx);

    skill_hw_api_push_table(s_L, slot_idx, &s_slots[slot_idx].permissions);
    lua_setfield(s_L, env_idx, "hw");
    push_console_table(slot_idx);
    lua_setfield(s_L, env_idx, "console");
    push_agent_table(slot_idx);
    lua_setfield(s_L, env_idx, "agent");
    push_struct_table();
    lua_setfield(s_L, env_idx, "struct");

    int ref = luaL_ref(s_L, LUA_REGISTRYINDEX);
    return ref;
}

static bool read_file_alloc(const char *path, char **out)
{
    *out = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        return false;
    }
    char *buf = calloc(1, (size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return false;
    }
    fread(buf, 1, (size_t)sz, f);
    fclose(f);
    *out = buf;
    return true;
}

static bool has_suffix(const char *s, const char *suffix)
{
    if (!s || !suffix) return false;
    size_t n = strlen(s);
    size_t m = strlen(suffix);
    if (n < m) return false;
    return strcmp(s + n - m, suffix) == 0;
}

static bool is_safe_relpath(const char *p)
{
    if (!p || !p[0]) return false;
    if (p[0] == '/' || p[0] == '\\') return false;
    if (strstr(p, "..")) return false;
    if (strchr(p, '\\')) return false;
    return true;
}

static bool file_exists_regular(const char *path)
{
    struct stat st = {0};
    if (stat(path, &st) != 0) return false;
    return S_ISREG(st.st_mode);
}

static bool file_exists_dir(const char *path)
{
    struct stat st = {0};
    if (stat(path, &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

static bool join_path2(char *dst, size_t dst_size, const char *a, const char *b);

static bool detect_bundle_root_dir(const char *extract_dir, char *out, size_t out_size)
{
    if (!extract_dir || !out || out_size == 0) return false;

    char manifest_path[512];
    if (join_path2(manifest_path, sizeof(manifest_path), extract_dir, "manifest.json") &&
        file_exists_regular(manifest_path)) {
        size_t n = strlen(extract_dir);
        if (n + 1 > out_size) return false;
        memcpy(out, extract_dir, n + 1);
        return true;
    }

    DIR *dir = opendir(extract_dir);
    if (!dir) return false;

    int child_dirs = 0;
    char only_dir[128] = {0};
    struct dirent *ent = NULL;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char child_path[512];
        if (!join_path2(child_path, sizeof(child_path), extract_dir, ent->d_name)) continue;
        if (!file_exists_dir(child_path)) continue;
        child_dirs++;
        if (child_dirs == 1) {
            size_t dlen = strlen(ent->d_name);
            if (dlen >= sizeof(only_dir)) dlen = sizeof(only_dir) - 1;
            memcpy(only_dir, ent->d_name, dlen);
            only_dir[dlen] = '\0';
        }
        if (child_dirs > 1) break;
    }
    closedir(dir);
    if (child_dirs != 1) return false;

    char nested_root[512];
    if (!join_path2(nested_root, sizeof(nested_root), extract_dir, only_dir)) return false;
    if (!join_path2(manifest_path, sizeof(manifest_path), nested_root, "manifest.json")) return false;
    if (!file_exists_regular(manifest_path)) return false;

    size_t n = strlen(nested_root);
    if (n + 1 > out_size) return false;
    memcpy(out, nested_root, n + 1);
    return true;
}

static bool ensure_dir_recursive(const char *path)
{
    if (!path || !path[0]) return false;
    if (file_exists_dir(path)) return true;

    char tmp[512];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(tmp)) return false;
    memcpy(tmp, path, n + 1);

    for (size_t i = 1; i < n; i++) {
        if (tmp[i] != '/') continue;
        tmp[i] = '\0';
        if (tmp[0] && !file_exists_dir(tmp) && mkdir(tmp, 0755) != 0) return false;
        tmp[i] = '/';
    }
    if (!file_exists_dir(tmp) && mkdir(tmp, 0755) != 0) return false;
    return true;
}

static int parse_octal_field(const char *p, size_t n)
{
    int v = 0;
    size_t i = 0;
    while (i < n && (p[i] == ' ' || p[i] == '\0')) i++;
    for (; i < n; i++) {
        char c = p[i];
        if (c < '0' || c > '7') break;
        v = (v << 3) + (c - '0');
    }
    return v;
}

static void copy_tar_field(char *dst, size_t dst_size, const char *src, size_t src_len)
{
    if (!dst || dst_size == 0) return;
    size_t n = 0;
    while (n < src_len && src[n] != '\0') n++;
    if (n >= dst_size) n = dst_size - 1;
    if (n > 0) memcpy(dst, src, n);
    dst[n] = '\0';
}

static bool join_path2(char *dst, size_t dst_size, const char *a, const char *b)
{
    if (!dst || dst_size == 0 || !a || !b) return false;
    size_t la = strlen(a);
    size_t lb = strlen(b);
    if (la == 0 || lb == 0) return false;
    if (la + 1 + lb + 1 > dst_size) return false;
    memcpy(dst, a, la);
    dst[la] = '/';
    memcpy(dst + la + 1, b, lb);
    dst[la + 1 + lb] = '\0';
    return true;
}

static bool build_staging_file_path(char *out, size_t out_size,
                                    const char *staging_dir,
                                    const char *fname,
                                    const char *tag,
                                    const char *suffix_with_dot)
{
    if (!out || out_size == 0 || !staging_dir || !fname || !tag || !suffix_with_dot) return false;
    size_t ls = strlen(staging_dir);
    size_t lf = strlen(fname);
    size_t lt = strlen(tag);
    size_t lx = strlen(suffix_with_dot);
    if (ls == 0 || lf == 0 || lt == 0 || lx == 0) return false;
    size_t need = ls + 1 + lf + 1 + lt + lx + 1;
    if (need > out_size) return false;

    size_t p = 0;
    memcpy(out + p, staging_dir, ls); p += ls;
    out[p++] = '/';
    memcpy(out + p, fname, lf); p += lf;
    out[p++] = '.';
    memcpy(out + p, tag, lt); p += lt;
    memcpy(out + p, suffix_with_dot, lx); p += lx;
    out[p] = '\0';
    return true;
}

static void cleanup_staging_temp(const char *staging_dir)
{
    if (!staging_dir || !staging_dir[0]) return;
    DIR *dir = opendir(staging_dir);
    if (!dir) return;

    struct dirent *ent = NULL;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        bool is_tmp = false;
        if (strstr(ent->d_name, ".part")) is_tmp = true;
        else if (strstr(ent->d_name, ".bak")) is_tmp = true;
        else if (strstr(ent->d_name, ".dir")) is_tmp = true;
        else if (strstr(ent->d_name, ".bakdir")) is_tmp = true;
        if (!is_tmp) continue;

        char full[512];
        if (!join_path2(full, sizeof(full), staging_dir, ent->d_name)) continue;
        remove_path_recursive(full);
    }
    closedir(dir);
}

static bool extract_tar_to_dir(const char *tar_path, const char *out_dir)
{
    if (!tar_path || !out_dir) return false;
    if (!ensure_dir_recursive(out_dir)) return false;

    FILE *f = fopen(tar_path, "rb");
    if (!f) return false;

    uint8_t header[512];
    uint8_t io_buf[512];
    bool ok = true;

    while (ok) {
        size_t r = fread(header, 1, sizeof(header), f);
        if (r != sizeof(header)) {
            ok = false;
            break;
        }

        bool all_zero = true;
        for (size_t i = 0; i < sizeof(header); i++) {
            if (header[i] != 0) {
                all_zero = false;
                break;
            }
        }
        if (all_zero) break;

        const char *name_raw = (const char *)&header[0];
        const char *size_field = (const char *)&header[124];
        const char *typeflag = (const char *)&header[156];
        const char *prefix_raw = (const char *)&header[345];
        char name[101] = {0};
        char prefix[156] = {0};
        copy_tar_field(name, sizeof(name), name_raw, 100);
        copy_tar_field(prefix, sizeof(prefix), prefix_raw, 155);

        char rel[320] = {0};
        if (prefix[0]) {
            if (!join_path2(rel, sizeof(rel), prefix, name)) {
                ok = false;
                break;
            }
        } else {
            size_t ln = strlen(name);
            if (ln == 0 || ln >= sizeof(rel)) {
                ok = false;
                break;
            }
            memcpy(rel, name, ln + 1);
        }

        if (!is_safe_relpath(rel)) {
            ok = false;
            break;
        }

        int file_sz = parse_octal_field(size_field, 12);
        if (file_sz < 0) {
            ok = false;
            break;
        }

        char full[512];
        if (!join_path2(full, sizeof(full), out_dir, rel)) {
            ok = false;
            break;
        }

        char tf = typeflag[0];
        if (tf == '5') {
            if (!ensure_dir_recursive(full)) {
                ok = false;
                break;
            }
        } else if (tf == '0' || tf == '\0') {
            char *slash = strrchr(full, '/');
            if (slash) {
                *slash = '\0';
                if (!ensure_dir_recursive(full)) {
                    ok = false;
                    break;
                }
                *slash = '/';
            }
            FILE *out = fopen(full, "wb");
            if (!out) {
                ok = false;
                break;
            }
            int remain = file_sz;
            while (remain > 0) {
                int chunk = remain > (int)sizeof(io_buf) ? (int)sizeof(io_buf) : remain;
                size_t rr = fread(io_buf, 1, (size_t)chunk, f);
                if (rr != (size_t)chunk) {
                    ok = false;
                    break;
                }
                if (fwrite(io_buf, 1, rr, out) != rr) {
                    ok = false;
                    break;
                }
                remain -= chunk;
            }
            fclose(out);
            if (!ok) break;
            int pad = (512 - (file_sz % 512)) % 512;
            if (pad > 0 && fseek(f, pad, SEEK_CUR) != 0) {
                ok = false;
                break;
            }
            continue;
        }

        int skip = file_sz;
        int pad = (512 - (skip % 512)) % 512;
        if (fseek(f, skip + pad, SEEK_CUR) != 0) {
            ok = false;
            break;
        }
    }

    fclose(f);
    return ok;
}

static uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

typedef enum {
    ZIP_EXTRACT_ERR_NONE = 0,
    ZIP_EXTRACT_ERR_GENERIC = 1,
    ZIP_EXTRACT_ERR_DATA_DESCRIPTOR = 2,
    ZIP_EXTRACT_ERR_METHOD_UNSUPPORTED = 3,
} zip_extract_err_t;

/* Minimal ZIP extractor: supports local-file stream with method=stored(0), no data descriptor(bit3). */
static bool extract_zip_to_dir(const char *zip_path, const char *out_dir, zip_extract_err_t *out_err)
{
    if (out_err) *out_err = ZIP_EXTRACT_ERR_NONE;
    if (!zip_path || !out_dir) return false;
    if (!ensure_dir_recursive(out_dir)) return false;

    FILE *f = fopen(zip_path, "rb");
    if (!f) return false;

    uint8_t hdr[30];
    uint8_t io_buf[512];
    bool ok = true;

    while (ok) {
        size_t r = fread(hdr, 1, sizeof(hdr), f);
        if (r == 0) break; /* EOF */
        if (r != sizeof(hdr)) { ok = false; break; }

        uint32_t sig = rd_le32(hdr);
        if (sig == 0x02014b50U || sig == 0x06054b50U) {
            break; /* central directory or end-of-central-dir */
        }
        if (sig != 0x04034b50U) { ok = false; break; }

        uint16_t gp_flag = rd_le16(&hdr[6]);
        uint16_t method = rd_le16(&hdr[8]);
        uint32_t comp_size = rd_le32(&hdr[18]);
        uint32_t uncomp_size = rd_le32(&hdr[22]);
        uint16_t name_len = rd_le16(&hdr[26]);
        uint16_t extra_len = rd_le16(&hdr[28]);

        if (gp_flag & 0x0008) {
            if (out_err) *out_err = ZIP_EXTRACT_ERR_DATA_DESCRIPTOR;
            ok = false;
            break;
        }
        if (method != 0) {
            if (out_err) *out_err = ZIP_EXTRACT_ERR_METHOD_UNSUPPORTED;
            ok = false;
            break;
        }
        if (comp_size != uncomp_size) { ok = false; break; }
        if (name_len == 0 || name_len > 300) { ok = false; break; }

        char rel[320] = {0};
        if (fread(rel, 1, name_len, f) != name_len) { ok = false; break; }
        rel[name_len] = '\0';
        if (extra_len > 0 && fseek(f, extra_len, SEEK_CUR) != 0) { ok = false; break; }

        if (!is_safe_relpath(rel)) { ok = false; break; }

        char full[512];
        if (!join_path2(full, sizeof(full), out_dir, rel)) { ok = false; break; }

        bool is_dir = false;
        size_t rel_len = strlen(rel);
        if (rel_len > 0 && rel[rel_len - 1] == '/') is_dir = true;

        if (is_dir) {
            if (!ensure_dir_recursive(full)) { ok = false; break; }
            continue;
        }

        char *slash = strrchr(full, '/');
        if (slash) {
            *slash = '\0';
            if (!ensure_dir_recursive(full)) { ok = false; break; }
            *slash = '/';
        }

        FILE *out = fopen(full, "wb");
        if (!out) { ok = false; break; }
        uint32_t remain = comp_size;
        while (remain > 0) {
            uint32_t chunk = remain > sizeof(io_buf) ? (uint32_t)sizeof(io_buf) : remain;
            size_t rr = fread(io_buf, 1, chunk, f);
            if (rr != chunk) { ok = false; break; }
            if (fwrite(io_buf, 1, rr, out) != rr) { ok = false; break; }
            remain -= chunk;
        }
        fclose(out);
        if (!ok) break;
    }

    fclose(f);
    if (!ok && out_err && *out_err == ZIP_EXTRACT_ERR_NONE) *out_err = ZIP_EXTRACT_ERR_GENERIC;
    return ok;
}

static int find_slot_by_skill_name(const char *name)
{
    if (!name || !name[0]) return -1;
    for (int i = 0; i < SKILL_MAX_SLOTS; i++) {
        if (!s_slots[i].used) continue;
        if (strcmp(s_slots[i].name, name) == 0) return i;
    }
    return -1;
}

static int find_free_slot_idx(void)
{
    for (int i = 0; i < SKILL_MAX_SLOTS; i++) {
        if (!s_slots[i].used) return i;
    }
    return -1;
}

static int count_used_slots(void)
{
    int n = 0;
    for (int i = 0; i < SKILL_MAX_SLOTS; i++) {
        if (s_slots[i].used) n++;
    }
    return n;
}

static bool parse_semver3(const char *s, int out[3])
{
    if (!s || !out) return false;
    int a = 0, b = 0, c = 0;
    char tail = '\0';
    int n = sscanf(s, "%d.%d.%d%c", &a, &b, &c, &tail);
    if (n < 3) return false;
    if (a < 0 || b < 0 || c < 0) return false;
    out[0] = a;
    out[1] = b;
    out[2] = c;
    return true;
}

/* returns: -1 old<new, 0 equal/unknown, 1 old>new */
static int compare_versions_old_vs_new(const char *old_v, const char *new_v)
{
    int ov[3] = {0}, nv[3] = {0};
    if (!parse_semver3(old_v, ov) || !parse_semver3(new_v, nv)) return 0;
    for (int i = 0; i < 3; i++) {
        if (ov[i] < nv[i]) return -1;
        if (ov[i] > nv[i]) return 1;
    }
    return 0;
}

static void parse_perm_array(cJSON *obj, const char *key, char out[][16], int *count)
{
    *count = 0;
    cJSON *arr = cJSON_GetObjectItem(obj, key);
    if (!cJSON_IsArray(arr)) return;
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        if (!cJSON_IsString(it)) continue;
        if (*count >= SKILL_MAX_PERM_ITEMS) break;
        snprintf(out[*count], 16, "%s", it->valuestring);
        (*count)++;
    }
}

static bool load_manifest(skill_slot_t *slot, const char *bundle_dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/manifest.json", bundle_dir);
    char *manifest_str = NULL;
    if (!read_file_alloc(path, &manifest_str)) return false;
    cJSON *root = cJSON_Parse(manifest_str);
    free(manifest_str);
    if (!root) return false;

    cJSON *name = cJSON_GetObjectItem(root, "name");
    cJSON *version = cJSON_GetObjectItem(root, "version");
    cJSON *author = cJSON_GetObjectItem(root, "author");
    cJSON *desc = cJSON_GetObjectItem(root, "description");
    cJSON *entry = cJSON_GetObjectItem(root, "entry");
    if (!cJSON_IsString(name) || !cJSON_IsString(entry)) {
        cJSON_Delete(root);
        return false;
    }
    if (!name->valuestring || !name->valuestring[0] ||
        strlen(name->valuestring) >= sizeof(slot->name)) {
        cJSON_Delete(root);
        return false;
    }
    if (!entry->valuestring || !entry->valuestring[0] ||
        strlen(entry->valuestring) >= sizeof(slot->entry) ||
        !is_safe_relpath(entry->valuestring)) {
        cJSON_Delete(root);
        return false;
    }

    snprintf(slot->name, sizeof(slot->name), "%s", name->valuestring);
    snprintf(slot->version, sizeof(slot->version), "%s", cJSON_IsString(version) ? version->valuestring : "1.0.0");
    snprintf(slot->author, sizeof(slot->author), "%s", cJSON_IsString(author) ? author->valuestring : "unknown");
    snprintf(slot->description, sizeof(slot->description), "%s", cJSON_IsString(desc) ? desc->valuestring : "");
    snprintf(slot->entry, sizeof(slot->entry), "%s", entry->valuestring);
    snprintf(slot->root_dir, sizeof(slot->root_dir), "%s", bundle_dir);
    char entry_path[512];
    snprintf(entry_path, sizeof(entry_path), "%s/%s", slot->root_dir, slot->entry);
    if (!file_exists_regular(entry_path)) {
        cJSON_Delete(root);
        return false;
    }

    cJSON *perms = cJSON_GetObjectItem(root, "permissions");
    if (cJSON_IsObject(perms)) {
        parse_perm_array(perms, "i2c", slot->permissions.i2c, &slot->permissions.i2c_count);
        parse_perm_array(perms, "gpio", slot->permissions.gpio, &slot->permissions.gpio_count);
        parse_perm_array(perms, "spi", slot->permissions.spi, &slot->permissions.spi_count);
        parse_perm_array(perms, "uart", slot->permissions.uart, &slot->permissions.uart_count);
        parse_perm_array(perms, "pwm", slot->permissions.pwm, &slot->permissions.pwm_count);
        parse_perm_array(perms, "adc", slot->permissions.adc, &slot->permissions.adc_count);
    }

    cJSON *events = cJSON_GetObjectItem(root, "events");
    if (cJSON_IsArray(events)) {
        cJSON *it = NULL;
        cJSON_ArrayForEach(it, events) {
            if (slot->event_count >= SKILL_MAX_EVENTS_PER_SKILL) break;
            cJSON *ename = cJSON_GetObjectItem(it, "name");
            if (!cJSON_IsString(ename)) continue;
            snprintf(slot->event_names[slot->event_count], sizeof(slot->event_names[slot->event_count]),
                     "%s", ename->valuestring);
            slot->event_count++;
        }
    }

    cJSON *hw_req = cJSON_GetObjectItem(root, "hw_requirements");
    if (cJSON_IsObject(hw_req)) {
        cJSON *i2c_req = cJSON_GetObjectItem(hw_req, "i2c");
        if (cJSON_IsObject(i2c_req)) {
            cJSON *bus = cJSON_GetObjectItem(i2c_req, "bus");
            cJSON *minf = cJSON_GetObjectItem(i2c_req, "min_freq_hz");
            cJSON *maxf = cJSON_GetObjectItem(i2c_req, "max_freq_hz");
            if (cJSON_IsString(bus)) {
                slot->req_i2c_enabled = true;
                snprintf(slot->req_i2c_bus, sizeof(slot->req_i2c_bus), "%s", bus->valuestring);
                slot->req_i2c_min_freq_hz = cJSON_IsNumber(minf) ? minf->valueint : 0;
                slot->req_i2c_max_freq_hz = cJSON_IsNumber(maxf) ? maxf->valueint : 0;
            }
        }
    }
    cJSON_Delete(root);
    return true;
}

static void load_legacy_permissions(skill_slot_t *slot)
{
    snprintf(slot->permissions.i2c[0], 16, "%s", "i2c0");
    slot->permissions.i2c_count = 1;
    snprintf(slot->permissions.uart[0], 16, "%s", "uart1");
    slot->permissions.uart_count = 1;
    for (int i = 0; i < SKILL_MAX_PERM_ITEMS; i++) {
        snprintf(slot->permissions.gpio[i], 16, "%d", i);
        snprintf(slot->permissions.pwm[i], 16, "%d", i);
        snprintf(slot->permissions.adc[i], 16, "%d", i);
    }
    slot->permissions.gpio_count = SKILL_MAX_PERM_ITEMS;
    slot->permissions.pwm_count = SKILL_MAX_PERM_ITEMS;
    slot->permissions.adc_count = SKILL_MAX_PERM_ITEMS;
}

static void push_config_table(skill_slot_t *slot)
{
    char cfg_path[512];
    snprintf(cfg_path, sizeof(cfg_path), "%s/config.json", slot->root_dir);
    char *cfg = NULL;
    if (!read_file_alloc(cfg_path, &cfg)) {
        lua_newtable(s_L);
        return;
    }
    cJSON *root = cJSON_Parse(cfg);
    free(cfg);
    if (!root) {
        lua_newtable(s_L);
        return;
    }
    cjson_to_lua(s_L, root);
    cJSON_Delete(root);
}

static bool schema_subset_valid(cJSON *schema)
{
    if (!schema || !cJSON_IsObject(schema)) return false;
    cJSON *type = cJSON_GetObjectItem(schema, "type");
    if (!cJSON_IsString(type) || strcmp(type->valuestring, "object") != 0) return false;
    cJSON *props = cJSON_GetObjectItem(schema, "properties");
    if (props && !cJSON_IsObject(props)) return false;
    return true;
}

static bool register_tool_ctx(int slot_idx, int tool_idx)
{
    int ctx_idx = s_tool_ctx_count;
    if (ctx_idx >= (int)(sizeof(s_tool_ctx) / sizeof(s_tool_ctx[0]))) return false;
    s_tool_ctx[ctx_idx].slot_idx = slot_idx;
    s_tool_ctx[ctx_idx].tool_idx = tool_idx;
    s_tool_ctx[ctx_idx].used = true;
    s_tool_ctx_count++;
    return true;
}

static esp_err_t lua_tool_execute(int ctx_idx, const char *input_json, char *output, size_t output_size)
{
    if (!lua_lock_take(pdMS_TO_TICKS(500))) {
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"lua lock timeout\"}");
        return ESP_OK;
    }
    if (ctx_idx < 0 || ctx_idx >= s_tool_ctx_count || !s_tool_ctx[ctx_idx].used) {
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"invalid tool context\"}");
        lua_lock_give();
        return ESP_OK;
    }
    int slot_idx = s_tool_ctx[ctx_idx].slot_idx;
    int tool_idx = s_tool_ctx[ctx_idx].tool_idx;
    skill_slot_t *slot = &s_slots[slot_idx];
    if (!slot->used || slot->state != SKILL_STATE_READY) {
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"skill not ready\"}");
        lua_lock_give();
        return ESP_OK;
    }

    lua_rawgeti(s_L, LUA_REGISTRYINDEX, slot->tool_handler_ref[tool_idx]);

    cJSON *args = cJSON_Parse(input_json ? input_json : "{}");
    if (args) {
        cjson_to_lua(s_L, args);
        cJSON_Delete(args);
    } else {
        lua_newtable(s_L);
    }

    guard_begin();
    int rc = lua_pcall(s_L, 1, 1, 0);
    guard_end();
    if (rc != LUA_OK) {
        const char *err = lua_tostring(s_L, -1);
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"%s\"}", err ? err : "lua error");
        lua_pop(s_L, 1);
        slot->state = SKILL_STATE_ERROR;
        lua_lock_give();
        return ESP_OK;
    }

    if (!lua_istable(s_L, -1)) {
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"tool must return object\"}");
        lua_pop(s_L, 1);
        lua_lock_give();
        return ESP_OK;
    }
    if (!lua_table_to_json_buf(s_L, -1, output, output_size)) {
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"failed to encode output\"}");
    }
    lua_pop(s_L, 1);
    lua_lock_give();
    return ESP_OK;
}
#define TRAMPOLINE(N) \
    static esp_err_t lua_trampoline_##N(const char *input, char *output, size_t len) { \
        return lua_tool_execute((N), input, output, len); \
    }

TRAMPOLINE(0)  TRAMPOLINE(1)  TRAMPOLINE(2)  TRAMPOLINE(3)  TRAMPOLINE(4)  TRAMPOLINE(5)
TRAMPOLINE(6)  TRAMPOLINE(7)  TRAMPOLINE(8)  TRAMPOLINE(9)  TRAMPOLINE(10) TRAMPOLINE(11)
TRAMPOLINE(12) TRAMPOLINE(13) TRAMPOLINE(14) TRAMPOLINE(15) TRAMPOLINE(16) TRAMPOLINE(17)
TRAMPOLINE(18) TRAMPOLINE(19) TRAMPOLINE(20) TRAMPOLINE(21) TRAMPOLINE(22) TRAMPOLINE(23)
TRAMPOLINE(24) TRAMPOLINE(25) TRAMPOLINE(26) TRAMPOLINE(27) TRAMPOLINE(28) TRAMPOLINE(29)
TRAMPOLINE(30) TRAMPOLINE(31) TRAMPOLINE(32) TRAMPOLINE(33) TRAMPOLINE(34) TRAMPOLINE(35)
TRAMPOLINE(36) TRAMPOLINE(37) TRAMPOLINE(38) TRAMPOLINE(39) TRAMPOLINE(40) TRAMPOLINE(41)
TRAMPOLINE(42) TRAMPOLINE(43) TRAMPOLINE(44) TRAMPOLINE(45) TRAMPOLINE(46) TRAMPOLINE(47)
TRAMPOLINE(48) TRAMPOLINE(49) TRAMPOLINE(50) TRAMPOLINE(51) TRAMPOLINE(52) TRAMPOLINE(53)
TRAMPOLINE(54) TRAMPOLINE(55) TRAMPOLINE(56) TRAMPOLINE(57) TRAMPOLINE(58) TRAMPOLINE(59)
TRAMPOLINE(60) TRAMPOLINE(61) TRAMPOLINE(62) TRAMPOLINE(63)

typedef esp_err_t (*tool_exec_fn)(const char *, char *, size_t);
static const tool_exec_fn s_trampolines[] = {
    lua_trampoline_0, lua_trampoline_1, lua_trampoline_2, lua_trampoline_3,
    lua_trampoline_4, lua_trampoline_5, lua_trampoline_6, lua_trampoline_7,
    lua_trampoline_8, lua_trampoline_9, lua_trampoline_10, lua_trampoline_11,
    lua_trampoline_12, lua_trampoline_13, lua_trampoline_14, lua_trampoline_15,
    lua_trampoline_16, lua_trampoline_17, lua_trampoline_18, lua_trampoline_19,
    lua_trampoline_20, lua_trampoline_21, lua_trampoline_22, lua_trampoline_23,
    lua_trampoline_24, lua_trampoline_25, lua_trampoline_26, lua_trampoline_27,
    lua_trampoline_28, lua_trampoline_29, lua_trampoline_30, lua_trampoline_31,
    lua_trampoline_32, lua_trampoline_33, lua_trampoline_34, lua_trampoline_35,
    lua_trampoline_36, lua_trampoline_37, lua_trampoline_38, lua_trampoline_39,
    lua_trampoline_40, lua_trampoline_41, lua_trampoline_42, lua_trampoline_43,
    lua_trampoline_44, lua_trampoline_45, lua_trampoline_46, lua_trampoline_47,
    lua_trampoline_48, lua_trampoline_49, lua_trampoline_50, lua_trampoline_51,
    lua_trampoline_52, lua_trampoline_53, lua_trampoline_54, lua_trampoline_55,
    lua_trampoline_56, lua_trampoline_57, lua_trampoline_58, lua_trampoline_59,
    lua_trampoline_60, lua_trampoline_61, lua_trampoline_62, lua_trampoline_63,
};

static bool parse_tools_for_slot(int slot_idx)
{
    skill_slot_t *slot = &s_slots[slot_idx];
    lua_rawgeti(s_L, LUA_REGISTRYINDEX, slot->env_ref);
    lua_getfield(s_L, -1, "TOOLS");
    if (!lua_istable(s_L, -1)) {
        lua_pop(s_L, 2);
        return true;
    }

    int n = (int)lua_rawlen(s_L, -1);
    if (n > SKILL_MAX_TOOLS_PER_SKILL) n = SKILL_MAX_TOOLS_PER_SKILL;
    for (int i = 0; i < n; i++) {
        lua_rawgeti(s_L, -1, i + 1);
        if (!lua_istable(s_L, -1)) {
            lua_pop(s_L, 1);
            continue;
        }

        lua_getfield(s_L, -1, "name");
        const char *name = lua_tostring(s_L, -1);
        lua_pop(s_L, 1);
        lua_getfield(s_L, -1, "description");
        const char *desc = lua_tostring(s_L, -1);
        lua_pop(s_L, 1);
        if (!desc || !desc[0]) {
            lua_getfield(s_L, -1, "desc");
            desc = lua_tostring(s_L, -1);
            lua_pop(s_L, 1);
        }

        char schema_buf[SKILL_MAX_SCHEMA_JSON] = {0};
        bool param_ok = false;
        lua_getfield(s_L, -1, "parameters");
        if (lua_istable(s_L, -1)) {
            param_ok = lua_table_to_json_buf(s_L, -1, schema_buf, sizeof(schema_buf));
        }
        lua_pop(s_L, 1);
        if (!param_ok) {
            lua_getfield(s_L, -1, "schema");
            if (lua_isstring(s_L, -1)) {
                snprintf(schema_buf, sizeof(schema_buf), "%s", lua_tostring(s_L, -1));
                param_ok = true;
            }
            lua_pop(s_L, 1);
        }
        lua_getfield(s_L, -1, "handler");
        bool has_handler = lua_isfunction(s_L, -1);
        int handler_ref = LUA_NOREF;
        if (has_handler) handler_ref = luaL_ref(s_L, LUA_REGISTRYINDEX);
        else lua_pop(s_L, 1);

        bool valid = name && name[0] && desc && desc[0] && param_ok && has_handler;
        if (!valid) {
            if (handler_ref != LUA_NOREF) luaL_unref(s_L, LUA_REGISTRYINDEX, handler_ref);
            lua_pop(s_L, 1);
            continue;
        }

        cJSON *schema = cJSON_Parse(schema_buf);
        if (!schema || !schema_subset_valid(schema)) {
            cJSON_Delete(schema);
            luaL_unref(s_L, LUA_REGISTRYINDEX, handler_ref);
            lua_pop(s_L, 1);
            ESP_LOGW(TAG, "Skill %s tool %s invalid schema, skipped", slot->name, name);
            continue;
        }
        cJSON_Delete(schema);

        int tool_idx = slot->tool_count;
        if (tool_idx >= SKILL_MAX_TOOLS_PER_SKILL) {
            luaL_unref(s_L, LUA_REGISTRYINDEX, handler_ref);
            lua_pop(s_L, 1);
            break;
        }
        if (!register_tool_ctx(slot_idx, tool_idx)) {
            luaL_unref(s_L, LUA_REGISTRYINDEX, handler_ref);
            lua_pop(s_L, 1);
            break;
        }

        snprintf(slot->tool_names[tool_idx], sizeof(slot->tool_names[tool_idx]), "%s", name);
        snprintf(slot->tool_descs[tool_idx], sizeof(slot->tool_descs[tool_idx]), "%s", desc);
        snprintf(slot->tool_schema[tool_idx], sizeof(slot->tool_schema[tool_idx]), "%s", schema_buf);
        slot->tool_handler_ref[tool_idx] = handler_ref;

        int tramp_idx = s_tool_ctx_count - 1;
        if (tramp_idx >= (int)(sizeof(s_trampolines) / sizeof(s_trampolines[0]))) {
            luaL_unref(s_L, LUA_REGISTRYINDEX, handler_ref);
            s_tool_ctx[tramp_idx].used = false;
            lua_pop(s_L, 1);
            break;
        }

        mimi_tool_t t = {
            .name = slot->tool_names[tool_idx],
            .description = slot->tool_descs[tool_idx],
            .input_schema_json = slot->tool_schema[tool_idx],
            .execute = s_trampolines[tramp_idx],
        };
        tool_registry_register(&t);
        slot->tool_count++;
        lua_pop(s_L, 1);
    }

    lua_pop(s_L, 2);
    return true;
}

static bool run_skill_entry(skill_slot_t *slot)
{
    char entry_path[512];
    snprintf(entry_path, sizeof(entry_path), "%s/%s", slot->root_dir, slot->entry[0] ? slot->entry : "main.lua");
    if (luaL_loadfile(s_L, entry_path) != LUA_OK) {
        ESP_LOGE(TAG, "Skill %s load failed: %s", slot->name, lua_tostring(s_L, -1));
        lua_pop(s_L, 1);
        return false;
    }
    lua_rawgeti(s_L, LUA_REGISTRYINDEX, slot->env_ref);
    if (lua_setupvalue(s_L, -2, 1) == NULL) {
        lua_pop(s_L, 1);
    }
    if (lua_pcall(s_L, 0, 0, 0) != LUA_OK) {
        ESP_LOGE(TAG, "Skill %s run failed: %s", slot->name, lua_tostring(s_L, -1));
        lua_pop(s_L, 1);
        return false;
    }
    return true;
}

static void unload_slot(int idx)
{
    skill_slot_t *slot = &s_slots[idx];
    if (!slot->used) return;
    skill_runtime_release_skill(idx);
    if (!lua_lock_take(pdMS_TO_TICKS(500))) {
        ESP_LOGW(TAG, "Failed to take lua lock during unload for slot %d", idx);
        return;
    }
    for (int i = 0; i < slot->tool_count; i++) {
        tool_registry_unregister(slot->tool_names[i]);
        if (slot->tool_handler_ref[i] != LUA_NOREF) {
            luaL_unref(s_L, LUA_REGISTRYINDEX, slot->tool_handler_ref[i]);
            slot->tool_handler_ref[i] = LUA_NOREF;
        }
    }
    if (slot->env_ref != LUA_NOREF) {
        luaL_unref(s_L, LUA_REGISTRYINDEX, slot->env_ref);
        slot->env_ref = LUA_NOREF;
    }
    skill_resmgr_release_all(idx);
    slot->state = SKILL_STATE_UNINSTALLED;
    slot->used = false;
    lua_lock_give();
}

static void remove_path_recursive(const char *path)
{
    struct stat st = {0};
    if (stat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(path);
        if (!dir) return;
        struct dirent *ent = NULL;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            char child[512];
            snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
            remove_path_recursive(child);
        }
        closedir(dir);
        rmdir(path);
    } else {
        remove(path);
    }
}

static bool load_bundle_dir(const char *bundle_dir, int slot_idx)
{
    skill_slot_t *slot = &s_slots[slot_idx];
    memset(slot, 0, sizeof(*slot));
    slot->env_ref = LUA_NOREF;
    for (int i = 0; i < SKILL_MAX_TOOLS_PER_SKILL; i++) slot->tool_handler_ref[i] = LUA_NOREF;

    if (!load_manifest(slot, bundle_dir)) {
        ESP_LOGW(TAG, "Invalid manifest: %s", bundle_dir);
        return false;
    }
    if (find_slot_by_skill_name(slot->name) >= 0) {
        ESP_LOGW(TAG, "Duplicate skill name rejected: %s", slot->name);
        return false;
    }

    if (slot->req_i2c_enabled) {
        int sda = 0, scl = 0, freq = 0;
        if (!board_profile_get_i2c(slot->req_i2c_bus, &sda, &scl, &freq)) {
            ESP_LOGW(TAG, "Skill %s requires missing I2C bus: %s", slot->name, slot->req_i2c_bus);
            return false;
        }
        if (slot->req_i2c_min_freq_hz > 0 && freq < slot->req_i2c_min_freq_hz) {
            ESP_LOGW(TAG, "Skill %s I2C freq too low: %d < %d", slot->name, freq, slot->req_i2c_min_freq_hz);
            return false;
        }
        if (slot->req_i2c_max_freq_hz > 0 && freq > slot->req_i2c_max_freq_hz) {
            ESP_LOGW(TAG, "Skill %s I2C freq too high: %d > %d", slot->name, freq, slot->req_i2c_max_freq_hz);
            return false;
        }
    }

    slot->used = true;
    slot->state = SKILL_STATE_INSTALLED;
    slot->env_ref = create_sandbox_env(slot_idx);
    slot->state = SKILL_STATE_LOADED;

    if (!run_skill_entry(slot)) {
        slot->state = SKILL_STATE_ERROR;
        return false;
    }

    lua_rawgeti(s_L, LUA_REGISTRYINDEX, slot->env_ref);
    lua_getfield(s_L, -1, "init");
    if (lua_isfunction(s_L, -1)) {
        push_config_table(slot);
        guard_begin();
        int rc = lua_pcall(s_L, 1, 1, 0);
        guard_end();
        if (rc != LUA_OK) {
            ESP_LOGE(TAG, "Skill %s init failed: %s", slot->name, lua_tostring(s_L, -1));
            lua_pop(s_L, 1);
            lua_pop(s_L, 1);
            slot->state = SKILL_STATE_ERROR;
            return false;
        }
        lua_pop(s_L, 1);
    } else {
        lua_pop(s_L, 1);
    }
    lua_pop(s_L, 1);

    if (!parse_tools_for_slot(slot_idx)) {
        slot->state = SKILL_STATE_ERROR;
        return false;
    }

    slot->state = SKILL_STATE_READY;
    ESP_LOGI(TAG, "Skill '%s' v%s loaded with %d tools", slot->name, slot->version, slot->tool_count);
    return true;
}

static bool load_legacy_lua_file(const char *filename, int slot_idx)
{
    skill_slot_t *slot = &s_slots[slot_idx];
    memset(slot, 0, sizeof(*slot));
    slot->env_ref = LUA_NOREF;
    for (int i = 0; i < SKILL_MAX_TOOLS_PER_SKILL; i++) slot->tool_handler_ref[i] = LUA_NOREF;
    snprintf(slot->name, sizeof(slot->name), "%s", filename);
    snprintf(slot->version, sizeof(slot->version), "1.0.0");
    snprintf(slot->description, sizeof(slot->description), "legacy lua skill");
    snprintf(slot->root_dir, sizeof(slot->root_dir), "%s", SKILL_DIR);
    snprintf(slot->entry, sizeof(slot->entry), "%s", filename);
    load_legacy_permissions(slot);
    slot->used = true;
    slot->state = SKILL_STATE_INSTALLED;
    slot->env_ref = create_sandbox_env(slot_idx);
    slot->state = SKILL_STATE_LOADED;

    char path[512];
    snprintf(path, sizeof(path), "%s/%s", SKILL_DIR, filename);
    if (luaL_loadfile(s_L, path) != LUA_OK) {
        ESP_LOGE(TAG, "Legacy skill load failed %s: %s", filename, lua_tostring(s_L, -1));
        lua_pop(s_L, 1);
        slot->state = SKILL_STATE_ERROR;
        return false;
    }
    lua_rawgeti(s_L, LUA_REGISTRYINDEX, slot->env_ref);
    if (lua_setupvalue(s_L, -2, 1) == NULL) lua_pop(s_L, 1);
    if (lua_pcall(s_L, 0, 0, 0) != LUA_OK) {
        ESP_LOGE(TAG, "Legacy skill run failed %s: %s", filename, lua_tostring(s_L, -1));
        lua_pop(s_L, 1);
        slot->state = SKILL_STATE_ERROR;
        return false;
    }
    parse_tools_for_slot(slot_idx);
    slot->state = SKILL_STATE_READY;
    return true;
}

static esp_err_t skill_engine_init_impl(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    memset(s_tool_ctx, 0, sizeof(s_tool_ctx));
    memset(s_timers, 0, sizeof(s_timers));
    memset(s_gpio_intr, 0, sizeof(s_gpio_intr));
    s_next_timer_id = 1;
    s_next_intr_id = 1;
    s_slot_count = 0;
    s_tool_ctx_count = 0;
    ESP_ERROR_CHECK(board_profile_init());
    ESP_ERROR_CHECK(skill_resmgr_init());

    if (!s_lua_lock) {
        s_lua_lock = xSemaphoreCreateRecursiveMutex();
        if (!s_lua_lock) return ESP_ERR_NO_MEM;
    }
    if (!s_install_lock) {
        s_install_lock = xSemaphoreCreateMutex();
        if (!s_install_lock) return ESP_ERR_NO_MEM;
    }

    if (!lua_lock_take(pdMS_TO_TICKS(500))) return ESP_ERR_TIMEOUT;

    if (s_L) {
        lua_close(s_L);
        s_L = NULL;
    }
    s_L = luaL_newstate();
    if (!s_L) {
        lua_lock_give();
        return ESP_ERR_NO_MEM;
    }
    luaL_requiref(s_L, "_G", luaopen_base, 1); lua_pop(s_L, 1);
    luaL_requiref(s_L, "table", luaopen_table, 1); lua_pop(s_L, 1);
    luaL_requiref(s_L, "string", luaopen_string, 1); lua_pop(s_L, 1);
    luaL_requiref(s_L, "math", luaopen_math, 1); lua_pop(s_L, 1);
    luaL_requiref(s_L, "utf8", luaopen_utf8, 1); lua_pop(s_L, 1);
    build_safe_stdlib();

    struct stat st = {0};
    if (stat(SKILL_DIR, &st) != 0) mkdir(SKILL_DIR, 0755);

    DIR *dir = opendir(SKILL_DIR);
    if (!dir) {
        lua_lock_give();
        ESP_ERROR_CHECK(skill_runtime_init());
        return ESP_OK;
    }

    struct dirent *ent = NULL;
    while ((ent = readdir(dir)) != NULL && s_slot_count < SKILL_MAX_SLOTS) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", SKILL_DIR, ent->d_name);
        struct stat est = {0};
        if (stat(path, &est) != 0) continue;
        if (S_ISDIR(est.st_mode)) {
            if (load_bundle_dir(path, s_slot_count)) s_slot_count++;
            continue;
        }
        size_t nlen = strlen(ent->d_name);
        if (nlen > 4 && strcmp(ent->d_name + nlen - 4, ".lua") == 0) {
            if (load_legacy_lua_file(ent->d_name, s_slot_count)) s_slot_count++;
        }
    }
    closedir(dir);

    tool_registry_rebuild_json();
    lua_lock_give();

    ESP_ERROR_CHECK(skill_runtime_init());
    ESP_LOGI(TAG, "Single-VM runtime ready: %d skills, %d tools", s_slot_count, s_tool_ctx_count);
    return ESP_OK;
}

typedef struct {
    SemaphoreHandle_t done;
    esp_err_t ret;
} skill_init_ctx_t;

static void skill_init_task(void *arg)
{
    skill_init_ctx_t *ctx = (skill_init_ctx_t *)arg;
    ctx->ret = skill_engine_init_impl();
    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
}

esp_err_t skill_engine_init(void)
{
    skill_init_ctx_t ctx = {0};
    ctx.done = xSemaphoreCreateBinary();
    if (!ctx.done) return ESP_ERR_NO_MEM;

    BaseType_t ok = xTaskCreatePinnedToCore(skill_init_task, "skill_init",
                                            12 * 1024, &ctx, 5, NULL, 0);
    if (ok != pdPASS) {
        vSemaphoreDelete(ctx.done);
        return ESP_FAIL;
    }

    if (xSemaphoreTake(ctx.done, pdMS_TO_TICKS(20000)) != pdTRUE) {
        vSemaphoreDelete(ctx.done);
        return ESP_ERR_TIMEOUT;
    }
    vSemaphoreDelete(ctx.done);
    return ctx.ret;
}

static bool normalize_checksum_hex(const char *in, char out[65])
{
    if (!in || !out) return false;
    size_t n = strlen(in);
    if (n != 64) return false;
    for (size_t i = 0; i < 64; i++) {
        unsigned char c = (unsigned char)in[i];
        if (!isxdigit(c)) return false;
        out[i] = (char)tolower(c);
    }
    out[64] = '\0';
    return true;
}

static void bytes_to_hex_lower(const uint8_t *bytes, size_t n, char *out, size_t out_size)
{
    static const char hex[] = "0123456789abcdef";
    if (!bytes || !out || out_size < (n * 2 + 1)) return;
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = hex[(bytes[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[bytes[i] & 0xF];
    }
    out[n * 2] = '\0';
}

static esp_err_t activate_bundle_from_extracted_dir(const char *extract_dir,
                                                    const char *staging_dir,
                                                    const char *install_tag,
                                                    int free_slot_idx)
{
    char bundle_root[512];
    if (!detect_bundle_root_dir(extract_dir, bundle_root, sizeof(bundle_root))) {
        remove_path_recursive(extract_dir);
        ESP_LOGE(TAG, "Cannot locate bundle root/manifest in package");
        return ESP_ERR_INVALID_ARG;
    }

    skill_slot_t meta = {0};
    meta.env_ref = LUA_NOREF;
    if (!load_manifest(&meta, bundle_root)) {
        remove_path_recursive(extract_dir);
        ESP_LOGE(TAG, "Invalid bundle manifest in package");
        return ESP_ERR_INVALID_ARG;
    }
    int existing_slot = find_slot_by_skill_name(meta.name);
    if (existing_slot >= 0) {
        int cmp = compare_versions_old_vs_new(s_slots[existing_slot].version, meta.version);
        if (cmp > 0) {
            remove_path_recursive(extract_dir);
            ESP_LOGW(TAG, "Reject downgrade for %s: installed=%s incoming=%s",
                     meta.name, s_slots[existing_slot].version, meta.version);
            install_status_step("reject_downgrade");
            return ESP_ERR_INVALID_VERSION;
        }
    }

    install_status_step("activate_bundle");
    char final_dir[512];
    if (snprintf(final_dir, sizeof(final_dir), "%s/%s", SKILL_DIR, meta.name) <= 0 ||
        strlen(final_dir) >= sizeof(final_dir)) {
        remove_path_recursive(extract_dir);
        return ESP_ERR_INVALID_ARG;
    }

    char dir_backup[512];
    if (!build_staging_file_path(dir_backup, sizeof(dir_backup), staging_dir, meta.name, install_tag, ".bakdir")) {
        remove_path_recursive(extract_dir);
        return ESP_ERR_INVALID_ARG;
    }

    bool had_old_dir = file_exists_dir(final_dir);
    remove_path_recursive(dir_backup);
    if (had_old_dir && rename(final_dir, dir_backup) != 0) {
        remove_path_recursive(extract_dir);
        ESP_LOGE(TAG, "Failed to backup existing bundle directory");
        return ESP_FAIL;
    }
    if (rename(bundle_root, final_dir) != 0) {
        if (had_old_dir) (void)rename(dir_backup, final_dir);
        remove_path_recursive(extract_dir);
        ESP_LOGE(TAG, "Failed to place extracted bundle");
        return ESP_FAIL;
    }
    if (strcmp(bundle_root, extract_dir) != 0) {
        remove_path_recursive(extract_dir);
    }

    existing_slot = find_slot_by_skill_name(meta.name);
    int load_slot = free_slot_idx;
    if (existing_slot < 0 && load_slot < 0) {
        remove_path_recursive(final_dir);
        if (had_old_dir) (void)rename(dir_backup, final_dir);
        else remove_path_recursive(dir_backup);
        return ESP_ERR_NO_MEM;
    }
    if (existing_slot >= 0) {
        unload_slot(existing_slot);
        load_slot = existing_slot;
    }

    if (!lua_lock_take(pdMS_TO_TICKS(500))) {
        remove_path_recursive(final_dir);
        if (had_old_dir) (void)rename(dir_backup, final_dir);
        return ESP_ERR_TIMEOUT;
    }
    bool ok_load = load_bundle_dir(final_dir, load_slot);
    lua_lock_give();

    if (ok_load) {
        remove_path_recursive(dir_backup);
        s_slot_count = count_used_slots();
        tool_registry_rebuild_json();
        return ESP_OK;
    }

    remove_path_recursive(final_dir);
    if (had_old_dir) {
        (void)rename(dir_backup, final_dir);
        if (!lua_lock_take(pdMS_TO_TICKS(500))) return ESP_ERR_TIMEOUT;
        bool restored = load_bundle_dir(final_dir, load_slot);
        lua_lock_give();
        if (restored) {
            s_slot_count = count_used_slots();
            tool_registry_rebuild_json();
        }
    } else {
        remove_path_recursive(dir_backup);
    }
    return ESP_FAIL;
}

static esp_err_t skill_engine_install_with_checksum_impl(const char *url, const char *checksum_hex)
{
    if (!url || !url[0]) return ESP_ERR_INVALID_ARG;
    install_status_step("validate");
    int free_slot_idx = find_free_slot_idx();

    char expected_sha256[65] = {0};
    bool verify_checksum = false;
    if (checksum_hex && checksum_hex[0]) {
        if (!normalize_checksum_hex(checksum_hex, expected_sha256)) {
            ESP_LOGE(TAG, "Invalid checksum format (expect 64 hex chars)");
            return ESP_ERR_INVALID_ARG;
        }
        verify_checksum = true;
    }

    const char *fname = strrchr(url, '/');
    fname = fname ? fname + 1 : url;
    size_t fname_len = strlen(fname);
    if (fname_len == 0 || fname_len > 255) {
        ESP_LOGE(TAG, "Invalid skill filename in URL");
        return ESP_ERR_INVALID_ARG;
    }
    if (strchr(fname, '?') || strchr(fname, '#') || strchr(fname, '\\')) {
        ESP_LOGE(TAG, "Invalid skill filename token");
        return ESP_ERR_INVALID_ARG;
    }
    if (!has_suffix(fname, ".lua") && !has_suffix(fname, ".tar") && !has_suffix(fname, ".zip")) {
        ESP_LOGE(TAG, "Unsupported skill format (only .lua/.tar/.zip)");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (has_suffix(fname, ".lua")) install_status_set_package_type("lua");
    else if (has_suffix(fname, ".tar")) install_status_set_package_type("tar");
    else if (has_suffix(fname, ".zip")) install_status_set_package_type("zip");

    char staging_dir[512];
    snprintf(staging_dir, sizeof(staging_dir), "%s/.staging", SKILL_DIR);
    struct stat st = {0};
    if (stat(staging_dir, &st) != 0) {
        mkdir(staging_dir, 0755);
    }
    install_status_step("cleanup_staging");
    cleanup_staging_temp(staging_dir);

    char staging_path[512];
    char out_path[512];
    char backup_path[512];
    char install_tag[24];
    size_t skill_dir_len = strlen(SKILL_DIR);
    size_t out_need = skill_dir_len + 1 + fname_len + 1;
    snprintf(install_tag, sizeof(install_tag), "%08lx%08lx",
             (unsigned long)(esp_timer_get_time() & 0xffffffff),
             (unsigned long)(++s_install_seq));

    if (out_need > sizeof(out_path)) {
        ESP_LOGE(TAG, "Output path too long");
        return ESP_ERR_INVALID_ARG;
    }
    if (!build_staging_file_path(staging_path, sizeof(staging_path), staging_dir, fname, install_tag, ".part")) {
        ESP_LOGE(TAG, "Staging path too long");
        return ESP_ERR_INVALID_ARG;
    }
    if (!build_staging_file_path(backup_path, sizeof(backup_path), staging_dir, fname, install_tag, ".bak")) {
        ESP_LOGE(TAG, "Backup path too long");
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(out_path, SKILL_DIR, skill_dir_len);
    out_path[skill_dir_len] = '/';
    memcpy(out_path + skill_dir_len + 1, fname, fname_len);
    out_path[skill_dir_len + 1 + fname_len] = '\0';

    FILE *f = fopen(staging_path, "wb");
    if (!f) return ESP_FAIL;

    install_status_step("download");
    esp_http_client_config_t cfg = {.url = url, .timeout_ms = 10000};
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        fclose(f);
        esp_http_client_cleanup(client);
        return ret;
    }
    int status = esp_http_client_fetch_headers(client);
    if (status >= 0) {
        int code = esp_http_client_get_status_code(client);
        if (code < 200 || code >= 300) {
            fclose(f);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            remove(staging_path);
            ESP_LOGE(TAG, "Skill download failed, HTTP status=%d", code);
            return ESP_FAIL;
        }
        int64_t content_len = esp_http_client_get_content_length(client);
        if (content_len > 0 && content_len > SKILL_INSTALL_MAX_BYTES) {
            fclose(f);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            remove(staging_path);
            ESP_LOGE(TAG, "Skill download too large: %lld bytes", (long long)content_len);
            return ESP_ERR_NO_MEM;
        }
        install_status_set_total_bytes(content_len);
    }
    mbedtls_sha256_context sha_ctx;
    uint8_t sha_bin[32] = {0};
    char sha_hex[65] = {0};
    if (verify_checksum) {
        mbedtls_sha256_init(&sha_ctx);
        mbedtls_sha256_starts(&sha_ctx, 0);
    }
    char buf[512];
    int total_read = 0;
    int n = 0;
    while ((n = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
        fwrite(buf, 1, n, f);
        total_read += n;
        install_status_add_downloaded(n);
        if (total_read > SKILL_INSTALL_MAX_BYTES) {
            fclose(f);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            remove(staging_path);
            if (verify_checksum) mbedtls_sha256_free(&sha_ctx);
            ESP_LOGE(TAG, "Skill download exceeded max size");
            return ESP_ERR_NO_MEM;
        }
        if (verify_checksum) {
            mbedtls_sha256_update(&sha_ctx, (const unsigned char *)buf, (size_t)n);
        }
    }
    fclose(f);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (n < 0) {
        if (verify_checksum) mbedtls_sha256_free(&sha_ctx);
        remove(staging_path);
        return ESP_FAIL;
    }
    if (verify_checksum) {
        install_status_step("verify_checksum");
        mbedtls_sha256_finish(&sha_ctx, sha_bin);
        mbedtls_sha256_free(&sha_ctx);
        bytes_to_hex_lower(sha_bin, sizeof(sha_bin), sha_hex, sizeof(sha_hex));
        if (strcmp(sha_hex, expected_sha256) != 0) {
            ESP_LOGE(TAG, "Skill checksum mismatch");
            ESP_LOGE(TAG, " expected=%s", expected_sha256);
            ESP_LOGE(TAG, " actual  =%s", sha_hex);
            remove(staging_path);
            return ESP_ERR_INVALID_CRC;
        }
    }
    if (has_suffix(fname, ".lua")) {
        install_status_step("activate_lua");
        bool had_old = file_exists_regular(out_path);
        remove(backup_path);
        if (had_old && rename(out_path, backup_path) != 0) {
            remove(staging_path);
            ESP_LOGE(TAG, "Failed to backup existing skill file");
            return ESP_FAIL;
        }
        if (rename(staging_path, out_path) != 0) {
            if (had_old) (void)rename(backup_path, out_path);
            remove(staging_path);
            return ESP_FAIL;
        }

        int existing_slot = find_slot_by_skill_name(fname);
        int load_slot = free_slot_idx;
        if (existing_slot < 0 && load_slot < 0) {
            remove(out_path);
            if (had_old) (void)rename(backup_path, out_path);
            else remove(backup_path);
            return ESP_ERR_NO_MEM;
        }
        if (existing_slot >= 0) {
            unload_slot(existing_slot);
            load_slot = existing_slot;
        }
        if (!lua_lock_take(pdMS_TO_TICKS(500))) {
            remove(out_path);
            if (had_old) (void)rename(backup_path, out_path);
            else remove(backup_path);
            return ESP_ERR_TIMEOUT;
        }
        bool ok_load = load_legacy_lua_file(fname, load_slot);
        lua_lock_give();
        if (ok_load) {
            remove(backup_path);
            s_slot_count = count_used_slots();
            tool_registry_rebuild_json();
            return ESP_OK;
        }

        remove(out_path);
        if (had_old) {
            (void)rename(backup_path, out_path);
            if (!lua_lock_take(pdMS_TO_TICKS(500))) return ESP_ERR_TIMEOUT;
            bool restored = load_legacy_lua_file(fname, load_slot);
            lua_lock_give();
            if (restored) {
                s_slot_count = count_used_slots();
                tool_registry_rebuild_json();
            }
        } else {
            remove(backup_path);
        }
        return ESP_FAIL;
    }

    if (has_suffix(fname, ".tar") || has_suffix(fname, ".zip")) {
        char extract_dir[512];
        if (!build_staging_file_path(extract_dir, sizeof(extract_dir), staging_dir, "extract", install_tag, ".dir")) {
            remove(staging_path);
            return ESP_ERR_INVALID_ARG;
        }
        remove_path_recursive(extract_dir);
        bool ok_extract = false;
        zip_extract_err_t zip_err = ZIP_EXTRACT_ERR_NONE;
        if (has_suffix(fname, ".tar")) {
            install_status_step("extract_tar");
            ok_extract = extract_tar_to_dir(staging_path, extract_dir);
        } else {
            install_status_step("extract_zip");
            ok_extract = extract_zip_to_dir(staging_path, extract_dir, &zip_err);
        }
        if (!ok_extract) {
            remove(staging_path);
            remove_path_recursive(extract_dir);
            if (has_suffix(fname, ".zip")) {
                if (zip_err == ZIP_EXTRACT_ERR_METHOD_UNSUPPORTED) {
                    install_status_step("zip_method_unsupported");
                    ESP_LOGE(TAG, "ZIP compression method unsupported (only stored method=0)");
                    return ESP_ERR_NOT_SUPPORTED;
                }
                if (zip_err == ZIP_EXTRACT_ERR_DATA_DESCRIPTOR) {
                    install_status_step("zip_data_descriptor_unsupported");
                    ESP_LOGE(TAG, "ZIP data descriptor unsupported");
                    return ESP_ERR_NOT_SUPPORTED;
                }
            }
            ESP_LOGE(TAG, "Package extract failed");
            return ESP_FAIL;
        }
        remove(staging_path);
        return activate_bundle_from_extracted_dir(extract_dir, staging_dir, install_tag, free_slot_idx);
    }

    remove(staging_path);
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t skill_engine_install_with_checksum(const char *url, const char *checksum_hex)
{
    if (!s_install_lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_install_lock, pdMS_TO_TICKS(15000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    install_status_begin(url);
    esp_err_t ret = skill_engine_install_with_checksum_impl(url, checksum_hex);
    install_status_finish(ret);
    xSemaphoreGive(s_install_lock);
    return ret;
}

esp_err_t skill_engine_install(const char *url)
{
    return skill_engine_install_with_checksum(url, NULL);
}

esp_err_t skill_engine_uninstall(const char *name)
{
    if (!name || !name[0]) return ESP_ERR_INVALID_ARG;
    if (!s_install_lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_install_lock, pdMS_TO_TICKS(15000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    for (int i = 0; i < SKILL_MAX_SLOTS; i++) {
        if (!s_slots[i].used) continue;
        if (strcmp(s_slots[i].name, name) != 0) continue;
        char fs_path[512] = {0};
        if (s_slots[i].root_dir[0]) {
            snprintf(fs_path, sizeof(fs_path), "%s", s_slots[i].root_dir);
        } else if (s_slots[i].entry[0]) {
            snprintf(fs_path, sizeof(fs_path), "%s/%s", SKILL_DIR, s_slots[i].entry);
        }
        unload_slot(i);
        if (fs_path[0]) remove_path_recursive(fs_path);
        s_slot_count = count_used_slots();
        tool_registry_rebuild_json();
        ESP_LOGI(TAG, "Skill uninstalled: %s", name);
        xSemaphoreGive(s_install_lock);
        return ESP_OK;
    }
    xSemaphoreGive(s_install_lock);
    return ESP_ERR_NOT_FOUND;
}

char *skill_engine_list_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < SKILL_MAX_SLOTS; i++) {
        if (!s_slots[i].used) continue;
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "name", s_slots[i].name);
        cJSON_AddStringToObject(obj, "version", s_slots[i].version);
        cJSON_AddStringToObject(obj, "description", s_slots[i].description);
        cJSON_AddNumberToObject(obj, "tools", s_slots[i].tool_count);
        cJSON_AddNumberToObject(obj, "state", s_slots[i].state);
        cJSON *events = cJSON_CreateArray();
        for (int e = 0; e < s_slots[i].event_count; e++) {
            cJSON_AddItemToArray(events, cJSON_CreateString(s_slots[i].event_names[e]));
        }
        cJSON_AddItemToObject(obj, "events", events);
        cJSON_AddItemToArray(arr, obj);
    }
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return json;
}

int skill_engine_get_count(void)
{
    return count_used_slots();
}

char *skill_engine_install_status_json(void)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;

    cJSON_AddBoolToObject(obj, "in_progress", s_install_status.in_progress);
    cJSON_AddNumberToObject(obj, "seq", (double)s_install_status.seq);
    cJSON_AddStringToObject(obj, "stage", s_install_status.stage);
    cJSON_AddStringToObject(obj, "package_type", s_install_status.package_type);
    cJSON_AddStringToObject(obj, "url", s_install_status.url);
    cJSON_AddStringToObject(obj, "last_error", s_install_status.last_error);
    cJSON_AddNumberToObject(obj, "total_bytes", (double)s_install_status.total_bytes);
    cJSON_AddNumberToObject(obj, "downloaded_bytes", (double)s_install_status.downloaded_bytes);
    cJSON_AddNumberToObject(obj, "started_us", (double)s_install_status.started_us);
    cJSON_AddNumberToObject(obj, "finished_us", (double)s_install_status.finished_us);
    int64_t end_us = s_install_status.in_progress ? esp_timer_get_time() : s_install_status.finished_us;
    int64_t elapsed_ms = 0;
    if (s_install_status.started_us > 0 && end_us >= s_install_status.started_us) {
        elapsed_ms = (end_us - s_install_status.started_us) / 1000;
    }
    cJSON_AddNumberToObject(obj, "elapsed_ms", (double)elapsed_ms);
    double progress_pct = 0.0;
    if (s_install_status.total_bytes > 0) {
        progress_pct = ((double)s_install_status.downloaded_bytes * 100.0) /
                       (double)s_install_status.total_bytes;
    }
    cJSON_AddNumberToObject(obj, "progress_pct", progress_pct);

    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return json;
}

char *skill_engine_install_capabilities_json(void)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;

    cJSON *ext = cJSON_CreateArray();
    cJSON_AddItemToArray(ext, cJSON_CreateString("lua"));
    cJSON_AddItemToArray(ext, cJSON_CreateString("tar"));
    cJSON_AddItemToArray(ext, cJSON_CreateString("zip"));
    cJSON_AddItemToObject(obj, "supported_extensions", ext);

    cJSON *zip_methods = cJSON_CreateArray();
    cJSON_AddItemToArray(zip_methods, cJSON_CreateString("stored"));
    cJSON_AddItemToObject(obj, "zip_methods", zip_methods);

    cJSON_AddStringToObject(obj, "checksum", "sha256");
    cJSON_AddBoolToObject(obj, "signature_verification", false);
    cJSON_AddStringToObject(obj, "downgrade_policy", "reject_if_installed_newer");
    cJSON_AddNumberToObject(obj, "max_package_bytes", (double)SKILL_INSTALL_MAX_BYTES);
    cJSON_AddNumberToObject(obj, "install_history_max", (double)INSTALL_HISTORY_MAX);

    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return json;
}

char *skill_engine_install_history_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddNumberToObject(root, "count", s_install_history_count);

    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "items", arr);

    for (int i = 0; i < s_install_history_count; i++) {
        int idx = (s_install_history_next - 1 - i + INSTALL_HISTORY_MAX) % INSTALL_HISTORY_MAX;
        install_history_entry_t *e = &s_install_history[idx];
        cJSON *it = cJSON_CreateObject();
        cJSON_AddNumberToObject(it, "seq", (double)e->seq);
        cJSON_AddBoolToObject(it, "success", e->success);
        cJSON_AddStringToObject(it, "stage", e->stage);
        cJSON_AddStringToObject(it, "url", e->url);
        cJSON_AddStringToObject(it, "error", e->error);
        cJSON_AddNumberToObject(it, "started_us", (double)e->started_us);
        cJSON_AddNumberToObject(it, "finished_us", (double)e->finished_us);
        int64_t elapsed_ms = 0;
        if (e->finished_us >= e->started_us && e->started_us > 0) {
            elapsed_ms = (e->finished_us - e->started_us) / 1000;
        }
        cJSON_AddNumberToObject(it, "elapsed_ms", (double)elapsed_ms);
        cJSON_AddItemToArray(arr, it);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

void skill_engine_install_history_clear(void)
{
    memset(s_install_history, 0, sizeof(s_install_history));
    s_install_history_count = 0;
    s_install_history_next = 0;
}
