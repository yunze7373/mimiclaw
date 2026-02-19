#include "tool_hardware.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "driver/uart.h"
#include "driver/temperature_sensor.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "soc/rtc.h"
#include "rgb/rgb.h"
#include "mimi_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s.h"
#include "mbedtls/base64.h"

#define HW_NVS_NAMESPACE "hw_config"

/* Default pin configuration */
static const struct {
    const char *key;
    int default_val;
} s_default_pins[] = {
    {"rgb_pin", 48},
    {"i2c0_sda", 41},
    {"i2c0_scl", 42},
    {"i2s0_ws", 4},
    {"i2s0_sck", 5},
    {"i2s0_sd", 6},
    {"i2s1_din", 7},
    {"i2s1_bclk", 15},
    {"i2s1_lrc", 16},
    {"vol_down", 39},
    {"vol_up", 40},
};

static esp_err_t hw_pins_get_handler(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_FAIL;

    nvs_handle_t nvs;
    if (nvs_open(HW_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        for (size_t i = 0; i < sizeof(s_default_pins)/sizeof(s_default_pins[0]); i++) {
            int32_t val = s_default_pins[i].default_val;
            nvs_get_i32(nvs, s_default_pins[i].key, &val);
            cJSON_AddNumberToObject(root, s_default_pins[i].key, val);
        }
        nvs_close(nvs);
    } else {
        /* Use defaults */
        for (size_t i = 0; i < sizeof(s_default_pins)/sizeof(s_default_pins[0]); i++) {
            cJSON_AddNumberToObject(root, s_default_pins[i].key, s_default_pins[i].default_val);
        }
    }

    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, strlen(out));
    free(out);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t hw_pins_post_handler(httpd_req_t *req) {
    char buf[512];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send(req, "{\"success\":false,\"error\":\"Invalid JSON\"}", -1);
        return ESP_FAIL;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(HW_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        httpd_resp_send(req, "{\"success\":false,\"error\":\"NVS error\"}", -1);
        return ESP_FAIL;
    }

    for (size_t i = 0; i < sizeof(s_default_pins)/sizeof(s_default_pins[0]); i++) {
        cJSON *item = cJSON_GetObjectItem(root, s_default_pins[i].key);
        if (item && cJSON_IsNumber(item)) {
            nvs_set_i32(nvs, s_default_pins[i].key, item->valueint);
        }
    }

    nvs_commit(nvs);
    nvs_close(nvs);
    cJSON_Delete(root);

    httpd_resp_send(req, "{\"success\":true}", -1);
    return ESP_OK;
}

esp_err_t tool_hardware_init(void);

static const char *TAG = "tool_hw";
static temperature_sensor_handle_t temp_handle = NULL;

/* --- ADC handle (oneshot mode) --- */
static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t s_adc_cali = NULL;
static bool s_adc_calibrated = false;

/* --- PWM channel tracking --- */
typedef struct {
    int pin;
    ledc_channel_t channel;
    bool in_use;
} pwm_slot_t;

static pwm_slot_t s_pwm_slots[MIMI_PWM_MAX_CHANNELS] = {0};
static bool s_pwm_timer_init = false;

/* --- Helper: Check Safe Pin --- */
static bool is_safe_pin(int pin) {
    if (pin < 0 || pin > 48) return false;
    /* System pins */
    if (pin == 0) return false; // Boot
    if (pin == 1 || pin == 3) return false; // UART0 (Legacy)
    if (pin == 43 || pin == 44) return false; // UART0 (S3 Default)
    if (pin >= 6 && pin <= 11) return false; // Flash/PSRAM
    if (pin >= 19 && pin <= 20) return false; // USB JTAG
    /* Display / Touch / I2C (based on current board config) */
    /* Restricted pins: 39, 40, 41, 42, 45, 46 (LCD), 47, 48 (I2C/Touch) */
    /* Safe pins for general use: 2, 4, 5, 12, 13, 14, 15, 16, 17, 18, 21, 38 */
    switch (pin) {
        case 2:
        case 4:
        case 5:
        case 12:
        case 13:
        case 14:
        case 15:
        case 16:
        case 17:
        case 18:
        case 21:
        case 38:
        case 48:
            return true;
        default:
            return false; // All other pins are considered unsafe for general control
    }
}

/* --- Helper: Get internal temperature (if supported) --- */
/* --- Helper: Get internal temperature (if supported) --- */
static float get_cpu_temp(void) {
    /* Lazy initialization */
    if (!temp_handle) {
        temperature_sensor_config_t temp_sensor = {
            .range_min = 20,
            .range_max = 100,
            .clk_src = 0,
        };
        if (temperature_sensor_install(&temp_sensor, &temp_handle) == ESP_OK) {
            temperature_sensor_enable(temp_handle);
            ESP_LOGI(TAG, "Temperature sensor initialized (lazy)");
        }
    }

    float tsens_out = 0.0f;
    if (temp_handle) {
        temperature_sensor_get_celsius(temp_handle, &tsens_out);
    }
    return tsens_out;
}

/* --- Tool Implementation --- */

/* Get System Status */
esp_err_t tool_system_status(const char *input, char *output, size_t out_len) {
    cJSON *root = cJSON_CreateObject();

    /* CPU Freq */
    rtc_cpu_freq_config_t conf;
    rtc_clk_cpu_freq_get_config(&conf);
    cJSON_AddNumberToObject(root, "cpu_freq_mhz", conf.freq_mhz);

    /* Detailed Heap Info */
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

    cJSON_AddNumberToObject(root, "free_heap_internal", internal_free);
    cJSON_AddNumberToObject(root, "total_heap_internal", internal_total);
    cJSON_AddNumberToObject(root, "free_heap_psram", psram_free);
    cJSON_AddNumberToObject(root, "total_heap_psram", psram_total);
    cJSON_AddNumberToObject(root, "min_free_heap", esp_get_minimum_free_heap_size());

    /* Largest free block */
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_INTERNAL);
    cJSON_AddNumberToObject(root, "largest_free_block", info.largest_free_block);
    cJSON_AddNumberToObject(root, "allocated_blocks", info.allocated_blocks);
    cJSON_AddNumberToObject(root, "free_blocks", info.free_blocks);

    /* Temp */
    cJSON_AddNumberToObject(root, "cpu_temp_c", get_cpu_temp());

    /* Uptime */
    cJSON_AddNumberToObject(root, "uptime_s", esp_timer_get_time() / 1000000);

    /* Task count */
    cJSON_AddNumberToObject(root, "task_count", uxTaskGetNumberOfTasks());

    /* GPIO States */
    cJSON *gpio_obj = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "gpio", gpio_obj);
    for (int i=0; i<=48; i++) {
        if (is_safe_pin(i)) {
            int level = gpio_get_level(i);
            char pin_str[8];
            snprintf(pin_str, sizeof(pin_str), "%d", i);
            cJSON_AddNumberToObject(gpio_obj, pin_str, level);
        }
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    if (out) {
        snprintf(output, out_len, "%s", out);
        free(out);
        return ESP_OK;
    }
    return ESP_FAIL;
}

