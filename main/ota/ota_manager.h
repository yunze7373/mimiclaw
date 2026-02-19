#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * Perform OTA firmware update from a URL.
 * Downloads the firmware binary and applies it. Reboots on success.
 *
 * @param url  HTTPS URL to the firmware .bin file
 * @return ESP_OK on success (device will reboot), error code otherwise
 */
esp_err_t ota_update_from_url(const char *url);

/**
 * Check for firmware updates by querying version endpoint.
 *
 * @param version_url  URL returning JSON: {"version":"x.y.z","url":"https://..."}
 * @return ESP_OK if update is available (call ota_get_pending_url to get URL),
 *         ESP_ERR_NOT_FOUND if already up to date.
 */
esp_err_t ota_check_for_update(const char *version_url);

/**
 * Get the pending update URL after a successful ota_check_for_update().
 * @return URL string (valid until next check) or NULL if no update pending.
 */
const char *ota_get_pending_url(void);

/**
 * Get the pending update version after a successful ota_check_for_update().
 * @return Version string or NULL.
 */
const char *ota_get_pending_version(void);

/**
 * Return current firmware version string.
 */
const char *ota_get_current_version(void);

/**
 * Validate the running firmware and mark as valid (confirms OTA succeeded).
 * Must be called after a successful OTA boot to avoid rollback.
 */
esp_err_t ota_confirm_running_firmware(void);

/**
 * Check if the current boot is from a pending OTA image not yet confirmed.
 */
bool ota_is_pending_verify(void);

/**
 * Rollback to the previous firmware partition.
 */
esp_err_t ota_rollback(void);

/**
 * Return OTA status as JSON string. Caller must free().
 * Includes: current_version, partition info, pending_verify, update status.
 */
char *ota_status_json(void);
