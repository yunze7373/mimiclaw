#pragma once

#include "esp_err.h"

/**
 * Initialize mDNS service and advertise MimiClaw on the local network.
 *
 * Registers hostname "mimiclaw" and a _mimiclaw._tcp service with
 * TXT records for version, skill count, and WebSocket port.
 *
 * Accessible at: http://mimiclaw.local
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
