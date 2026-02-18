#pragma once

#include <stdbool.h>
#include <string.h>

#define SKILL_MAX_SLOTS             16
#define SKILL_MAX_TOOLS_PER_SKILL   8
#define SKILL_MAX_EVENTS_PER_SKILL  8
#define SKILL_MAX_PERM_ITEMS        8

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
