#include "skills/skill_engine.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "cJSON.h"
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
#include "tools/tool_registry.h"
#include "bus/message_bus.h"

static const char *TAG = "skill_engine";

#define SKILL_DIR                 "/spiffs/skills"
#define SKILL_MAX_SCHEMA_JSON     512
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

static bool slot_has_declared_event(int slot_idx, const char *event_name)
{
    if (!event_name || !event_name[0] || slot_idx < 0 || slot_idx >= SKILL_MAX_SLOTS) return false;
    skill_slot_t *slot = &s_slots[slot_idx];
    for (int i = 0; i < slot->event_count; i++) {
        if (strcmp(slot->event_names[i], event_name) == 0) return true;
    }
    return false;
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

    snprintf(slot->name, sizeof(slot->name), "%s", name->valuestring);
    snprintf(slot->version, sizeof(slot->version), "%s", cJSON_IsString(version) ? version->valuestring : "1.0.0");
    snprintf(slot->author, sizeof(slot->author), "%s", cJSON_IsString(author) ? author->valuestring : "unknown");
    snprintf(slot->description, sizeof(slot->description), "%s", cJSON_IsString(desc) ? desc->valuestring : "");
    snprintf(slot->entry, sizeof(slot->entry), "%s", entry->valuestring);
    snprintf(slot->root_dir, sizeof(slot->root_dir), "%s", bundle_dir);

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
    ESP_ERROR_CHECK(skill_resmgr_init());

    if (!s_lua_lock) {
        s_lua_lock = xSemaphoreCreateRecursiveMutex();
        if (!s_lua_lock) return ESP_ERR_NO_MEM;
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

esp_err_t skill_engine_install(const char *url)
{
    if (!url || !url[0]) return ESP_ERR_INVALID_ARG;
    if (s_slot_count >= SKILL_MAX_SLOTS) return ESP_ERR_NO_MEM;

    const char *fname = strrchr(url, '/');
    fname = fname ? fname + 1 : url;
    char out_path[512];
    snprintf(out_path, sizeof(out_path), "%s/%s", SKILL_DIR, fname);

    FILE *f = fopen(out_path, "wb");
    if (!f) return ESP_FAIL;

    esp_http_client_config_t cfg = {.url = url, .timeout_ms = 10000};
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        fclose(f);
        esp_http_client_cleanup(client);
        return ret;
    }
    char buf[512];
    int n = 0;
    while ((n = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
        fwrite(buf, 1, n, f);
    }
    fclose(f);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (strstr(fname, ".lua")) {
        if (!lua_lock_take(pdMS_TO_TICKS(500))) return ESP_ERR_TIMEOUT;
        bool ok_load = load_legacy_lua_file(fname, s_slot_count);
        lua_lock_give();
        if (ok_load) {
            s_slot_count++;
            tool_registry_rebuild_json();
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

esp_err_t skill_engine_uninstall(const char *name)
{
    if (!name || !name[0]) return ESP_ERR_INVALID_ARG;
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
        tool_registry_rebuild_json();
        ESP_LOGI(TAG, "Skill uninstalled: %s", name);
        return ESP_OK;
    }
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
    return s_slot_count;
}
