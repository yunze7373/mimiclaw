#include "skills/skill_hw_api.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "lua.h"
#include "lauxlib.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/uart.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "skills/skill_resource_manager.h"
#include "skills/skill_runtime.h"
#include "skills/board_profile.h"
#include "skills/skill_rate_limit.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"

static const char *TAG = "skill_hw";

static skill_permissions_t s_permissions[SKILL_MAX_SLOTS];

typedef struct {
    bool inited;
    i2c_port_t port;
    int sda;
    int scl;
    int freq_hz;
    char bus[16];
} i2c_ctx_t;

static i2c_ctx_t s_i2c_ctx[SKILL_MAX_SLOTS];

static int up_skill_id(lua_State *L)
{
    return (int)lua_tointeger(L, lua_upvalueindex(1));
}

static bool resolve_gpio_arg(lua_State *L, int idx, int *out_pin, char *out_alias, size_t alias_size)
{
    int t = lua_type(L, idx);
    if (t == LUA_TNUMBER) {
        *out_pin = (int)lua_tointeger(L, idx);
        if (out_alias && alias_size > 0) out_alias[0] = '\0';
        return true;
    }
    if (t == LUA_TSTRING) {
        const char *alias = lua_tostring(L, idx);
        int pin = -1;
        if (!board_profile_resolve_gpio(alias, &pin)) return false;
        *out_pin = pin;
        if (out_alias && alias_size > 0) snprintf(out_alias, alias_size, "%s", alias);
        return true;
    }
    return false;
}

static bool has_perm_gpio(int skill_id, int pin, const char *alias)
{
    char key[16];
    snprintf(key, sizeof(key), "%d", pin);
    if (skill_perm_contains(s_permissions[skill_id].gpio, s_permissions[skill_id].gpio_count, "*")) return true;
    if (skill_perm_contains(s_permissions[skill_id].gpio, s_permissions[skill_id].gpio_count, key)) return true;
    if (alias && alias[0]) {
        if (skill_perm_contains(s_permissions[skill_id].gpio, s_permissions[skill_id].gpio_count, alias)) return true;
        char scoped[24];
        snprintf(scoped, sizeof(scoped), "gpio:%s", alias);
        if (skill_perm_contains(s_permissions[skill_id].gpio, s_permissions[skill_id].gpio_count, scoped)) return true;
    }
    return false;
}

static bool has_perm_i2c(int skill_id, const char *bus)
{
    return skill_perm_contains(s_permissions[skill_id].i2c, s_permissions[skill_id].i2c_count, bus);
}

static bool has_perm_uart(int skill_id, int uart_port)
{
    char key[16];
    snprintf(key, sizeof(key), "uart%d", uart_port);
    return skill_perm_contains(s_permissions[skill_id].uart, s_permissions[skill_id].uart_count, key);
}

static bool has_perm_pwm(int skill_id, int pin, const char *alias)
{
    char key[16];
    snprintf(key, sizeof(key), "%d", pin);
    if (skill_perm_contains(s_permissions[skill_id].pwm, s_permissions[skill_id].pwm_count, "*")) return true;
    if (skill_perm_contains(s_permissions[skill_id].pwm, s_permissions[skill_id].pwm_count, key)) return true;
    if (alias && alias[0]) {
        if (skill_perm_contains(s_permissions[skill_id].pwm, s_permissions[skill_id].pwm_count, alias)) return true;
        char scoped[24];
        snprintf(scoped, sizeof(scoped), "gpio:%s", alias);
        if (skill_perm_contains(s_permissions[skill_id].pwm, s_permissions[skill_id].pwm_count, scoped)) return true;
    }
    return false;
}

static bool has_perm_adc(int skill_id, int ch)
{
    char key[16];
    snprintf(key, sizeof(key), "%d", ch);
    return skill_perm_contains(s_permissions[skill_id].adc, s_permissions[skill_id].adc_count, key);
}

