/*
 * skill_hw_api.c — Hardware API for Lua skills
 *
 * Exposes hw.* functions to Lua scripts so they can interact with
 * GPIO, ADC, I2C, SPI, PWM, UART, and more.
 */

#include "skills/skill_hw_api.h"

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2c.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mimi_config.h"

static const char *TAG = "skill_hw";

/* ── Shared I2C state (lazy init) ─────────────────────── */

static bool s_i2c_initialized = false;
static i2c_port_t s_i2c_port = I2C_NUM_0;

/* ── GPIO ──────────────────────────────────────────────── */

/* hw.gpio_set_mode(pin, mode)
 * mode: "input", "output", "input_pullup", "input_pulldown" */
static int l_gpio_set_mode(lua_State *L)
{
    int pin = (int)luaL_checkinteger(L, 1);
    const char *mode = luaL_checkstring(L, 2);

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << pin),
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    if (strcmp(mode, "output") == 0) {
        io_conf.mode = GPIO_MODE_OUTPUT;
    } else if (strcmp(mode, "input_pullup") == 0) {
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    } else if (strcmp(mode, "input_pulldown") == 0) {
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
    } else {
        io_conf.mode = GPIO_MODE_INPUT;
    }

    esp_err_t ret = gpio_config(&io_conf);
    lua_pushboolean(L, ret == ESP_OK);
    return 1;
}

/* hw.gpio_read(pin) → 0 or 1 */
static int l_gpio_read(lua_State *L)
{
    int pin = (int)luaL_checkinteger(L, 1);
    lua_pushinteger(L, gpio_get_level(pin));
    return 1;
}

/* hw.gpio_write(pin, value) */
static int l_gpio_write(lua_State *L)
{
    int pin = (int)luaL_checkinteger(L, 1);
    int val = (int)luaL_checkinteger(L, 2);
    gpio_set_level(pin, val ? 1 : 0);
    return 0;
}

/* ── ADC ───────────────────────────────────────────────── */

/* hw.adc_read(channel) → {raw=N, voltage_mv=N} */
static int l_adc_read(lua_State *L)
{
    int channel = (int)luaL_checkinteger(L, 1);

    if (channel < 0 || channel > 9) {
        return luaL_error(L, "ADC channel must be 0-9");
    }

    /* One-shot ADC read */
    adc_oneshot_unit_handle_t handle;
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    esp_err_t ret = adc_oneshot_new_unit(&init_cfg, &handle);
    if (ret != ESP_OK) {
        return luaL_error(L, "ADC unit init failed");
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    adc_oneshot_config_channel(handle, channel, &chan_cfg);

    int raw = 0;
    adc_oneshot_read(handle, channel, &raw);

    /* Simple voltage estimate: 3100mV range at 12-bit */
    int voltage_mv = (raw * 3100) / 4095;

    adc_oneshot_del_unit(handle);

    lua_newtable(L);
    lua_pushinteger(L, raw);
    lua_setfield(L, -2, "raw");
    lua_pushinteger(L, voltage_mv);
    lua_setfield(L, -2, "voltage_mv");
    return 1;
}

/* ── PWM ───────────────────────────────────────────────── */

/* hw.pwm_set(pin, freq_hz, duty_percent) */
static int l_pwm_set(lua_State *L)
{
    int pin = (int)luaL_checkinteger(L, 1);
    int freq = (int)luaL_optinteger(L, 2, 5000);
    lua_Number duty_pct = luaL_optnumber(L, 3, 50.0);

    /* Use channels 4-7 for Lua skills (0-3 reserved for C tools) */
    static int s_lua_pwm_pins[4] = {-1, -1, -1, -1};
    int ch = -1;

    /* Find existing or free channel */
    for (int i = 0; i < 4; i++) {
        if (s_lua_pwm_pins[i] == pin) { ch = i; break; }
    }
    if (ch < 0) {
        for (int i = 0; i < 4; i++) {
            if (s_lua_pwm_pins[i] < 0) { ch = i; break; }
        }
    }
    if (ch < 0) {
        return luaL_error(L, "No free Lua PWM channel (max 4)");
    }

    ledc_channel_t ledc_ch = LEDC_CHANNEL_4 + ch;

    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_2, /* Separate timer for Lua skills */
        .duty_resolution = LEDC_TIMER_13_BIT,
        .freq_hz = freq,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer_cfg);

    uint32_t duty_max = (1U << 13) - 1;
    uint32_t duty = (uint32_t)(duty_max * duty_pct / 100.0);

    ledc_channel_config_t ch_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = ledc_ch,
        .timer_sel = LEDC_TIMER_2,
        .gpio_num = pin,
        .duty = duty,
        .hpoint = 0,
    };
    ledc_channel_config(&ch_cfg);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, ledc_ch, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, ledc_ch);

    s_lua_pwm_pins[ch] = pin;
    lua_pushboolean(L, 1);
    return 1;
}

/* hw.pwm_stop(pin) */
static int l_pwm_stop(lua_State *L)
{
    int pin = (int)luaL_checkinteger(L, 1);
    static int s_lua_pwm_pins[4] = {-1, -1, -1, -1};

    for (int i = 0; i < 4; i++) {
        if (s_lua_pwm_pins[i] == pin) {
            ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_4 + i, 0);
            s_lua_pwm_pins[i] = -1;
            break;
        }
    }
    return 0;
}

