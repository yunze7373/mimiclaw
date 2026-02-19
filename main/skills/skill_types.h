#pragma once

#include <stdbool.h>
#include <string.h>

#define SKILL_MAX_SLOTS             4
#define SKILL_MAX_TOOLS_PER_SKILL   8
#define SKILL_MAX_EVENTS_PER_SKILL  8
#define SKILL_MAX_PERM_ITEMS        8

/* ── Skill Taxonomy ───────────────────────────────────────────── */

typedef enum {
    SKILL_CAT_UNKNOWN   = 0,
    SKILL_CAT_SENSOR,       /* Reads data from hardware */
    SKILL_CAT_ACTUATOR,     /* Controls hardware output */
    SKILL_CAT_PROTOCOL,     /* Communication protocol bridge */
    SKILL_CAT_UTILITY,      /* Software-only utility */
    SKILL_CAT_SYSTEM,       /* System-level management */
} skill_category_t;

typedef enum {
    SKILL_TYPE_UNKNOWN     = 0,
    SKILL_TYPE_DRIVER,      /* Direct hardware driver */
    SKILL_TYPE_INTEGRATION, /* External service integration */
    SKILL_TYPE_AUTOMATION,  /* Rule/timer-based automation */
    SKILL_TYPE_TOOL,        /* Exposes LLM-callable tools */
} skill_type_t;

typedef enum {
    SKILL_BUS_NONE  = 0,
    SKILL_BUS_I2C,
    SKILL_BUS_SPI,
    SKILL_BUS_UART,
    SKILL_BUS_GPIO,
    SKILL_BUS_BLE,
    SKILL_BUS_WIFI,
    SKILL_BUS_I2S,
    SKILL_BUS_RMT,
} skill_bus_t;

static inline skill_category_t skill_category_from_str(const char *s)
{
    if (!s) return SKILL_CAT_UNKNOWN;
    if (strcmp(s, "sensor") == 0)   return SKILL_CAT_SENSOR;
    if (strcmp(s, "actuator") == 0) return SKILL_CAT_ACTUATOR;
    if (strcmp(s, "protocol") == 0) return SKILL_CAT_PROTOCOL;
    if (strcmp(s, "utility") == 0)  return SKILL_CAT_UTILITY;
    if (strcmp(s, "system") == 0)   return SKILL_CAT_SYSTEM;
    return SKILL_CAT_UNKNOWN;
}

static inline skill_type_t skill_type_from_str(const char *s)
{
    if (!s) return SKILL_TYPE_UNKNOWN;
    if (strcmp(s, "driver") == 0)      return SKILL_TYPE_DRIVER;
    if (strcmp(s, "integration") == 0) return SKILL_TYPE_INTEGRATION;
    if (strcmp(s, "automation") == 0)  return SKILL_TYPE_AUTOMATION;
    if (strcmp(s, "tool") == 0)        return SKILL_TYPE_TOOL;
    return SKILL_TYPE_UNKNOWN;
}

static inline skill_bus_t skill_bus_from_str(const char *s)
{
    if (!s) return SKILL_BUS_NONE;
    if (strcmp(s, "i2c") == 0)  return SKILL_BUS_I2C;
    if (strcmp(s, "spi") == 0)  return SKILL_BUS_SPI;
    if (strcmp(s, "uart") == 0) return SKILL_BUS_UART;
    if (strcmp(s, "gpio") == 0) return SKILL_BUS_GPIO;
    if (strcmp(s, "ble") == 0)  return SKILL_BUS_BLE;
    if (strcmp(s, "wifi") == 0) return SKILL_BUS_WIFI;
    if (strcmp(s, "i2s") == 0)  return SKILL_BUS_I2S;
    if (strcmp(s, "rmt") == 0)  return SKILL_BUS_RMT;
    return SKILL_BUS_NONE;
}

static inline const char *skill_category_str(skill_category_t c)
{
    switch (c) {
        case SKILL_CAT_SENSOR:   return "sensor";
        case SKILL_CAT_ACTUATOR: return "actuator";
        case SKILL_CAT_PROTOCOL: return "protocol";
        case SKILL_CAT_UTILITY:  return "utility";
        case SKILL_CAT_SYSTEM:   return "system";
        default:                 return "unknown";
    }
}

static inline const char *skill_type_str(skill_type_t t)
{
    switch (t) {
        case SKILL_TYPE_DRIVER:      return "driver";
        case SKILL_TYPE_INTEGRATION: return "integration";
        case SKILL_TYPE_AUTOMATION:  return "automation";
        case SKILL_TYPE_TOOL:        return "tool";
        default:                     return "unknown";
    }
}

static inline const char *skill_bus_str(skill_bus_t b)
{
    switch (b) {
        case SKILL_BUS_I2C:  return "i2c";
        case SKILL_BUS_SPI:  return "spi";
        case SKILL_BUS_UART: return "uart";
        case SKILL_BUS_GPIO: return "gpio";
        case SKILL_BUS_BLE:  return "ble";
        case SKILL_BUS_WIFI: return "wifi";
        case SKILL_BUS_I2S:  return "i2s";
        case SKILL_BUS_RMT:  return "rmt";
        default:             return "none";
    }
}

typedef enum {
    SKILL_STATE_INSTALLED = 0,
    SKILL_STATE_LOADED,
    SKILL_STATE_READY,
    SKILL_STATE_ERROR,
    SKILL_STATE_DISABLED,
    SKILL_STATE_UNINSTALLED,
} skill_state_t;

typedef struct {
    char i2c[SKILL_MAX_PERM_ITEMS][16];
    int i2c_count;
    char gpio[SKILL_MAX_PERM_ITEMS][16];
    int gpio_count;
    char spi[SKILL_MAX_PERM_ITEMS][16];
    int spi_count;
    char uart[SKILL_MAX_PERM_ITEMS][16];
    int uart_count;
    char pwm[SKILL_MAX_PERM_ITEMS][16];
    int pwm_count;
    char adc[SKILL_MAX_PERM_ITEMS][16];
    int adc_count;
    char i2s[SKILL_MAX_PERM_ITEMS][16];
    int i2s_count;
    char rmt[SKILL_MAX_PERM_ITEMS][16];
    int rmt_count;
} skill_permissions_t;

static inline bool skill_perm_contains(const char list[][16], int count, const char *value)
{
    if (!value || !value[0]) return false;
    for (int i = 0; i < count; i++) {
        if (list[i][0] && strcmp(list[i], value) == 0) {
            return true;
        }
    }
    return false;
}
