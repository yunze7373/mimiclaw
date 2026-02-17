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
#include "driver/temperature_sensor.h"
#include "soc/rtc.h"

static const char *TAG = "tool_hw";

/* --- Helper: Check Safe Pin --- */
static bool is_safe_pin(int pin) {
    if (pin < 0 || pin > 48) return false;
    /* System pins */
    if (pin == 0) return false; // Boot
    if (pin == 1 || pin == 3) return false; // UART0
    if (pin >= 6 && pin <= 11) return false; // Flash/PSRAM
    if (pin >= 19 && pin <= 20) return false; // USB JTAG
    /* Display / Touch / I2C (based on current board config) */
    /* LCD: 39, 40, 41, 42, 45, 46 */
    if (pin == 39 || pin == 40 || pin == 41 || pin == 42 || pin == 45 || pin == 46) return false;
    /* I2C/Touch: 47, 48 */
    if (pin == 47 || pin == 48) return false;
    
    return true;
}

/* --- Helper: Get internal temperature (if supported) --- */
static float get_cpu_temp(void) {
    float tsens_out;
    temperature_sensor_handle_t temp_handle = NULL;
    temperature_sensor_config_t temp_sensor = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);
    if (temperature_sensor_install(&temp_sensor, &temp_handle) == ESP_OK) {
        temperature_sensor_enable(temp_handle);
        temperature_sensor_get_celsius(temp_handle, &tsens_out);
        temperature_sensor_disable(temp_handle);
        temperature_sensor_uninstall(temp_handle);
        return tsens_out;
    }
    return 0.0f;
}

/* --- Tool Implementation --- */

/* Get System Status */
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
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, state);

    snprintf(output, out_len, "OK: GPIO %d set to %s", pin, state ? "HIGH (1)" : "LOW (0)");
    return ESP_OK;
}

/* I2C Scan */
/* I2C Scan */
esp_err_t tool_i2c_scan(const char *input, char *output, size_t out_len) {
    (void)input; /* Assuming bus 0 for now */
    
    /* Port 0 should be initialized by IMU manager. We just scan. */
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
    snprintf(output, out_len, "Detected %d devices: %s", count, out);
    free(out);
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
    // char *res = tool_i2c_scan(NULL);
    httpd_resp_set_type(req, "text/plain"); /* Or JSON? scan returns text description for LLM */
    /* For Web UI, we might prefer raw JSON array. But let's stick to tool output for consistency */
    /* Wait, for Web UI list, JSON array is better. 
       Let's create a separate helper or parse the string. 
       Actually, `tool_i2c_scan` returns "Detected X devices: [...]".
       I'll modify `tool_i2c_scan` to return raw JSON if input is "json"? 
       Or just parse it in JS. 
       Let's keep it simple: I will make `tool_i2c_scan` return JSON array directly if input is "raw".
     */
     /* Re-implementing scan logic for handler to be clean */
    
    /* ... actually, let's just run scan again and return JSON array */
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
    /* Nothing special to init, I2C is init by IMU manager */
    return ESP_OK;
}
