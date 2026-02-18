#include "skills/board_profile.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "board_profile";

#define BOARD_PROFILE_PATH          "/spiffs/config/board_profile.json"
#define BOARD_MAX_I2C_BUSES         4
#define BOARD_MAX_GPIO_ALIASES      16
#define BOARD_MAX_RESERVED_PINS     32

typedef struct {
    char name[16];
    int sda;
    int scl;
    int freq_hz;
    bool used;
} board_i2c_t;

typedef struct {
    char name[24];
    int pin;
    bool used;
} board_gpio_alias_t;

static char s_board_id[32] = "default";
static board_i2c_t s_i2c[BOARD_MAX_I2C_BUSES];
static board_gpio_alias_t s_gpio_alias[BOARD_MAX_GPIO_ALIASES];
static int s_reserved[BOARD_MAX_RESERVED_PINS];
static int s_reserved_count = 0;
static bool s_inited = false;

static void set_defaults(void)
{
    memset(s_i2c, 0, sizeof(s_i2c));
    memset(s_gpio_alias, 0, sizeof(s_gpio_alias));
    memset(s_reserved, 0, sizeof(s_reserved));
    s_reserved_count = 0;
    snprintf(s_board_id, sizeof(s_board_id), "xiaozhi_s3_audio");

    /* I2C0 for SSD1306 OLED - GPIO41=SDA, GPIO42=SCL */
    s_i2c[0].used = true;
    snprintf(s_i2c[0].name, sizeof(s_i2c[0].name), "i2c0");
    s_i2c[0].sda = 41;
    s_i2c[0].scl = 42;
    s_i2c[0].freq_hz = 400000;

    /* GPIO Aliases */
    int idx = 0;

    /* RGB LED */
    s_gpio_alias[idx].used = true;
    snprintf(s_gpio_alias[idx].name, sizeof(s_gpio_alias[idx].name), "rgb");
    s_gpio_alias[idx].pin = 38;
    idx++;

    /* Volume buttons */
    s_gpio_alias[idx].used = true;
    snprintf(s_gpio_alias[idx].name, sizeof(s_gpio_alias[idx].name), "vol_down");
    s_gpio_alias[idx].pin = 39;
    idx++;

    s_gpio_alias[idx].used = true;
    snprintf(s_gpio_alias[idx].name, sizeof(s_gpio_alias[idx].name), "vol_up");
    s_gpio_alias[idx].pin = 40;
    idx++;

    /* Reserved pins for hardware peripherals (from HARD_WRITING.md) */
    /* INMP441 Mic: GPIO4=WS, GPIO5=SCK, GPIO6=SD */
    s_reserved[s_reserved_count++] = 4;
    s_reserved[s_reserved_count++] = 5;
    s_reserved[s_reserved_count++] = 6;

    /* MAX98357A Amp: GPIO7=DIN, GPIO15=BCLK, GPIO16=LRC */
    s_reserved[s_reserved_count++] = 7;
    s_reserved[s_reserved_count++] = 15;
    s_reserved[s_reserved_count++] = 16;

    /* SSD1306 OLED: GPIO41=SDA, GPIO42=SCL */
    s_reserved[s_reserved_count++] = 41;
    s_reserved[s_reserved_count++] = 42;

    /* Volume buttons and RGB LED */
    s_reserved[s_reserved_count++] = 38;  /* RGB */
    s_reserved[s_reserved_count++] = 39;  /* Vol- */
    s_reserved[s_reserved_count++] = 40;  /* Vol+ */
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

esp_err_t board_profile_init(void)
{
    if (s_inited) return ESP_OK;
    set_defaults();

    char *raw = NULL;
    if (!read_file_alloc(BOARD_PROFILE_PATH, &raw)) {
        ESP_LOGW(TAG, "No board profile file, using defaults: %s", BOARD_PROFILE_PATH);
        s_inited = true;
        return ESP_OK;
    }

    cJSON *root = cJSON_Parse(raw);
    free(raw);
    if (!root) {
        ESP_LOGW(TAG, "Invalid board profile JSON, using defaults");
        s_inited = true;
        return ESP_OK;
    }

    cJSON *bid = cJSON_GetObjectItem(root, "board_id");
    if (cJSON_IsString(bid)) {
        snprintf(s_board_id, sizeof(s_board_id), "%s", bid->valuestring);
    }

    cJSON *reserved = cJSON_GetObjectItem(root, "gpio_reserved");
    if (cJSON_IsArray(reserved)) {
        cJSON *it = NULL;
        cJSON_ArrayForEach(it, reserved) {
            if (!cJSON_IsNumber(it)) continue;
            if (s_reserved_count >= BOARD_MAX_RESERVED_PINS) break;
            s_reserved[s_reserved_count++] = it->valueint;
        }
    }

    cJSON *i2c = cJSON_GetObjectItem(root, "i2c");
    if (cJSON_IsObject(i2c)) {
        int idx = 0;
        cJSON *bus = NULL;
        cJSON_ArrayForEach(bus, i2c) {
            if (idx >= BOARD_MAX_I2C_BUSES) break;
            if (!bus->string || !cJSON_IsObject(bus)) continue;
            cJSON *sda = cJSON_GetObjectItem(bus, "sda");
            cJSON *scl = cJSON_GetObjectItem(bus, "scl");
            cJSON *freq = cJSON_GetObjectItem(bus, "freq_hz");
            if (!cJSON_IsNumber(sda) || !cJSON_IsNumber(scl)) continue;
            s_i2c[idx].used = true;
            snprintf(s_i2c[idx].name, sizeof(s_i2c[idx].name), "%s", bus->string);
            s_i2c[idx].sda = sda->valueint;
            s_i2c[idx].scl = scl->valueint;
            s_i2c[idx].freq_hz = cJSON_IsNumber(freq) ? freq->valueint : 100000;
            idx++;
        }
    }

    cJSON *aliases = cJSON_GetObjectItem(root, "gpio_aliases");
    if (cJSON_IsObject(aliases)) {
        int idx = 0;
        cJSON *it = NULL;
        cJSON_ArrayForEach(it, aliases) {
            if (idx >= BOARD_MAX_GPIO_ALIASES) break;
            if (!it->string || !cJSON_IsNumber(it)) continue;
            s_gpio_alias[idx].used = true;
            snprintf(s_gpio_alias[idx].name, sizeof(s_gpio_alias[idx].name), "%s", it->string);
            s_gpio_alias[idx].pin = it->valueint;
            idx++;
        }
    }

    cJSON_Delete(root);
    s_inited = true;
    ESP_LOGI(TAG, "Board profile loaded: id=%s", s_board_id);
    return ESP_OK;
}

bool board_profile_get_i2c(const char *bus, int *sda, int *scl, int *freq_hz)
{
    if (!s_inited) board_profile_init();
    if (!bus || !bus[0]) bus = "i2c0";
    for (int i = 0; i < BOARD_MAX_I2C_BUSES; i++) {
        if (!s_i2c[i].used) continue;
        if (strcmp(s_i2c[i].name, bus) != 0) continue;
        if (sda) *sda = s_i2c[i].sda;
        if (scl) *scl = s_i2c[i].scl;
        if (freq_hz) *freq_hz = s_i2c[i].freq_hz;
        return true;
    }
    return false;
}

bool board_profile_resolve_gpio(const char *name, int *pin)
{
    if (!s_inited) board_profile_init();
    if (!name || !name[0]) return false;
    for (int i = 0; i < BOARD_MAX_GPIO_ALIASES; i++) {
        if (!s_gpio_alias[i].used) continue;
        if (strcmp(s_gpio_alias[i].name, name) != 0) continue;
        if (pin) *pin = s_gpio_alias[i].pin;
        return true;
    }
    return false;
}

bool board_profile_is_gpio_reserved(int pin)
{
    if (!s_inited) board_profile_init();
    for (int i = 0; i < s_reserved_count; i++) {
        if (s_reserved[i] == pin) return true;
    }
    return false;
}

const char *board_profile_get_id(void)
{
    if (!s_inited) board_profile_init();
    return s_board_id;
}

