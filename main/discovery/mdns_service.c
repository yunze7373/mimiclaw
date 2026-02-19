#include "discovery/mdns_service.h"
#include "federation/peer_manager.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "mdns.h"
#include "esp_app_desc.h"
#include "mimi_config.h"

static const char *TAG = "mdns_svc";

#define MDNS_HOSTNAME    "mimiclaw"
#define MDNS_INSTANCE    "MimiClaw AI Agent"
#define MDNS_SERVICE     "_mimiclaw"
#define MDNS_PROTO       "_tcp"

esp_err_t mdns_service_init(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Set hostname — accessible at mimiclaw.local */
    err = mdns_hostname_set(MDNS_HOSTNAME);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set hostname: %s", esp_err_to_name(err));
        return err;
    }

    mdns_instance_name_set(MDNS_INSTANCE);

    ESP_LOGI(TAG, "mDNS initialized: %s.local", MDNS_HOSTNAME);
    
    /* Init peer manager */
    peer_manager_init();
    
    return ESP_OK;
}

esp_err_t mdns_service_start(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();

    /* Register _mimiclaw._tcp service on WS port */
    esp_err_t err = mdns_service_add(
        MDNS_INSTANCE,
        MDNS_SERVICE,
        MDNS_PROTO,
        MIMI_WS_PORT,
        NULL, 0
    );
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add mDNS service: %s", esp_err_to_name(err));
        return err;
    }

    /* Add TXT records with device info */
    mdns_service_txt_item_set(MDNS_SERVICE, MDNS_PROTO, "version", desc->version);
    mdns_service_txt_item_set(MDNS_SERVICE, MDNS_PROTO, "project", desc->project_name);
    mdns_service_txt_item_set(MDNS_SERVICE, MDNS_PROTO, "idf", desc->idf_ver);

    char ws_port[8];
    snprintf(ws_port, sizeof(ws_port), "%d", MIMI_WS_PORT);
    mdns_service_txt_item_set(MDNS_SERVICE, MDNS_PROTO, "ws_port", ws_port);

    /* Also register as HTTP service for browser discovery */
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

    ESP_LOGI(TAG, "mDNS service started: %s._mimiclaw._tcp port %d", MDNS_HOSTNAME, MIMI_WS_PORT);
    return ESP_OK;
}

void mdns_service_update_skill_count(int count)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", count);
    mdns_service_txt_item_set(MDNS_SERVICE, MDNS_PROTO, "skills", buf);
}

void mdns_service_query_peers(void)
{
    ESP_LOGI(TAG, "Querying for peers (_mimiclaw._tcp)...");
    
    peer_manager_prune();

    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr(MDNS_SERVICE, MDNS_PROTO, 3000, 16, &results);
    if (err) {
        ESP_LOGW(TAG, "mDNS query failed");
        return;
    }

    if (!results) {
        ESP_LOGW(TAG, "No peers found");
        return;
    }

    mdns_result_t *r = results;
    while (r) {
        if (r->addr) {
            char ip_str[16];
            if (r->addr->addr.type == ESP_IPADDR_TYPE_V4) {
                snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&r->addr->addr.u_addr.ip4));
                
                /* Filter out ourselves if possible, or handle loopback logic elsewhere. 
                   For now, just add everyone found. */
                const char *hostname = r->hostname ? r->hostname : (r->instance_name ? r->instance_name : "unknown");
                
                ESP_LOGI(TAG, "Found peer: %s @ %s:%d", hostname, ip_str, r->port);
                peer_manager_add_or_update(hostname, ip_str, r->port);
            }
        }
        r = r->next;
    }

    mdns_query_results_free(results);
}