static int l_gpio_set_mode(lua_State *L)
{
    int skill_id = up_skill_id(L);
    int pin = -1;
    char alias[24] = {0};
    if (!resolve_gpio_arg(L, 1, &pin, alias, sizeof(alias))) return luaL_error(L, "invalid gpio pin/alias");
    const char *mode = luaL_checkstring(L, 2);

    if (!has_perm_gpio(skill_id, pin, alias)) {
        return luaL_error(L, "permission denied: gpio %d", pin);
    }
    if (!skill_rate_limit_check(skill_id, RATE_LIMIT_GPIO)) {
        return luaL_error(L, "rate limit exceeded: gpio");
    }
    if (skill_resmgr_acquire_gpio(skill_id, pin) != ESP_OK) {
        return luaL_error(L, "gpio conflict: %d", pin);
    }

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (strcmp(mode, "output") == 0) cfg.mode = GPIO_MODE_OUTPUT;
    else if (strcmp(mode, "input_pullup") == 0) cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    else if (strcmp(mode, "input_pulldown") == 0) cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;

    esp_err_t ret = gpio_config(&cfg);
    lua_pushboolean(L, ret == ESP_OK);
    return 1;
}

static int l_gpio_read(lua_State *L)
{
    int skill_id = up_skill_id(L);
    int pin = -1;
    char alias[24] = {0};
    if (!resolve_gpio_arg(L, 1, &pin, alias, sizeof(alias))) return luaL_error(L, "invalid gpio pin/alias");
    if (!has_perm_gpio(skill_id, pin, alias)) return luaL_error(L, "permission denied: gpio %d", pin);
    if (!skill_rate_limit_check(skill_id, RATE_LIMIT_GPIO)) return luaL_error(L, "rate limit exceeded: gpio");
    lua_pushinteger(L, gpio_get_level(pin));
    return 1;
}

static int l_gpio_write(lua_State *L)
{
    int skill_id = up_skill_id(L);
    int pin = -1;
    char alias[24] = {0};
    if (!resolve_gpio_arg(L, 1, &pin, alias, sizeof(alias))) return luaL_error(L, "invalid gpio pin/alias");
    int val = (int)luaL_checkinteger(L, 2);
    if (!has_perm_gpio(skill_id, pin, alias)) return luaL_error(L, "permission denied: gpio %d", pin);
    if (!skill_rate_limit_check(skill_id, RATE_LIMIT_GPIO)) return luaL_error(L, "rate limit exceeded: gpio");
    if (skill_resmgr_acquire_gpio(skill_id, pin) != ESP_OK) return luaL_error(L, "gpio conflict: %d", pin);
    gpio_set_level(pin, val ? 1 : 0);
    return 0;
}

