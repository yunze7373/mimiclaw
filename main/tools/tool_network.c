#include "tool_network.h"
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "mimi_config.h"

/* NimBLE includes */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"
#include "freertos/semphr.h"

static const char *TAG = "tool_net";

/* ====================================================================
 * WiFi Tools
 * ==================================================================== */

/* WiFi Scan — scan for nearby APs */
esp_err_t tool_wifi_scan(const char *input, char *output, size_t out_len) {
    (void)input;

    /* Start a blocking scan */
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };

    esp_err_t ret = esp_wifi_scan_start(&scan_cfg, true);
    if (ret != ESP_OK) {
        snprintf(output, out_len, "Error: WiFi scan failed: %s", esp_err_to_name(ret));
        return ESP_OK;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        snprintf(output, out_len, "{\"count\":0,\"aps\":[]}");
        return ESP_OK;
    }

    /* Limit to avoid excessive memory usage */
    if (ap_count > 20) ap_count = 20;

    wifi_ap_record_t *ap_list = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!ap_list) {
        snprintf(output, out_len, "Error: Out of memory for scan results");
        return ESP_OK;
    }

    esp_wifi_scan_get_ap_records(&ap_count, ap_list);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "count", ap_count);
    cJSON *aps = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "aps", aps);

    for (int i = 0; i < ap_count; i++) {
        cJSON *ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "ssid", (char *)ap_list[i].ssid);
        cJSON_AddNumberToObject(ap, "rssi", ap_list[i].rssi);
        cJSON_AddNumberToObject(ap, "channel", ap_list[i].primary);

        const char *auth_str = "unknown";
        switch (ap_list[i].authmode) {
            case WIFI_AUTH_OPEN:          auth_str = "open"; break;
            case WIFI_AUTH_WEP:           auth_str = "WEP"; break;
            case WIFI_AUTH_WPA_PSK:       auth_str = "WPA-PSK"; break;
            case WIFI_AUTH_WPA2_PSK:      auth_str = "WPA2-PSK"; break;
            case WIFI_AUTH_WPA_WPA2_PSK:  auth_str = "WPA/WPA2-PSK"; break;
            case WIFI_AUTH_WPA3_PSK:      auth_str = "WPA3-PSK"; break;
            case WIFI_AUTH_WPA2_WPA3_PSK: auth_str = "WPA2/WPA3-PSK"; break;
            default:                      auth_str = "other"; break;
        }
        cJSON_AddStringToObject(ap, "auth", auth_str);

        cJSON_AddItemToArray(aps, ap);
    }

    free(ap_list);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str) {
        snprintf(output, out_len, "%s", json_str);
        free(json_str);
    } else {
        snprintf(output, out_len, "Error: JSON serialization failed");
    }

    ESP_LOGI(TAG, "WiFi scan found %d APs", ap_count);
    return ESP_OK;
}

/* WiFi Status — get current connection info */
esp_err_t tool_wifi_status(const char *input, char *output, size_t out_len) {
    (void)input;

    cJSON *root = cJSON_CreateObject();

    /* Connection state */
    wifi_ap_record_t ap_info;
    bool connected = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);
    cJSON_AddBoolToObject(root, "connected", connected);

    if (connected) {
        cJSON_AddStringToObject(root, "ssid", (char *)ap_info.ssid);
        cJSON_AddNumberToObject(root, "rssi", ap_info.rssi);
        cJSON_AddNumberToObject(root, "channel", ap_info.primary);

        char bssid_str[18];
        snprintf(bssid_str, sizeof(bssid_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 ap_info.bssid[0], ap_info.bssid[1], ap_info.bssid[2],
                 ap_info.bssid[3], ap_info.bssid[4], ap_info.bssid[5]);
        cJSON_AddStringToObject(root, "bssid", bssid_str);
    }

    /* IP address */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            char ip_str[16];
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
            cJSON_AddStringToObject(root, "ip", ip_str);

            char gw_str[16];
            snprintf(gw_str, sizeof(gw_str), IPSTR, IP2STR(&ip_info.gw));
            cJSON_AddStringToObject(root, "gateway", gw_str);
        }
    }

    /* MAC address */
    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        cJSON_AddStringToObject(root, "mac", mac_str);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str) {
        snprintf(output, out_len, "%s", json_str);
        free(json_str);
    } else {
        snprintf(output, out_len, "Error: JSON serialization failed");
    }
    return ESP_OK;
}

/* ====================================================================
 * BLE Tools — NimBLE
 * ==================================================================== */

#define BLE_SCAN_MAX_RESULTS  20

typedef struct {
    uint8_t addr[6];
    int8_t  rssi;
    char    name[32];
} ble_scan_result_t;

static ble_scan_result_t s_ble_results[BLE_SCAN_MAX_RESULTS];
static int s_ble_result_count = 0;
static SemaphoreHandle_t s_ble_scan_done = NULL;
static bool s_ble_initialized = false;