/* ── I2C ───────────────────────────────────────────────── */

/* hw.i2c_init(sda, scl, freq_hz) */
static int l_i2c_init(lua_State *L)
{
    int sda = (int)luaL_checkinteger(L, 1);
    int scl = (int)luaL_checkinteger(L, 2);
    int freq = (int)luaL_optinteger(L, 3, 100000);

    if (s_i2c_initialized) {
        i2c_driver_delete(s_i2c_port);
        s_i2c_initialized = false;
    }

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = freq,
    };

    esp_err_t ret = i2c_param_config(s_i2c_port, &conf);
    if (ret != ESP_OK) {
        return luaL_error(L, "I2C config failed");
    }

    ret = i2c_driver_install(s_i2c_port, I2C_MODE_MASTER, 0, 0, 0);
    if (ret != ESP_OK) {
        return luaL_error(L, "I2C driver install failed");
    }

    s_i2c_initialized = true;
    lua_pushboolean(L, 1);
    return 1;
}

/* hw.i2c_read(addr, reg, len) → table of bytes */
static int l_i2c_read(lua_State *L)
{
    if (!s_i2c_initialized) {
        return luaL_error(L, "I2C not initialized, call hw.i2c_init() first");
    }

    int addr = (int)luaL_checkinteger(L, 1);
    int reg = (int)luaL_checkinteger(L, 2);
    int len = (int)luaL_checkinteger(L, 3);

    if (len <= 0 || len > 256) {
        return luaL_error(L, "I2C read length must be 1-256");
    }

    uint8_t reg_byte = (uint8_t)reg;
    uint8_t *buf = malloc(len);
    if (!buf) {
        return luaL_error(L, "Out of memory");
    }

    esp_err_t ret = i2c_master_write_read_device(
        s_i2c_port, addr, &reg_byte, 1, buf, len, pdMS_TO_TICKS(100));

    if (ret != ESP_OK) {
        free(buf);
        return luaL_error(L, "I2C read failed: %s", esp_err_to_name(ret));
    }

    lua_newtable(L);
    for (int i = 0; i < len; i++) {
        lua_pushinteger(L, buf[i]);
        lua_rawseti(L, -2, i + 1);
    }

    free(buf);
    return 1;
}

/* hw.i2c_write(addr, reg, data_table) */
static int l_i2c_write(lua_State *L)
{
    if (!s_i2c_initialized) {
        return luaL_error(L, "I2C not initialized, call hw.i2c_init() first");
    }

    int addr = (int)luaL_checkinteger(L, 1);
    int reg = (int)luaL_checkinteger(L, 2);

    luaL_checktype(L, 3, LUA_TTABLE);
    int len = (int)lua_rawlen(L, 3);

    uint8_t *buf = malloc(len + 1);
    if (!buf) {
        return luaL_error(L, "Out of memory");
    }

    buf[0] = (uint8_t)reg;
    for (int i = 0; i < len; i++) {
        lua_rawgeti(L, 3, i + 1);
        buf[i + 1] = (uint8_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }

    esp_err_t ret = i2c_master_write_to_device(
        s_i2c_port, addr, buf, len + 1, pdMS_TO_TICKS(100));

    free(buf);
    lua_pushboolean(L, ret == ESP_OK);
    return 1;
}

/* ── UART ──────────────────────────────────────────────── */

/* hw.uart_send(port, data_string) */
static int l_uart_send(lua_State *L)
{
    int port = (int)luaL_optinteger(L, 1, 1);
    size_t len;
    const char *data = luaL_checklstring(L, 2, &len);

    if (port < 0 || port > 2) {
        return luaL_error(L, "UART port must be 0-2");
    }

    int written = uart_write_bytes(port, data, len);
    lua_pushinteger(L, written);
    return 1;
}

/* ── Utility ───────────────────────────────────────────── */

/* hw.delay_ms(ms) */
static int l_delay_ms(lua_State *L)
{
    int ms = (int)luaL_checkinteger(L, 1);
    if (ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
    return 0;
}

/* hw.log(message) */
static int l_log(lua_State *L)
{
    const char *msg = luaL_checkstring(L, 1);
    ESP_LOGI(TAG, "[Lua] %s", msg);
    return 0;
}

/* hw.free_heap() → internal free bytes */
static int l_free_heap(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)esp_get_free_heap_size());
    return 1;
}

/* ── Registration ──────────────────────────────────────── */

static const luaL_Reg hw_lib[] = {
    /* GPIO */
    {"gpio_set_mode", l_gpio_set_mode},
    {"gpio_read",     l_gpio_read},
    {"gpio_write",    l_gpio_write},
    /* ADC */
    {"adc_read",      l_adc_read},
    /* PWM */
    {"pwm_set",       l_pwm_set},
    {"pwm_stop",      l_pwm_stop},
    /* I2C */
    {"i2c_init",      l_i2c_init},
    {"i2c_read",      l_i2c_read},
    {"i2c_write",     l_i2c_write},
    /* UART */
    {"uart_send",     l_uart_send},
    /* Utility */
    {"delay_ms",      l_delay_ms},
    {"log",           l_log},
    {"free_heap",     l_free_heap},
    /* Sentinel */
    {NULL, NULL},
};

void skill_hw_api_register(lua_State *L)
{
    luaL_newlib(L, hw_lib);
    lua_setglobal(L, "hw");
}
