#ifndef MQTT_MANAGER_H
#define MQTT_MANAGER_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the MQTT manager component
esp_err_t mqtt_manager_init(void);

// Start the MQTT client (called by component manager or manually)
esp_err_t mqtt_manager_start(void);

// Validate MQTT configuration
bool mqtt_manager_is_configured(void);

// Publish a message manually (for other components)
esp_err_t mqtt_manager_publish(const char *topic, const char *payload, int qos, int retain);

#ifdef __cplusplus
}
#endif

#endif // MQTT_MANAGER_H