/* GPIO Control */
esp_err_t tool_gpio_control(const char *input, char *output, size_t out_len) {
    cJSON *in_json = cJSON_Parse(input);
    if (!in_json) {
        snprintf(output, out_len, "Error: Invalid JSON");
        return ESP_OK; // Return OK so Agent sees the error message
    }

    cJSON *pin_item = cJSON_GetObjectItem(in_json, "pin");
    cJSON *state_item = cJSON_GetObjectItem(in_json, "state");
    
    if (!pin_item || !cJSON_IsNumber(pin_item) || !state_item || !cJSON_IsBool(state_item)) {
        cJSON_Delete(in_json);
        snprintf(output, out_len, "Error: Missing 'pin' (int) or 'state' (bool)");
        return ESP_OK;
    }

    int pin = pin_item->valueint;
    bool state = cJSON_IsTrue(state_item);
    cJSON_Delete(in_json);

    if (!is_safe_pin(pin)) {
        snprintf(output, out_len, "Error: Pin %d is restricted/system/display pin.", pin);
        return ESP_OK;
    }

    /* Configure as output if needed */
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level(pin, state);


    ESP_LOGI(TAG, "Setting GPIO %d to %d", pin, state);
    snprintf(output, out_len, "OK: GPIO %d set to %s", pin, state ? "HIGH (1)" : "LOW (0)");
    return ESP_OK;
}

