#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/i2c.h"
#include "component/component_mgr.h"
#include "component/component_auto_detect.h"

static const char *TAG = "comp_auto";

/* Hardware Constants */
#define MIN_PSRAM_FOR_AGENT   (2 * 1024 * 1024)  /* 2MB PSRAM required for Agent */
#define I2C_MASTER_NUM        I2C_NUM_0
#define I2C_MASTER_SDA_IO     41
#define I2C_MASTER_SCL_IO     42
#define I2C_MASTER_FREQ_HZ    400000

/* Probe for I2C device presence */
static bool i2c_probe(uint8_t addr)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, 10 / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);
    return ret == ESP_OK;
}

void comp_auto_detect_apply(void)
{
    ESP_LOGI(TAG, "Running hardware auto-detection...");

    /* 1. Check PSRAM */
    size_t psram_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "Detected PSRAM: %d MB", psram_size / (1024 * 1024));

    if (psram_size < MIN_PSRAM_FOR_AGENT) {
        ESP_LOGW(TAG, "Insufficient PSRAM for Agent (<2MB). Disabling Agent and dependent components.");
        comp_set_enabled("agent", false);
        comp_set_enabled("llm", false);
        comp_set_enabled("tool_reg", false);
        comp_set_enabled("web_ui", false); // heavy web UI might need PSRAM
    }

    /* 2. Check Display (I2C Probe) */
    /* Note: This assumes I2C is already initialized or we init it briefly here. 
       For safety, we'll skip actual I2C init here to avoid conflicts if main Init does it differently. 
       This is a placeholder for logic that would verify display presence. */
    // if (!i2c_probe(0x3C)) { // SSD1306 default addr
    //     ESP_LOGW(TAG, "OLED display not found. Disabling UI.");
    //     // comp_set_enabled("web_ui", false); // WebUI is WiFi based, not display based
    // }

    /* 3. Check WiFi Credentials (If NVS empty, disable WiFi components) */
    // This logic is better handled by wifi_manager itself, but we could disable modules here if we knew.

    /* 4. Zigbee Hardware Check (e.g. check UART response) */
    /* Placeholder */

    ESP_LOGI(TAG, "Auto-detection complete.");
}

bool comp_auto_detect_is_enabled(void)
{
    // state is stored in component manager's config parsing result
    // For now, we assume it's passed or stored globally. 
    // We will update component_mgr.c to expose this or handle it.
    return true; 
}