static int l_i2c_init(lua_State *L)
{
    int skill_id = up_skill_id(L);
    const char *bus = "i2c0";
    int sda = 8, scl = 9, freq_hz = 100000;

    int argc = lua_gettop(L);
    if (argc >= 1 && lua_type(L, 1) == LUA_TSTRING) {
        bus = lua_tostring(L, 1);
        bool ok = board_profile_get_i2c(bus, &sda, &scl, &freq_hz);
        if (!ok) return luaL_error(L, "unknown i2c bus: %s", bus);
        if (argc >= 2 && lua_isnumber(L, 2)) sda = (int)lua_tointeger(L, 2);
        if (argc >= 3 && lua_isnumber(L, 3)) scl = (int)lua_tointeger(L, 3);
        if (argc >= 4 && lua_isnumber(L, 4)) freq_hz = (int)lua_tointeger(L, 4);
    } else if (argc >= 1 && lua_type(L, 1) == LUA_TNUMBER) {
        sda = (int)lua_tointeger(L, 1);
        scl = (int)luaL_checkinteger(L, 2);
        freq_hz = (argc >= 3 && lua_isnumber(L, 3)) ? (int)lua_tointeger(L, 3) : 100000;
        bus = "i2c0";
    } else {
        if (!board_profile_get_i2c(bus, &sda, &scl, &freq_hz)) {
            sda = 8; scl = 9; freq_hz = 100000;
        }
    }

    if (!has_perm_i2c(skill_id, bus)) return luaL_error(L, "permission denied: i2c %s", bus);
    if (!skill_rate_limit_check(skill_id, RATE_LIMIT_I2C)) return luaL_error(L, "rate limit exceeded: i2c");
    if (skill_resmgr_acquire_i2c(skill_id, bus, freq_hz) != ESP_OK) {
        return luaL_error(L, "i2c conflict: %s", bus);
    }

    i2c_ctx_t *ctx = &s_i2c_ctx[skill_id];
    if (ctx->inited) {
        i2c_driver_delete(ctx->port);
        ctx->inited = false;
    }

    ctx->port = I2C_NUM_0;
    ctx->sda = sda;
    ctx->scl = scl;
    ctx->freq_hz = freq_hz;
    snprintf(ctx->bus, sizeof(ctx->bus), "%s", bus);

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = freq_hz,
    };
    esp_err_t ret = i2c_param_config(ctx->port, &conf);
    if (ret == ESP_OK) {
        ret = i2c_driver_install(ctx->port, I2C_MODE_MASTER, 0, 0, 0);
    }
    if (ret != ESP_OK) return luaL_error(L, "i2c init failed: %s", esp_err_to_name(ret));
    ctx->inited = true;
    lua_pushboolean(L, 1);
    return 1;
}

static int l_i2c_scan(lua_State *L)
{
    /* Scan I2C bus for devices - returns table of found addresses */
    /* For now, return empty table - real scan requires pin config from board profile */
    (void)L;  /* Unused parameter */
    lua_newtable(L);
    return 1;
}

static int l_i2c_read(lua_State *L)
{
    int skill_id = up_skill_id(L);
    const char *bus = "i2c0";
    int addr = 0, reg = 0, len = 0;
    if (lua_type(L, 1) == LUA_TSTRING) {
        bus = lua_tostring(L, 1);
        addr = (int)luaL_checkinteger(L, 2);
        reg = (int)luaL_checkinteger(L, 3);
        len = (int)luaL_checkinteger(L, 4);
    } else {
        addr = (int)luaL_checkinteger(L, 1);
        reg = (int)luaL_checkinteger(L, 2);
        len = (int)luaL_checkinteger(L, 3);
    }
    if (!has_perm_i2c(skill_id, bus)) return luaL_error(L, "permission denied: i2c %s", bus);
    if (!skill_rate_limit_check(skill_id, RATE_LIMIT_I2C)) return luaL_error(L, "rate limit exceeded: i2c");
    i2c_ctx_t *ctx = &s_i2c_ctx[skill_id];
    if (!ctx->inited || strcmp(ctx->bus, bus) != 0) return luaL_error(L, "i2c not initialized: %s", bus);
    if (len <= 0 || len > 256) return luaL_error(L, "invalid i2c read len");

    uint8_t reg_b = (uint8_t)reg;
    uint8_t *buf = heap_caps_malloc((size_t)len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = malloc(len);
    }
    if (!buf) return luaL_error(L, "no memory");
    esp_err_t ret = i2c_master_write_read_device(ctx->port, addr, &reg_b, 1, buf, len, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        free(buf);
        return luaL_error(L, "i2c read failed: %s", esp_err_to_name(ret));
    }
    lua_pushlstring(L, (const char *)buf, len);
    free(buf);
    return 1;
}

