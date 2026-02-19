#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include "esp_http_server.h"

#define ZIGBEE_MAX_DEVICES 16

typedef enum {
    ZIGBEE_DEV_UNKNOWN = 0,
    ZIGBEE_DEV_LIGHT,
    ZIGBEE_DEV_SWITCH,
    ZIGBEE_DEV_SENSOR_TEMP,
    ZIGBEE_DEV_SENSOR_MOTION,
} zigbee_device_type_t;

typedef struct {
    uint16_t short_addr;       /* 0x1234 */
    char ieee_addr[17];        /* "00124b001ca6fc9a" */
    char name[32];             /* "Living Room Light" */
    zigbee_device_type_t type;
    bool online;
    
    /* State */
    bool on_off;
    uint8_t level;             /* 0-255 */
    float temperature;
    float humidity;
    bool occupancy;
} zigbee_device_t;

/**
 * Initialize Zigbee Gateway (Mock/Stub or Hardware)
 */
esp_err_t zigbee_gateway_init(void);

/**
 * Start the Gateway
 */
esp_err_t zigbee_gateway_start(void);

/**
 * Get list of known devices.
 * @param count Output pointer for number of devices
 * @return Pointer to internal array (do not free)
 */
const zigbee_device_t *zigbee_gateway_get_devices(int *count);

/**
 * Control a device.
 * @param short_addr Device Short Address
 * @param on_off Desired state
 * @return ESP_OK or ESP_ERR_NOT_FOUND
 */
esp_err_t zigbee_gateway_control_onoff(uint16_t short_addr, bool on_off);

/**
 * Enable Permit Join (Pairing Mode)
 */
esp_err_t zigbee_gateway_permit_join(bool enable);

/**
 * Create a JSON string of all devices. Caller must free.
 */
char *zigbee_gateway_json(void);

/**
 * HTTP handlers for Web UI API
 */
esp_err_t zigbee_devices_handler(httpd_req_t *req);
esp_err_t zigbee_control_handler(httpd_req_t *req);