/* BLE GAP event callback */
static int ble_gap_event_cb(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
        case BLE_GAP_EVENT_DISC: {
            if (s_ble_result_count >= BLE_SCAN_MAX_RESULTS) break;

            /* Check for duplicates by address */
            for (int i = 0; i < s_ble_result_count; i++) {
                if (memcmp(s_ble_results[i].addr, event->disc.addr.val, 6) == 0) {
                    return 0; /* Skip duplicate */
                }
            }

            ble_scan_result_t *r = &s_ble_results[s_ble_result_count];
            memcpy(r->addr, event->disc.addr.val, 6);
            r->rssi = event->disc.rssi;
            r->name[0] = '\0';

            /* Try to extract name from advertisement data */
            struct ble_hs_adv_fields fields;
            if (ble_hs_adv_parse_fields(&fields, event->disc.data,
                                         event->disc.length_data) == 0) {
                if (fields.name != NULL && fields.name_len > 0) {
                    int len = fields.name_len;
                    if (len > 31) len = 31;
                    memcpy(r->name, fields.name, len);
                    r->name[len] = '\0';
                }
            }

            s_ble_result_count++;
            break;
        }
        case BLE_GAP_EVENT_DISC_COMPLETE:
            ESP_LOGI(TAG, "BLE scan complete, found %d devices", s_ble_result_count);
            if (s_ble_scan_done) {
                xSemaphoreGive(s_ble_scan_done);
            }
            break;
        default:
            break;
    }
    return 0;
}

/* NimBLE host task */
static void ble_host_task(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* BLE on_sync callback */
static void ble_on_sync(void) {
    /* Use best available address */
    ble_hs_util_ensure_addr(0);
    ESP_LOGI(TAG, "NimBLE host synced");
}

/* BLE Scan tool — lazy-initializes NimBLE on first call */
esp_err_t tool_ble_scan(const char *input, char *output, size_t out_len) {
    (void)input;

    /* Lazy init: start NimBLE only when BLE scan is first requested.
     * This avoids consuming ~80KB internal RAM at boot. */
    if (!s_ble_initialized) {
        ESP_LOGI(TAG, "Initializing NimBLE (lazy, first ble_scan call)...");
        esp_err_t ret = nimble_port_init();
        if (ret != ESP_OK) {
            snprintf(output, out_len,
                     "Error: NimBLE init failed: %s", esp_err_to_name(ret));
            return ESP_OK;
        }

        ble_hs_cfg.sync_cb = ble_on_sync;
        nimble_port_freertos_init(ble_host_task);
        s_ble_initialized = true;

        /* Give NimBLE host a moment to sync */
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP_LOGI(TAG, "NimBLE initialized for BLE scanning");
    }

    /* Reset results */
    s_ble_result_count = 0;
    memset(s_ble_results, 0, sizeof(s_ble_results));

    if (!s_ble_scan_done) {
        s_ble_scan_done = xSemaphoreCreateBinary();
    }

    /* Start BLE scan */
    struct ble_gap_disc_params scan_params = {
        .itvl = BLE_GAP_SCAN_ITVL_MS(100),
        .window = BLE_GAP_SCAN_WIN_MS(100),
        .filter_policy = BLE_HCI_SCAN_FILT_NO_WL,
        .limited = 0,
        .passive = 0,    /* Active scan to get names */
        .filter_duplicates = 1,
    };

    esp_err_t ret = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, MIMI_BLE_SCAN_DURATION_S * 1000,
                                  &scan_params, ble_gap_event_cb, NULL);
    if (ret != 0) {
        snprintf(output, out_len, "Error: BLE scan start failed (rc=%d)", ret);
        return ESP_OK;
    }

    /* Wait for scan to complete (with timeout) */
    if (xSemaphoreTake(s_ble_scan_done, pdMS_TO_TICKS((MIMI_BLE_SCAN_DURATION_S + 2) * 1000)) != pdTRUE) {
        ESP_LOGW(TAG, "BLE scan timeout");
    }

    /* Build JSON response */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "count", s_ble_result_count);
    cJSON *devices = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "devices", devices);

    for (int i = 0; i < s_ble_result_count; i++) {
        cJSON *dev = cJSON_CreateObject();

        char addr_str[18];
        snprintf(addr_str, sizeof(addr_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 s_ble_results[i].addr[5], s_ble_results[i].addr[4],
                 s_ble_results[i].addr[3], s_ble_results[i].addr[2],
                 s_ble_results[i].addr[1], s_ble_results[i].addr[0]);
        cJSON_AddStringToObject(dev, "addr", addr_str);
        cJSON_AddNumberToObject(dev, "rssi", s_ble_results[i].rssi);

        if (s_ble_results[i].name[0]) {
            cJSON_AddStringToObject(dev, "name", s_ble_results[i].name);
        } else {
            cJSON_AddStringToObject(dev, "name", "(unknown)");
        }

        cJSON_AddItemToArray(devices, dev);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str) {
        snprintf(output, out_len, "%s", json_str);
        free(json_str);
    } else {
        snprintf(output, out_len, "Error: JSON serialization failed");
    }

    ESP_LOGI(TAG, "BLE scan found %d devices", s_ble_result_count);
    return ESP_OK;
}

/* Network init — NimBLE is lazy-initialized on first ble_scan call */
esp_err_t tool_network_init(void) {
    ESP_LOGI(TAG, "Network tools ready (BLE will init on first scan)");
    return ESP_OK;
}