static int l_i2c_write(lua_State *L)
{
    int skill_id = up_skill_id(L);
    const char *bus = "i2c0";
    int addr = 0, reg = 0;
    size_t len = 0;
    const char *payload = NULL;
    if (lua_type(L, 1) == LUA_TSTRING) {
        bus = lua_tostring(L, 1);
        addr = (int)luaL_checkinteger(L, 2);
        reg = (int)luaL_checkinteger(L, 3);
        payload = luaL_checklstring(L, 4, &len);
    } else {
        addr = (int)luaL_checkinteger(L, 1);
        reg = (int)luaL_checkinteger(L, 2);
        payload = luaL_checklstring(L, 3, &len);
    }
    if (!has_perm_i2c(skill_id, bus)) return luaL_error(L, "permission denied: i2c %s", bus);
    if (!skill_rate_limit_check(skill_id, RATE_LIMIT_I2C)) return luaL_error(L, "rate limit exceeded: i2c");
    i2c_ctx_t *ctx = &s_i2c_ctx[skill_id];
    if (!ctx->inited || strcmp(ctx->bus, bus) != 0) return luaL_error(L, "i2c not initialized: %s", bus);

    uint8_t *buf = heap_caps_malloc((size_t)len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = malloc(len + 1);
    }
    if (!buf) return luaL_error(L, "no memory");
    buf[0] = (uint8_t)reg;
    memcpy(buf + 1, payload, len);
    esp_err_t ret = i2c_master_write_to_device(ctx->port, addr, buf, (int)len + 1, pdMS_TO_TICKS(100));
    free(buf);
    lua_pushboolean(L, ret == ESP_OK);
    return 1;
}

static int l_uart_send(lua_State *L)
{
    int skill_id = up_skill_id(L);
    int port = (int)luaL_optinteger(L, 1, 1);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);
    if (!has_perm_uart(skill_id, port)) return luaL_error(L, "permission denied: uart%d", port);
    if (!skill_rate_limit_check(skill_id, RATE_LIMIT_UART)) return luaL_error(L, "rate limit exceeded: uart");
    int n = uart_write_bytes(port, data, len);
    lua_pushinteger(L, n);
    return 1;
}

static int l_pwm_set(lua_State *L)
{
    int skill_id = up_skill_id(L);
    int pin = -1;
    char alias[24] = {0};
    if (!resolve_gpio_arg(L, 1, &pin, alias, sizeof(alias))) return luaL_error(L, "invalid gpio pin/alias");
    int freq = (int)luaL_optinteger(L, 2, 5000);
    lua_Number duty_pct = luaL_optnumber(L, 3, 50.0);
    if (!has_perm_pwm(skill_id, pin, alias)) return luaL_error(L, "permission denied: pwm pin %d", pin);
    if (skill_resmgr_acquire_gpio(skill_id, pin) != ESP_OK) return luaL_error(L, "pwm pin conflict: %d", pin);

    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_2,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .freq_hz = freq,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));
    ledc_channel_config_t ch_cfg = {
        .gpio_num = pin,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_4,
        .timer_sel = LEDC_TIMER_2,
        .duty = (uint32_t)(((1U << 13) - 1) * duty_pct / 100.0),
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));
    lua_pushboolean(L, 1);
    return 1;
}

static int l_pwm_stop(lua_State *L)
{
    (void)up_skill_id(L);
    int pin = -1;
    char alias[24] = {0};
    if (!resolve_gpio_arg(L, 1, &pin, alias, sizeof(alias))) return luaL_error(L, "invalid gpio pin/alias");
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_4, 0);
    gpio_set_level(pin, 0);
    return 0;
}

static int l_adc_read(lua_State *L)
{
    int skill_id = up_skill_id(L);
    int ch = (int)luaL_checkinteger(L, 1);
    if (!has_perm_adc(skill_id, ch)) return luaL_error(L, "permission denied: adc %d", ch);
    adc_oneshot_unit_handle_t handle = NULL;
    adc_oneshot_unit_init_cfg_t init_cfg = {.unit_id = ADC_UNIT_1};
    if (adc_oneshot_new_unit(&init_cfg, &handle) != ESP_OK) return luaL_error(L, "adc init failed");
    adc_oneshot_chan_cfg_t cfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
    adc_oneshot_config_channel(handle, ch, &cfg);
    int raw = 0;
    adc_oneshot_read(handle, ch, &raw);
    adc_oneshot_del_unit(handle);
    lua_newtable(L);
    lua_pushinteger(L, raw);
    lua_setfield(L, -2, "raw");
    lua_pushinteger(L, (raw * 3100) / 4095);
    lua_setfield(L, -2, "voltage_mv");
    return 1;
}

