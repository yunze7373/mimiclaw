#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * Peer Manager: Tracks discoverable Esp32Claw devices on the local network.
 * Stores IP, hostname, and last seen timestamp.
 * Used for multi-device federation and swarm capabilities.
 */

#define PEER_MAX_COUNT 16
#define PEER_TIMEOUT_SEC 300  /* Remove peer if not seen for 5 mins */

typedef struct {
    char hostname[64];
    char ip_addr[16];     /* IPv4 string */
    int64_t last_seen;    /* Timestamp (seconds) */
    bool active;
    int port;             /* Service port (default 80) */
    char group_id[32];    /* Device group ID */
    /* Potential future fields: role, capabilities, version */
} peer_t;

/**
 * Initialize the peer manager.
 */
void peer_manager_init(void);

/**
 * Add or update a peer in the list.
 * @param hostname  Device hostname (e.g., mimiclaw-1234)
 * @param ip        IP address string
 * @param port      Service port
 * @param group_id  Device group ID (optional, default "default")
 * @return ESP_OK on success, ESP_ERR_NO_MEM if list full
 */
esp_err_t peer_manager_add_or_update(const char *hostname, const char *ip, int port, const char *group_id);

/**
 * Remove stale peers that haven't been seen for PEER_TIMEOUT_SEC.
 */
void peer_manager_prune(void);

/**
 * Get the full list of peers as a JSON string.
 * Caller must free the returned string.
 */
char *peer_manager_get_json(void);

/**
 * Get internal peer list (read-only).
 * @param count Output pointer for number of peers
 * @return Pointer to peer array
 */
const peer_t *peer_manager_get_list(int *count);