/* I2C Scan */
static bool s_i2c_scan_initialized = false;

esp_err_t tool_i2c_scan(const char *input, char *output, size_t out_len) {
    (void)input; /* Assuming bus 0 for now */

    /* Lazy-init I2C driver if not already done (e.g. IMU disabled) */
    if (!s_i2c_scan_initialized) {
        i2c_config_t conf = {
            .mode = I2C_MODE_MASTER,
            .sda_io_num = MIMI_PIN_I2C0_SDA,
            .scl_io_num = MIMI_PIN_I2C0_SCL,
            .sda_pullup_en = GPIO_PULLUP_ENABLE,
            .scl_pullup_en = GPIO_PULLUP_ENABLE,
            .master.clk_speed = 400000,
        };
        esp_err_t ret = i2c_param_config(0, &conf);
        if (ret == ESP_OK) {
            ret = i2c_driver_install(0, I2C_MODE_MASTER, 0, 0, 0);
            /* ESP_ERR_INVALID_STATE means driver already installed (by IMU/OLED). 
             * ESP_FAIL (-1) also seen when re-installing. Assume driver OK if config succeeded. */
            if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE || ret == ESP_FAIL) {
                s_i2c_scan_initialized = true;
            }
        }
        if (!s_i2c_scan_initialized) {
            snprintf(output, out_len, "Error: I2C driver init failed");
            return ESP_OK;
        }
    }

    cJSON *root = cJSON_CreateArray();
    int count = 0;

    for (int i = 1; i < 127; i++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (i << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t ret = i2c_master_cmd_begin(0, cmd, 10 / portTICK_PERIOD_MS);
        i2c_cmd_link_delete(cmd);

        if (ret == ESP_OK) {
            char addr_str[8];
            snprintf(addr_str, sizeof(addr_str), "0x%02X", i);
            cJSON_AddItemToArray(root, cJSON_CreateString(addr_str));
            count++;
        }
    }

    if (count == 0) {
        cJSON_Delete(root);
        snprintf(output, out_len, "No I2C devices found.");
        return ESP_OK;
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    /* Prefix with message for LLM context */
    ESP_LOGI(TAG, "I2C scan found %d devices", count);
    snprintf(output, out_len, "Detected %d devices: %s", count, out);
    free(out);
    return ESP_OK;
}

/* ====================================================================
 * NEW TOOLS — Phase 1: ADC, PWM, RGB
 * ==================================================================== */

/* ADC Read — ESP-IDF 5.x oneshot API */
esp_err_t tool_adc_read(const char *input, char *output, size_t out_len) {
    cJSON *in_json = cJSON_Parse(input);
    if (!in_json) {
        snprintf(output, out_len, "Error: Invalid JSON");
        return ESP_OK;
    }

    cJSON *ch_item = cJSON_GetObjectItem(in_json, "channel");
    if (!ch_item || !cJSON_IsNumber(ch_item)) {
        cJSON_Delete(in_json);
        snprintf(output, out_len, "Error: Missing 'channel' (int 0-9)");
        return ESP_OK;
    }
    int channel = ch_item->valueint;
    cJSON_Delete(in_json);

    if (channel < 0 || channel > 9) {
        snprintf(output, out_len, "Error: ADC channel must be 0-9 (ADC1)");
        return ESP_OK;
    }

    if (!s_adc_handle) {
        snprintf(output, out_len, "Error: ADC not initialized");
        return ESP_OK;
    }

    /* Configure this channel */
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = MIMI_ADC_DEFAULT_ATTEN,
        .bitwidth = MIMI_ADC_DEFAULT_BITWIDTH,
    };
    esp_err_t ret = adc_oneshot_config_channel(s_adc_handle, channel, &chan_cfg);
    if (ret != ESP_OK) {
        snprintf(output, out_len, "Error: Failed to configure ADC channel %d: %s",
                 channel, esp_err_to_name(ret));
        return ESP_OK;
    }

    /* Read raw value */
    int raw = 0;
    ret = adc_oneshot_read(s_adc_handle, channel, &raw);
    if (ret != ESP_OK) {
        snprintf(output, out_len, "Error: ADC read failed: %s", esp_err_to_name(ret));
        return ESP_OK;
    }

    /* Try calibrated voltage */
    int voltage_mv = 0;
    if (s_adc_calibrated && s_adc_cali) {
        adc_cali_raw_to_voltage(s_adc_cali, raw, &voltage_mv);
    } else {
        /* Rough estimate: 3100mV / 4095 * raw */
        voltage_mv = (int)((float)raw / 4095.0f * 3100.0f);
    }

    ESP_LOGI(TAG, "ADC ch%d: raw=%d, voltage=%dmV", channel, raw, voltage_mv);
    snprintf(output, out_len,
             "{\"channel\":%d,\"raw\":%d,\"voltage_mv\":%d,\"calibrated\":%s}",
             channel, raw, voltage_mv, s_adc_calibrated ? "true" : "false");
    return ESP_OK;
}

