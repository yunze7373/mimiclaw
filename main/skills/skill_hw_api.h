#pragma once

/*
 * Hardware API for Lua skills.
 *
 * Registers the "hw" table in a Lua state with functions like:
 *   hw.gpio_read(pin)
 *   hw.gpio_write(pin, value)
 *   hw.adc_read(channel)
 *   hw.i2c_init(sda, scl, freq_hz)
 *   hw.i2c_read(addr, reg, len)
 *   hw.i2c_write(addr, reg, data_table)
 *   hw.pwm_set(pin, freq_hz, duty_percent)
 *   hw.pwm_stop(pin)
 *   hw.uart_send(port, data_string)
 *   hw.delay_ms(ms)
 *   hw.log(message)
 */

struct lua_State;

/**
 * Register the hw.* API into the given Lua state.
 */
void skill_hw_api_register(struct lua_State *L);
