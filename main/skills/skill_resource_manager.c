#include "skills/skill_resource_manager.h"

#include <string.h>
#include <stdbool.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "skills/board_profile.h"

static const char *TAG = "skill_res";

#define MAX_I2C_BUSES 4

typedef struct {
    bool used;
    int owner_skill;
} gpio_claim_t;

typedef struct {
    bool used;
    char name[16];
    int owner_skill;
    int freq_hz;
} i2c_claim_t;

static gpio_claim_t s_gpio[GPIO_NUM_MAX];
static i2c_claim_t s_i2c[MAX_I2C_BUSES];
static SemaphoreHandle_t s_lock = NULL;

static int find_i2c_idx(const char *bus)
{
    for (int i = 0; i < MAX_I2C_BUSES; i++) {
        if (s_i2c[i].used && strcmp(s_i2c[i].name, bus) == 0) {
            return i;
        }
    }
    for (int i = 0; i < MAX_I2C_BUSES; i++) {
        if (!s_i2c[i].used) {
            snprintf(s_i2c[i].name, sizeof(s_i2c[i].name), "%s", bus);
            return i;
        }
    }
    return -1;
}

esp_err_t skill_resmgr_init(void)
{
    memset(s_gpio, 0, sizeof(s_gpio));
    memset(s_i2c, 0, sizeof(s_i2c));
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t skill_resmgr_acquire_gpio(int skill_id, int pin)
{
    if (pin < 0 || pin >= GPIO_NUM_MAX) return ESP_ERR_INVALID_ARG;
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    if (board_profile_is_gpio_reserved(pin)) {
        ESP_LOGW(TAG, "GPIO %d is reserved by board profile", pin);
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t ret = ESP_OK;
    if (!s_gpio[pin].used) {
        s_gpio[pin].used = true;
        s_gpio[pin].owner_skill = skill_id;
    } else if (s_gpio[pin].owner_skill != skill_id) {
        ESP_LOGW(TAG, "GPIO conflict pin=%d owner=%d requester=%d",
                 pin, s_gpio[pin].owner_skill, skill_id);
        ret = ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t skill_resmgr_acquire_i2c(int skill_id, const char *bus, int freq_hz)
{
    if (!bus || !bus[0] || freq_hz <= 0) return ESP_ERR_INVALID_ARG;
    if (!s_lock) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t ret = ESP_OK;
    int idx = find_i2c_idx(bus);
    if (idx < 0) {
        ret = ESP_ERR_NO_MEM;
    } else if (!s_i2c[idx].used) {
        s_i2c[idx].used = true;
        s_i2c[idx].owner_skill = skill_id;
        s_i2c[idx].freq_hz = freq_hz;
    } else if (s_i2c[idx].owner_skill != skill_id) {
        if (s_i2c[idx].freq_hz != freq_hz) {
            ESP_LOGW(TAG, "I2C strict conflict bus=%s freq=%d owner=%d/%d requester=%d/%d",
                     bus, freq_hz, s_i2c[idx].owner_skill, s_i2c[idx].freq_hz, skill_id, freq_hz);
            ret = ESP_ERR_INVALID_STATE;
        }
    }
    xSemaphoreGive(s_lock);
    return ret;
}

void skill_resmgr_release_all(int skill_id)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < GPIO_NUM_MAX; i++) {
        if (s_gpio[i].used && s_gpio[i].owner_skill == skill_id) {
            s_gpio[i].used = false;
            s_gpio[i].owner_skill = 0;
        }
    }
    for (int i = 0; i < MAX_I2C_BUSES; i++) {
        if (s_i2c[i].used && s_i2c[i].owner_skill == skill_id) {
            memset(&s_i2c[i], 0, sizeof(s_i2c[i]));
        }
    }
    xSemaphoreGive(s_lock);
}