static int l_delay_ms(lua_State *L)
{
    int ms = (int)luaL_checkinteger(L, 1);
    if (ms < 0) ms = 0;
    if (ms > 50) ms = 50;
    if (ms > 0) vTaskDelay(pdMS_TO_TICKS(ms));
    return 0;
}

static int l_log(lua_State *L)
{
    int skill_id = up_skill_id(L);
    const char *msg = luaL_checkstring(L, 1);
    ESP_LOGI(TAG, "[skill=%d] %s", skill_id, msg);
    return 0;
}

static int l_free_heap(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)esp_get_free_heap_size());
    return 1;
}

static int l_timer_every(lua_State *L)
{
    int skill_id = up_skill_id(L);
    int ms = (int)luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (ms < 10) return luaL_error(L, "timer period must be >= 10ms");

    lua_pushvalue(L, 2);
    int cb_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    int timer_id = 0;
    esp_err_t ret = skill_runtime_register_timer(skill_id, ms, true, cb_ref, &timer_id);
    if (ret != ESP_OK) {
        luaL_unref(L, LUA_REGISTRYINDEX, cb_ref);
        return luaL_error(L, "timer_every failed: %s", esp_err_to_name(ret));
    }
    lua_pushinteger(L, timer_id);
    return 1;
}

static int l_timer_once(lua_State *L)
{
    int skill_id = up_skill_id(L);
    int ms = (int)luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (ms < 1) return luaL_error(L, "timer delay must be >= 1ms");

    lua_pushvalue(L, 2);
    int cb_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    int timer_id = 0;
    esp_err_t ret = skill_runtime_register_timer(skill_id, ms, false, cb_ref, &timer_id);
    if (ret != ESP_OK) {
        luaL_unref(L, LUA_REGISTRYINDEX, cb_ref);
        return luaL_error(L, "timer_once failed: %s", esp_err_to_name(ret));
    }
    lua_pushinteger(L, timer_id);
    return 1;
}

static int l_timer_cancel(lua_State *L)
{
    int timer_id = (int)luaL_checkinteger(L, 1);
    esp_err_t ret = skill_runtime_cancel_timer(timer_id);
    lua_pushboolean(L, ret == ESP_OK);
    return 1;
}