/* PWM Control — LEDC driver */
esp_err_t tool_pwm_control(const char *input, char *output, size_t out_len) {
    cJSON *in_json = cJSON_Parse(input);
    if (!in_json) {
        snprintf(output, out_len, "Error: Invalid JSON");
        return ESP_OK;
    }

    cJSON *pin_item = cJSON_GetObjectItem(in_json, "pin");
    cJSON *freq_item = cJSON_GetObjectItem(in_json, "freq_hz");
    cJSON *duty_item = cJSON_GetObjectItem(in_json, "duty_percent");
    cJSON *stop_item = cJSON_GetObjectItem(in_json, "stop");

    if (!pin_item || !cJSON_IsNumber(pin_item)) {
        cJSON_Delete(in_json);
        snprintf(output, out_len, "Error: Missing 'pin' (int)");
        return ESP_OK;
    }

    int pin = pin_item->valueint;
    bool stop = (stop_item && cJSON_IsTrue(stop_item));
    int freq_hz = freq_item ? freq_item->valueint : MIMI_PWM_DEFAULT_FREQ_HZ;
    float duty_pct = duty_item ? (float)duty_item->valuedouble : 50.0f;
    cJSON_Delete(in_json);

    if (!is_safe_pin(pin)) {
        snprintf(output, out_len, "Error: Pin %d is restricted.", pin);
        return ESP_OK;
    }

    /* Find existing slot for this pin, or allocate a new one */
    int slot_idx = -1;
    int free_idx = -1;
    for (int i = 0; i < MIMI_PWM_MAX_CHANNELS; i++) {
        if (s_pwm_slots[i].in_use && s_pwm_slots[i].pin == pin) {
            slot_idx = i;
            break;
        }
        if (!s_pwm_slots[i].in_use && free_idx < 0) {
            free_idx = i;
        }
    }

    /* Stop PWM on this pin */
    if (stop) {
        if (slot_idx >= 0) {
            ledc_stop(MIMI_PWM_MODE, s_pwm_slots[slot_idx].channel, 0);
            gpio_reset_pin(pin);
            s_pwm_slots[slot_idx].in_use = false;
            snprintf(output, out_len, "OK: PWM stopped on GPIO %d", pin);
        } else {
            snprintf(output, out_len, "OK: No PWM active on GPIO %d", pin);
        }
        return ESP_OK;
    }

    /* Init timer if first use */
    if (!s_pwm_timer_init) {
        ledc_timer_config_t timer_conf = {
            .speed_mode = MIMI_PWM_MODE,
            .timer_num = MIMI_PWM_TIMER,
            .duty_resolution = MIMI_PWM_DUTY_RESOLUTION,
            .freq_hz = freq_hz,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        esp_err_t ret = ledc_timer_config(&timer_conf);
        if (ret != ESP_OK) {
            snprintf(output, out_len, "Error: LEDC timer init failed: %s",
                     esp_err_to_name(ret));
            return ESP_OK;
        }
        s_pwm_timer_init = true;
    }

    /* Allocate channel */
    if (slot_idx < 0) {
        if (free_idx < 0) {
            snprintf(output, out_len, "Error: All %d PWM channels in use. Stop one first.",
                     MIMI_PWM_MAX_CHANNELS);
            return ESP_OK;
        }
        slot_idx = free_idx;
    }

    /* Compute duty: 13-bit resolution → max 8191 */
    uint32_t max_duty = (1 << 13) - 1; /* 8191 */
    if (duty_pct < 0) duty_pct = 0;
    if (duty_pct > 100) duty_pct = 100;
    uint32_t duty_val = (uint32_t)(max_duty * duty_pct / 100.0f);

    ledc_channel_config_t ch_conf = {
        .speed_mode = MIMI_PWM_MODE,
        .channel = (ledc_channel_t)slot_idx,
        .timer_sel = MIMI_PWM_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = pin,
        .duty = duty_val,
        .hpoint = 0,
    };
    esp_err_t ret = ledc_channel_config(&ch_conf);
    if (ret != ESP_OK) {
        snprintf(output, out_len, "Error: LEDC channel config failed: %s",
                 esp_err_to_name(ret));
        return ESP_OK;
    }

    s_pwm_slots[slot_idx].pin = pin;
    s_pwm_slots[slot_idx].channel = (ledc_channel_t)slot_idx;
    s_pwm_slots[slot_idx].in_use = true;

    ESP_LOGI(TAG, "PWM GPIO %d: freq=%dHz, duty=%.1f%% (raw=%lu)",
             pin, freq_hz, duty_pct, (unsigned long)duty_val);
    snprintf(output, out_len,
             "OK: PWM on GPIO %d — freq=%dHz, duty=%.1f%%, channel=%d",
             pin, freq_hz, duty_pct, slot_idx);
    return ESP_OK;
}

/* RGB LED Control — wraps existing rgb_set() */
esp_err_t tool_rgb_control(const char *input, char *output, size_t out_len) {
    cJSON *in_json = cJSON_Parse(input);
    if (!in_json) {
        snprintf(output, out_len, "Error: Invalid JSON");
        return ESP_OK;
    }

    cJSON *r_item = cJSON_GetObjectItem(in_json, "r");
    cJSON *g_item = cJSON_GetObjectItem(in_json, "g");
    cJSON *b_item = cJSON_GetObjectItem(in_json, "b");

    int r = r_item ? r_item->valueint : 0;
    int g = g_item ? g_item->valueint : 0;
    int b = b_item ? b_item->valueint : 0;
    cJSON_Delete(in_json);

    /* Clamp to 0-255 */
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;

    rgb_set((uint8_t)r, (uint8_t)g, (uint8_t)b);

    ESP_LOGI(TAG, "RGB set to (%d, %d, %d)", r, g, b);
    snprintf(output, out_len, "OK: RGB LED set to R=%d G=%d B=%d", r, g, b);
    return ESP_OK;
}

/* UART Send — send data via UART1 */
esp_err_t tool_uart_send(const char *input, char *output, size_t out_len) {
    cJSON *in_json = cJSON_Parse(input);
    if (!in_json) {
        snprintf(output, out_len, "Error: Invalid JSON");
        return ESP_OK;
    }

    cJSON *data_item = cJSON_GetObjectItem(in_json, "data");
    cJSON *port_item = cJSON_GetObjectItem(in_json, "port");

    if (!data_item || !cJSON_IsString(data_item)) {
        cJSON_Delete(in_json);
        snprintf(output, out_len, "Error: Missing 'data' (string)");
        return ESP_OK;
    }

    const char *data = data_item->valuestring;
    int port = UART_NUM_1;
    if (port_item) {
        if (!cJSON_IsNumber(port_item)) {
            cJSON_Delete(in_json);
            snprintf(output, out_len, "Error: 'port' must be an integer (0-%d)", UART_NUM_MAX - 1);
            return ESP_OK;
        }
        port = port_item->valueint;
    }
    cJSON_Delete(in_json);

    if (port < 0 || port > UART_NUM_MAX - 1) {
        snprintf(output, out_len, "Error: Invalid UART port %d", port);
        return ESP_OK;
    }

    /* Check if UART is already installed; if not, install with defaults */
    int tx_len = uart_write_bytes(port, data, strlen(data));
    if (tx_len < 0) {
        snprintf(output, out_len, "Error: UART%d write failed. Port may not be initialized.", port);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "UART%d sent %d bytes", port, tx_len);
    snprintf(output, out_len, "OK: Sent %d bytes via UART%d", tx_len, port);
    return ESP_OK;
}

/* System Restart — controlled ESP restart */
esp_err_t tool_system_restart(const char *input, char *output, size_t out_len) {
    (void)input;
    ESP_LOGW(TAG, "System restart requested by agent");
    snprintf(output, out_len, "OK: Restarting in 500ms...");

    /* Use a timer to delay restart so the response can be sent first */
    esp_timer_handle_t restart_timer;
    const esp_timer_create_args_t args = {
        .callback = (esp_timer_cb_t)esp_restart,
        .name = "restart_timer",
    };
    esp_timer_create(&args, &restart_timer);
    esp_timer_start_once(restart_timer, 500 * 1000); /* 500ms */

    return ESP_OK;
}


/* --- Web API Handlers --- */

/* GET /api/hardware/status */
static esp_err_t hw_status_handler(httpd_req_t *req) {
    char json[1024] = {0};
    tool_system_status(NULL, json, sizeof(json));

    /* Append hardware info */
    cJSON *root = cJSON_Parse(json);
    if (root) {
        /* Add configured hardware */
        cJSON *hw = cJSON_CreateObject();
        cJSON_AddNumberToObject(hw, "rgb_pin", MIMI_PIN_RGB_LED);
        cJSON_AddNumberToObject(hw, "i2c0_sda", MIMI_PIN_I2C0_SDA);
        cJSON_AddNumberToObject(hw, "i2c0_scl", MIMI_PIN_I2C0_SCL);
        cJSON_AddNumberToObject(hw, "i2s0_ws", MIMI_PIN_I2S0_WS);
        cJSON_AddNumberToObject(hw, "i2s0_sck", MIMI_PIN_I2S0_SCK);
        cJSON_AddNumberToObject(hw, "i2s0_sd", MIMI_PIN_I2S0_SD);
        cJSON_AddNumberToObject(hw, "i2s1_din", MIMI_PIN_I2S1_DIN);
        cJSON_AddNumberToObject(hw, "i2s1_bclk", MIMI_PIN_I2S1_BCLK);
        cJSON_AddNumberToObject(hw, "i2s1_lrc", MIMI_PIN_I2S1_LRC);
        cJSON_AddNumberToObject(hw, "vol_down", MIMI_PIN_VOL_DOWN);
        cJSON_AddNumberToObject(hw, "vol_up", MIMI_PIN_VOL_UP);
        cJSON_AddItemToObject(root, "hardware_config", hw);

        char *out = cJSON_PrintUnformatted(root);
        if (out) {
            strncpy(json, out, sizeof(json) - 1);
            free(out);
        }
        cJSON_Delete(root);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    return ESP_OK;
}

/* POST /api/hardware/gpio */
static esp_err_t hw_gpio_handler(httpd_req_t *req) {
    char content[128];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) return ESP_FAIL;
    content[ret] = '\0';

    char res[256] = {0};
    tool_gpio_control(content, res, sizeof(res));
    bool is_error = (strncmp(res, "Error", 5) == 0);
    
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, res, strlen(res));
    return is_error ? ESP_FAIL : ESP_OK; /* Or strictly ESP_OK with error msg */
}

