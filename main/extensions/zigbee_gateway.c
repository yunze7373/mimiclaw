#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "zigbee_gateway.h"
#include "cJSON.h"

static const char *TAG = "zigbee_gateway";

static zigbee_device_t s_devices[ZIGBEE_MAX_DEVICES];
static int s_device_count = 0;
static bool s_permit_join = false;

static void add_mock_device(uint16_t addr, const char *ieee, const char *name, zigbee_device_type_t type)
{
    if (s_device_count >= ZIGBEE_MAX_DEVICES) return;
    zigbee_device_t *d = &s_devices[s_device_count++];
    d->short_addr = addr;
    snprintf(d->ieee_addr, sizeof(d->ieee_addr), "%s", ieee);
    snprintf(d->name, sizeof(d->name), "%s", name);
    d->type = type;
    d->online = true;
    d->on_off = false;
    d->level = 0;
    d->temperature = 22.5f;
    d->humidity = 45.0f;
    d->occupancy = false;
}

esp_err_t zigbee_gateway_init(void)
{
    ESP_LOGI(TAG, "Initializing Virtual Zigbee Gateway");
    s_device_count = 0;
    
    /* Populate with mock devices */
    add_mock_device(0x1A2B, "00124b001ca6fc9a", "Living Room Light", ZIGBEE_DEV_LIGHT);
    add_mock_device(0x3C4D, "00124b001ca6fc9b", "Kitchen Switch", ZIGBEE_DEV_SWITCH);
    add_mock_device(0x5E6F, "00124b001ca6fc9c", "Bedroom Sensor", ZIGBEE_DEV_SENSOR_TEMP);
    
    return ESP_OK;
}

esp_err_t zigbee_gateway_start(void)
{
    ESP_LOGI(TAG, "Zigbee Gateway started");
    return ESP_OK;
}

const zigbee_device_t *zigbee_gateway_get_devices(int *count)
{
    if (count) *count = s_device_count;
    return s_devices;
}

esp_err_t zigbee_gateway_control_onoff(uint16_t short_addr, bool on_off)
{
    for (int i = 0; i < s_device_count; i++) {
        if (s_devices[i].short_addr == short_addr) {
            s_devices[i].on_off = on_off;
            ESP_LOGI(TAG, "Control Device 0x%04X -> %s", short_addr, on_off ? "ON" : "OFF");
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t zigbee_gateway_permit_join(bool enable)
{
    s_permit_join = enable;
    ESP_LOGI(TAG, "Permit Join: %s", enable ? "ENABLED" : "DISABLED");
    return ESP_OK;
}

char *zigbee_gateway_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *devs = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "devices", devs);
    cJSON_AddBoolToObject(root, "permit_join", s_permit_join);

    for (int i = 0; i < s_device_count; i++) {
        zigbee_device_t *d = &s_devices[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ieee", d->ieee_addr);
        cJSON_AddNumberToObject(item, "nwk", d->short_addr); // Hex displayed as decimal in JSON usually fine
        cJSON_AddStringToObject(item, "name", d->name);
        cJSON_AddNumberToObject(item, "type", d->type);
        cJSON_AddBoolToObject(item, "online", d->online);
        
        switch (d->type) {
            case ZIGBEE_DEV_LIGHT:
            case ZIGBEE_DEV_SWITCH:
                cJSON_AddBoolToObject(item, "on_off", d->on_off);
                break;
            case ZIGBEE_DEV_SENSOR_TEMP:
                cJSON_AddNumberToObject(item, "temperature", d->temperature);
                cJSON_AddNumberToObject(item, "humidity", d->humidity);
                break;
            case ZIGBEE_DEV_SENSOR_MOTION:
                cJSON_AddBoolToObject(item, "occupancy", d->occupancy);
                break;
            default: break;
        }
        cJSON_AddItemToArray(devs, item);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}