static int l_gpio_attach_interrupt(lua_State *L)
{
    int skill_id = up_skill_id(L);
    int pin = -1;
    char alias[24] = {0};
    if (!resolve_gpio_arg(L, 1, &pin, alias, sizeof(alias))) return luaL_error(L, "invalid gpio pin/alias");
    const char *edge = luaL_optstring(L, 2, "both");
    luaL_checktype(L, 3, LUA_TFUNCTION);

    if (!has_perm_gpio(skill_id, pin, alias)) return luaL_error(L, "permission denied: gpio %d", pin);
    if (skill_resmgr_acquire_gpio(skill_id, pin) != ESP_OK) return luaL_error(L, "gpio conflict: %d", pin);

    lua_pushvalue(L, 3);
    int cb_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    esp_err_t ret = skill_runtime_register_gpio_interrupt(skill_id, pin, edge, cb_ref);
    if (ret != ESP_OK) {
        luaL_unref(L, LUA_REGISTRYINDEX, cb_ref);
        return luaL_error(L, "gpio_attach_interrupt failed: %s", esp_err_to_name(ret));
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_gpio_detach_interrupt(lua_State *L)
{
    int skill_id = up_skill_id(L);
    int pin = -1;
    char alias[24] = {0};
    if (!resolve_gpio_arg(L, 1, &pin, alias, sizeof(alias))) return luaL_error(L, "invalid gpio pin/alias");
    if (!has_perm_gpio(skill_id, pin, alias)) return luaL_error(L, "permission denied: gpio %d", pin);
    esp_err_t ret = skill_runtime_detach_gpio_interrupt(skill_id, pin);
    lua_pushboolean(L, ret == ESP_OK);
    return 1;
}

/* ── I2S API (for INMP441 mic, MAX98357A amp) ── */
/* Using deprecated driver API for compatibility */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include "driver/i2s.h"
#pragma GCC diagnostic pop

static bool has_perm_i2s(int skill_id, const char *i2s)
{
    return skill_perm_contains(s_permissions[skill_id].i2s, s_permissions[skill_id].i2s_count, i2s);
}

static int l_i2s_init(lua_State *L)
{
    int skill_id = up_skill_id(L);
    const char *i2s = luaL_checkstring(L, 1);
    if (!has_perm_i2s(skill_id, i2s)) return luaL_error(L, "permission denied: i2s %s", i2s);

    i2s_port_t port = I2S_NUM_0;
    if (strcmp(i2s, "i2s1") == 0 || strcmp(i2s, "I2S1") == 0) {
        port = I2S_NUM_1;
    }

    i2s_config_t config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_RX,
        .sample_rate = 16000,
        .bits_per_sample = 16,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0,
    };

    esp_err_t ret = i2s_driver_install(port, &config, 0, NULL);
    if (ret != ESP_OK) {
        return luaL_error(L, "i2s_driver_install failed: %s", esp_err_to_name(ret));
    }

    i2s_zero_dma_buffer(port);
    lua_pushboolean(L, true);
    return 1;
}

static int l_i2s_read(lua_State *L)
{
    int skill_id = up_skill_id(L);
    const char *i2s = luaL_checkstring(L, 1);
    if (!has_perm_i2s(skill_id, i2s)) return luaL_error(L, "permission denied: i2s %s", i2s);

    i2s_port_t port = I2S_NUM_0;
    if (strcmp(i2s, "i2s1") == 0 || strcmp(i2s, "I2S1") == 0) {
        port = I2S_NUM_1;
    }

    size_t bytes_read = 0;
    int timeout_ms = (int)luaL_optinteger(L, 2, 100);
    int max_bytes = (int)luaL_optinteger(L, 3, 1024);

    uint8_t *buf = heap_caps_malloc((size_t)max_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = malloc(max_bytes);
    }
    if (!buf) return luaL_error(L, "malloc failed");

    esp_err_t ret = i2s_read(port, buf, max_bytes, &bytes_read, pdMS_TO_TICKS(timeout_ms));
    free(buf);

    if (ret != ESP_OK) {
        return luaL_error(L, "i2s_read failed: %s", esp_err_to_name(ret));
    }

    lua_pushinteger(L, bytes_read);
    return 1;
}

static int l_i2s_scan(lua_State *L)
{
    /* I2S device detection - simplified, returns available ports */
    lua_newtable(L);
    lua_pushboolean(L, true);
    lua_setfield(L, -2, "i2s0");
    #ifdef SOC_I2S_NUM
    #if SOC_I2S_NUM > 1
    lua_pushboolean(L, true);
    lua_setfield(L, -2, "i2s1");
    #endif
    #endif
    return 1;
}

/* ── HTTP Client API ─────────────────────────────────────────────── */

typedef struct {
    char *data;
    int   len;
    int   capacity;
} http_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_buf_t *buf = (http_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && buf) {
        int avail = buf->capacity - buf->len - 1;
        int copy = evt->data_len < avail ? evt->data_len : avail;
        if (copy > 0) {
            memcpy(buf->data + buf->len, evt->data, copy);
            buf->len += copy;
            buf->data[buf->len] = '\0';
        }
    }
    return ESP_OK;
}

