#pragma once

#include "esp_err.h"

/**
 * Initialize mDNS service and advertise Esp32Claw on the local network.
 *
 * Registers hostname "esp32claw" and a _esp32claw._tcp service with
 * TXT records for version, skill count, and WebSocket port.
 *
 * Accessible at: http://esp32claw.local
 */
esp_err_t mdns_service_init(void);

/**
 * Start mDNS service (call after WiFi is connected).
 */
esp_err_t mdns_service_start(void);

/**
 * Update the advertised skill count TXT record.
 * Call after skills are loaded/unloaded.
 */
void mdns_service_update_skill_count(int count);

/**
 * Query peers advertising _mimiclaw._tcp and refresh peer manager entries.
 */
void mdns_service_query_peers(void);
