#include "ota/ota_manager.h"

#include <string.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

static const char *TAG = "ota";

/* ── State ────────────────────────────────────────────────────── */

static char s_pending_url[256];
static char s_pending_version[32];
static bool s_update_available = false;

/* Current firmware version (populated once on first call) */
static const char *s_current_version = NULL;

/* ── Helpers ──────────────────────────────────────────────────── */

/**
 * Compare semver strings "major.minor.patch".
 * Returns >0 if a > b, <0 if a < b, 0 if equal.
 */
static int semver_compare(const char *a, const char *b)
{
    int a_major = 0, a_minor = 0, a_patch = 0;
    int b_major = 0, b_minor = 0, b_patch = 0;
    sscanf(a, "%d.%d.%d", &a_major, &a_minor, &a_patch);
    sscanf(b, "%d.%d.%d", &b_major, &b_minor, &b_patch);

    if (a_major != b_major) return a_major - b_major;
    if (a_minor != b_minor) return a_minor - b_minor;
    return a_patch - b_patch;
}

/* ── Public API ───────────────────────────────────────────────── */

const char *ota_get_current_version(void)
{
    if (!s_current_version) {
        const esp_app_desc_t *desc = esp_app_get_description();
        s_current_version = desc->version;
    }
    return s_current_version;
}

esp_err_t ota_check_for_update(const char *version_url)
{
    if (!version_url || !version_url[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    s_update_available = false;
    s_pending_url[0] = '\0';
    s_pending_version[0] = '\0';

    ESP_LOGI(TAG, "Checking for updates at: %s", version_url);

    /* Fetch version JSON */
    esp_http_client_config_t config = {
        .url = version_url,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_ERR_NO_MEM;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to connect: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int content_len = esp_http_client_fetch_headers(client);
    if (content_len <= 0 || content_len > 2048) {
        ESP_LOGE(TAG, "Invalid response length: %d", content_len);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }

    char *body = heap_caps_malloc(content_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        body = malloc(content_len + 1);
    }
    if (!body) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int read_len = esp_http_client_read(client, body, content_len);
    body[read_len > 0 ? read_len : 0] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    /* Parse JSON: {"version":"x.y.z", "url":"https://...", "notes":"..."} */
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse version JSON");
        return ESP_FAIL;
    }

    cJSON *ver = cJSON_GetObjectItem(root, "version");
    cJSON *url = cJSON_GetObjectItem(root, "url");

    if (!cJSON_IsString(ver) || !cJSON_IsString(url)) {
        ESP_LOGE(TAG, "Version JSON missing 'version' or 'url' fields");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const char *current = ota_get_current_version();
    const char *remote = ver->valuestring;

    ESP_LOGI(TAG, "Current: %s  Remote: %s", current, remote);

    if (semver_compare(remote, current) > 0) {
        /* Newer version available */
        snprintf(s_pending_version, sizeof(s_pending_version), "%s", remote);
        snprintf(s_pending_url, sizeof(s_pending_url), "%s", url->valuestring);
        s_update_available = true;
        ESP_LOGI(TAG, "Update available: %s → %s", current, remote);
        cJSON_Delete(root);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Already up to date (%s)", current);
    cJSON_Delete(root);
    return ESP_ERR_NOT_FOUND;
}

const char *ota_get_pending_url(void)
{
    return s_update_available ? s_pending_url : NULL;
}

const char *ota_get_pending_version(void)
{
    return s_update_available ? s_pending_version : NULL;
}

esp_err_t ota_update_from_url(const char *url)
{
    if (!url || !url[0]) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "Starting OTA from: %s", url);

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 120000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    esp_err_t ret = esp_https_ota(&ota_config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA successful, restarting...");
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(ret));
    }

    return ret;
}

/* ── Rollback & Verification ──────────────────────────────────── */

bool ota_is_pending_verify(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
        return (state == ESP_OTA_IMG_PENDING_VERIFY);
    }
    return false;
}

esp_err_t ota_confirm_running_firmware(void)
{
    if (!ota_is_pending_verify()) {
        ESP_LOGI(TAG, "Firmware already confirmed or factory image");
        return ESP_OK;
    }

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Running firmware confirmed as valid");
    } else {
        ESP_LOGE(TAG, "Failed to confirm firmware: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t ota_rollback(void)
{
    ESP_LOGW(TAG, "Rolling back to previous firmware...");
    esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    /* If we get here, rollback failed (device should have rebooted) */
    ESP_LOGE(TAG, "Rollback failed: %s", esp_err_to_name(err));
    return err;
}

/* ── JSON Status ──────────────────────────────────────────────── */

char *ota_status_json(void)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;

    /* Current firmware info */
    const esp_app_desc_t *desc = esp_app_get_description();
    cJSON_AddStringToObject(obj, "version", desc->version);
    cJSON_AddStringToObject(obj, "project", desc->project_name);
    cJSON_AddStringToObject(obj, "date", desc->date);
    cJSON_AddStringToObject(obj, "time", desc->time);
    cJSON_AddStringToObject(obj, "idf_ver", desc->idf_ver);

    /* Partition info */
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running) {
        cJSON_AddStringToObject(obj, "partition", running->label);
        cJSON_AddNumberToObject(obj, "partition_addr", running->address);
        cJSON_AddNumberToObject(obj, "partition_size", running->size);
    }

    /* Rollback state */
    cJSON_AddBoolToObject(obj, "pending_verify", ota_is_pending_verify());

    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (next) {
        cJSON_AddStringToObject(obj, "next_partition", next->label);
    }

    /* Pending update */
    cJSON_AddBoolToObject(obj, "update_available", s_update_available);
    if (s_update_available) {
        cJSON_AddStringToObject(obj, "update_version", s_pending_version);
        cJSON_AddStringToObject(obj, "update_url", s_pending_url);
    }

    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return json;
}