/* POST /api/hardware/scan */
static esp_err_t hw_scan_handler(httpd_req_t *req) {
    /* Initialize I2C driver if not already done */
    static bool i2c_inited = false;
    if (!i2c_inited) {
        i2c_config_t conf = {
            .mode = I2C_MODE_MASTER,
            .sda_io_num = MIMI_PIN_I2C0_SDA,
            .scl_io_num = MIMI_PIN_I2C0_SCL,
            .sda_pullup_en = GPIO_PULLUP_ENABLE,
            .scl_pullup_en = GPIO_PULLUP_ENABLE,
            .master.clk_speed = MIMI_I2C0_FREQ_HZ,
        };
        esp_err_t ret = i2c_param_config(I2C_NUM_0, &conf);
        if (ret == ESP_OK) {
            ret = i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
            /* ESP_ERR_INVALID_STATE (-1) means driver already installed */
            if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE || ret == ESP_FAIL) {
                i2c_inited = true;
            }
        }
        if (!i2c_inited) {
            ESP_LOGW(TAG, "I2C driver init failed");
        }
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *devs = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "devices", devs);

    for (int i = 1; i < 127; i++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (i << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, 10 / portTICK_PERIOD_MS);
        i2c_cmd_link_delete(cmd);

        if (ret == ESP_OK) {
            cJSON_AddItemToArray(devs, cJSON_CreateNumber(i));
        }
    }

    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, strlen(out));
    free(out);
    cJSON_Delete(root);
    return ESP_OK;
}