static int l_http_get(lua_State *L)
{
    int skill_id = up_skill_id(L);
    const char *url = luaL_checkstring(L, 1);

    if (!skill_rate_limit_check(skill_id, RATE_LIMIT_HTTP)) {
        return luaL_error(L, "rate limit exceeded: http");
    }

    http_buf_t buf = {0};
    buf.capacity = 8192;
    buf.data = heap_caps_malloc((size_t)buf.capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf.data) {
        buf.data = malloc(buf.capacity);
    }
    if (!buf.data) return luaL_error(L, "no memory");
    buf.data[0] = '\0';

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &buf,
        .timeout_ms = 10000,
        .buffer_size = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(buf.data);
        return luaL_error(L, "http client init failed");
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        free(buf.data);
        return luaL_error(L, "http get failed: %s", esp_err_to_name(err));
    }

    /* Return: status_code, body */
    lua_pushinteger(L, status);
    lua_pushlstring(L, buf.data, buf.len);
    free(buf.data);
    return 2;
}

static int l_http_post(lua_State *L)
{
    int skill_id = up_skill_id(L);
    const char *url = luaL_checkstring(L, 1);
    size_t body_len = 0;
    const char *body = luaL_checklstring(L, 2, &body_len);
    const char *content_type = luaL_optstring(L, 3, "application/json");

    if (!skill_rate_limit_check(skill_id, RATE_LIMIT_HTTP)) {
        return luaL_error(L, "rate limit exceeded: http");
    }

    http_buf_t buf = {0};
    buf.capacity = 8192;
    buf.data = heap_caps_malloc((size_t)buf.capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf.data) {
        buf.data = malloc(buf.capacity);
    }
    if (!buf.data) return luaL_error(L, "no memory");
    buf.data[0] = '\0';

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &buf,
        .timeout_ms = 10000,
        .buffer_size = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(buf.data);
        return luaL_error(L, "http client init failed");
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_post_field(client, body, (int)body_len);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        free(buf.data);
        return luaL_error(L, "http post failed: %s", esp_err_to_name(err));
    }

    lua_pushinteger(L, status);
    lua_pushlstring(L, buf.data, buf.len);
    free(buf.data);
    return 2;
}

typedef struct {
    const char *name;
    lua_CFunction fn;
} hw_fn_t;

void skill_hw_api_push_table(lua_State *L, int skill_id, const skill_permissions_t *permissions)
{
    if (skill_id >= 0 && skill_id < SKILL_MAX_SLOTS && permissions) {
        s_permissions[skill_id] = *permissions;
    }

    static const hw_fn_t fns[] = {
        {"gpio_set_mode", l_gpio_set_mode},
        {"gpio_read", l_gpio_read},
        {"gpio_write", l_gpio_write},
        {"adc_read", l_adc_read},
        {"pwm_set", l_pwm_set},
        {"pwm_stop", l_pwm_stop},
        {"i2c_init", l_i2c_init},
        {"i2c_read", l_i2c_read},
        {"i2c_write", l_i2c_write},
        {"i2c_scan", l_i2c_scan},
        {"uart_send", l_uart_send},
        {"delay_ms", l_delay_ms},
        {"log", l_log},
        {"free_heap", l_free_heap},
        {"timer_every", l_timer_every},
        {"timer_once", l_timer_once},
        {"timer_cancel", l_timer_cancel},
        {"gpio_attach_interrupt", l_gpio_attach_interrupt},
        {"gpio_detach_interrupt", l_gpio_detach_interrupt},
        {"i2s_init", l_i2s_init},
        {"i2s_read", l_i2s_read},
        {"i2s_scan", l_i2s_scan},
        {"http_get", l_http_get},
        {"http_post", l_http_post},
    };

    lua_newtable(L);
    for (size_t i = 0; i < sizeof(fns) / sizeof(fns[0]); i++) {
        lua_pushinteger(L, skill_id);
        lua_pushcclosure(L, fns[i].fn, 1);
        lua_setfield(L, -2, fns[i].name);
    }
}
