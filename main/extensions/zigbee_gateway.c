#include <stdio.h>
#include "esp_log.h"
#include "zigbee_gateway.h"

static const char *TAG = "zigbee_gateway";

esp_err_t zigbee_gateway_init(void)
{
    ESP_LOGI(TAG, "Initializing Virtual Zigbee Gateway (Stub)");
    return ESP_OK;
}

esp_err_t zigbee_gateway_start(void)
{
    ESP_LOGI(TAG, "Zigbee Gateway started (No hardware attached)");
    return ESP_OK;
}