void tool_hardware_register_handlers(httpd_handle_t server) {
    tool_hardware_init();

    httpd_uri_t uri_status = {
        .uri = "/api/hardware/status",
        .method = HTTP_GET,
        .handler = hw_status_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_status);

    httpd_uri_t uri_gpio = {
        .uri = "/api/hardware/gpio",
        .method = HTTP_POST,
        .handler = hw_gpio_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_gpio);

    httpd_uri_t uri_scan = {
        .uri = "/api/hardware/scan",
        .method = HTTP_POST, // POST to trigger scan
        .handler = hw_scan_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_scan);

    /* Pin configuration API */
    httpd_uri_t uri_pins = {
        .uri = "/api/hardware/pins",
        .method = HTTP_GET,
        .handler = hw_pins_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_pins);

    httpd_uri_t uri_pins_post = {
        .uri = "/api/hardware/pins",
        .method = HTTP_POST,
        .handler = hw_pins_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_pins_post);
}

esp_err_t tool_hardware_init(void) {
    ESP_LOGI(TAG, "Legacy Temp Sensor init disabled");

    ESP_LOGI(TAG, "Legacy ADC init disabled");

    return ESP_OK;
}

/* --- I2S Driver Support (Phase 4) - Legacy API --- */
#if 0
static int s_tx_port = -1; /* Amp */
    if (s_tx_port != -1) return ESP_OK;

    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX,
        .sample_rate = 44100,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 6,
        .dma_buf_len = 240,
        .use_apll = false,
        .tx_desc_auto_clear = true,
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = MIMI_PIN_I2S1_BCLK,
        .ws_io_num = MIMI_PIN_I2S1_LRC,
        .data_out_num = MIMI_PIN_I2S1_DIN,
        .data_in_num = I2S_PIN_NO_CHANGE
    };

    esp_err_t ret = i2s_driver_install(I2S_NUM_1, &i2s_config, 0, NULL);
    if (ret != ESP_OK) return ret;
    
    ret = i2s_set_pin(I2S_NUM_1, &pin_config);
    if (ret != ESP_OK) return ret;
    
    s_tx_port = I2S_NUM_1;
    ESP_LOGI(TAG, "I2S TX (Amp) initialized on I2S_NUM_1");
    return ESP_OK;
}

