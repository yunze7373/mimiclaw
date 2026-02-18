#include "tool_hardware.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
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
            return true;
        default:
            return false; // All other pins are considered unsafe for general control
    }
}

/* --- Helper: Get internal temperature (if supported) --- */
static float get_cpu_temp(void) {
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

    /* Heap */
    cJSON_AddNumberToObject(root, "free_heap_internal", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "free_heap_psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(root, "min_free_heap", esp_get_minimum_free_heap_size());

    /* Temp */
    cJSON_AddNumberToObject(root, "cpu_temp_c", get_cpu_temp());

    /* Uptime */
    cJSON_AddNumberToObject(root, "uptime_s", esp_timer_get_time() / 1000000);

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
            .sda_io_num = 48,  /* Same as IMU: I2C_Touch_SDA_IO */
            .scl_io_num = 47,  /* Same as IMU: I2C_Touch_SCL_IO */
            .sda_pullup_en = GPIO_PULLUP_ENABLE,
            .scl_pullup_en = GPIO_PULLUP_ENABLE,
            .master.clk_speed = 400000,
        };
        esp_err_t ret = i2c_param_config(0, &conf);
        if (ret == ESP_OK) {
            ret = i2c_driver_install(0, I2C_MODE_MASTER, 0, 0, 0);
            /* ESP_ERR_INVALID_STATE means driver already installed (by IMU) */
            if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
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
    int port = port_item ? port_item->valueint : UART_NUM_1;
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
    char json[512] = {0};
    tool_system_status(NULL, json, sizeof(json));
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
    cJSON *root = cJSON_CreateObject();
    cJSON *devs = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "devices", devs);
    
    for (int i = 1; i < 127; i++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (i << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t ret = i2c_master_cmd_begin(0, cmd, 10 / portTICK_PERIOD_MS);
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
}

esp_err_t tool_hardware_init(void) {
    /* Init Temperature Sensor */
    /* Manual config to avoid missing TEMPERATURE_SENSOR_CLK_SRC_DEFAULT macro issue */
    temperature_sensor_config_t temp_sensor = {
        .range_min = 20,
        .range_max = 100,
        /* Leave clk_src to 0 (default) or attempt RC_FAST if needed */
        .clk_src = 0, 
    };
    if (temperature_sensor_install(&temp_sensor, &temp_handle) == ESP_OK) {
        temperature_sensor_enable(temp_handle);
        ESP_LOGI(TAG, "Temperature sensor initialized");
    } else {
        ESP_LOGW(TAG, "Temperature sensor init failed");
    }

    /* Init ADC1 oneshot */
    adc_oneshot_unit_init_cfg_t adc_init_cfg = {
        .unit_id = MIMI_ADC_UNIT,
    };
    if (adc_oneshot_new_unit(&adc_init_cfg, &s_adc_handle) == ESP_OK) {
        ESP_LOGI(TAG, "ADC1 oneshot initialized");

        /* Use Curve Fitting (standard for ESP32-S3) */
        adc_cali_curve_fitting_config_t cali_cfg = {
            .unit_id = MIMI_ADC_UNIT,
            .atten = MIMI_ADC_DEFAULT_ATTEN,
            .bitwidth = MIMI_ADC_DEFAULT_BITWIDTH,
        };
        if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali) == ESP_OK) {
            s_adc_calibrated = true;
            ESP_LOGI(TAG, "ADC calibration (curve fitting) enabled");
        } else {
            ESP_LOGW(TAG, "ADC calibration not available, using raw estimation");
        }
    } else {
        ESP_LOGW(TAG, "ADC1 init failed");
    }

    return ESP_OK;
}