static esp_err_t audio_ensure_rx_init(void) {
    if (s_rx_port != -1) return ESP_OK;

    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_RX,
        .sample_rate = 16000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 6,
        .dma_buf_len = 240,
        .use_apll = false,
        .tx_desc_auto_clear = false,
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = MIMI_PIN_I2S0_SCK,
        .ws_io_num = MIMI_PIN_I2S0_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = MIMI_PIN_I2S0_SD
    };

    esp_err_t ret = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    if (ret != ESP_OK) return ret;

    ret = i2s_set_pin(I2S_NUM_0, &pin_config);
    if (ret != ESP_OK) return ret;

    s_rx_port = I2S_NUM_0;
    ESP_LOGI(TAG, "I2S RX (Mic) initialized on I2S_NUM_0");
    return ESP_OK;
}

esp_err_t tool_i2s_read(const char *input, char *output, size_t out_len) {
    cJSON *in_json = cJSON_Parse(input);
    int bytes_to_read = 4096;
    if (in_json) {
        cJSON *bytes_item = cJSON_GetObjectItem(in_json, "bytes");
        if (bytes_item && cJSON_IsNumber(bytes_item)) {
            bytes_to_read = bytes_item->valueint;
        }
        cJSON_Delete(in_json);
    }

    if (bytes_to_read > 32768) bytes_to_read = 32768;
    if (bytes_to_read < 0) bytes_to_read = 4096;

    if (audio_ensure_rx_init() != ESP_OK) {
        snprintf(output, out_len, "Error: I2S RX init failed");
        return ESP_OK;
    }

    void *buf = heap_caps_malloc(bytes_to_read, MALLOC_CAP_SPIRAM);
    if (!buf) {
        snprintf(output, out_len, "Error: OOM");
        return ESP_OK;
    }

    size_t bytes_read = 0;
    esp_err_t ret = i2s_read(s_rx_port, buf, bytes_to_read, &bytes_read, 1000 / portTICK_PERIOD_MS);
    
    if (ret != ESP_OK) {
        free(buf);
        snprintf(output, out_len, "Error: Read failed %s", esp_err_to_name(ret));
        return ESP_OK;
    }

    size_t b64_len = 0;
    mbedtls_base64_encode(NULL, 0, &b64_len, buf, bytes_read);
    char *b64_buf = heap_caps_malloc(b64_len + 1, MALLOC_CAP_SPIRAM);
    if (!b64_buf) {
        free(buf);
        snprintf(output, out_len, "Error: OOM B64");
        return ESP_OK;
    }

    mbedtls_base64_encode((unsigned char *)b64_buf, b64_len, &b64_len, buf, bytes_read);
    b64_buf[b64_len] = 0;
    
    if (strlen(b64_buf) + 64 > out_len) {
         snprintf(output, out_len, "Error: Result too large for output buffer (%d bytes)", (int)b64_len);
    } else {
         snprintf(output, out_len, "{\"bytes_read\":%d,\"data_base64\":\"%s\"}", (int)bytes_read, b64_buf);
    }
    
    free(b64_buf);
    free(buf);
    return ESP_OK;
}

esp_err_t tool_i2s_write(const char *input, char *output, size_t out_len) {
    cJSON *in_json = cJSON_Parse(input);
    if (!in_json) {
        snprintf(output, out_len, "Error: Invalid JSON");
        return ESP_OK;
    }
    cJSON *data_item = cJSON_GetObjectItem(in_json, "data_base64");
    if (!data_item || !cJSON_IsString(data_item)) {
        cJSON_Delete(in_json);
        snprintf(output, out_len, "Error: Missing data_base64");
        return ESP_OK;
    }

    const char *b64_data = data_item->valuestring;
    size_t b64_len = strlen(b64_data);
    size_t out_buf_len = b64_len * 3 / 4 + 4;
    unsigned char *pcm_buf = heap_caps_malloc(out_buf_len, MALLOC_CAP_SPIRAM);
    if (!pcm_buf) {
        cJSON_Delete(in_json);
        snprintf(output, out_len, "Error: OOM");
        return ESP_OK;
    }

    size_t pcm_len = 0;
    int ret_b64 = mbedtls_base64_decode(pcm_buf, out_buf_len, &pcm_len, (const unsigned char *)b64_data, b64_len);
    cJSON_Delete(in_json);

    if (ret_b64 != 0) {
        free(pcm_buf);
        snprintf(output, out_len, "Error: Base64 decode failed");
        return ESP_OK;
    }

    if (audio_ensure_tx_init() != ESP_OK) {
        free(pcm_buf);
        snprintf(output, out_len, "Error: I2S TX init failed");
        return ESP_OK;
    }

    size_t bytes_written = 0;
    esp_err_t ret = i2s_write(s_tx_port, pcm_buf, pcm_len, &bytes_written, 1000 / portTICK_PERIOD_MS);
    free(pcm_buf);

    if (ret != ESP_OK) {
        snprintf(output, out_len, "Error: Write failed %s", esp_err_to_name(ret));
    } else {
        snprintf(output, out_len, "OK: Wrote %d bytes to I2S1", (int)bytes_written);
    }
    return ESP_OK;
}
#endif

