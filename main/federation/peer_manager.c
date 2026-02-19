#include "federation/peer_manager.h"
#include "esp_log.h"
#include "cJSON.h"
#include "esp_timer.h"
#include <string.h>
#include <sys/time.h>
#include <time.h>

static const char *TAG = "peer_mgr";
static peer_t s_peers[PEER_MAX_COUNT];
static int s_peer_count = 0;

/* Helper: Get current timestamp in seconds */
static int64_t get_time_sec(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec;
}

void peer_manager_init(void)
{
    memset(s_peers, 0, sizeof(s_peers));
    s_peer_count = 0;
    ESP_LOGI(TAG, "Peer Manager initialized (max %d peers)", PEER_MAX_COUNT);
}

esp_err_t peer_manager_add_or_update(const char *hostname, const char *ip, int port, const char *group_id)
{
    if (!hostname || !ip) return ESP_ERR_INVALID_ARG;
    if (!group_id) group_id = "default";

    int64_t now = get_time_sec();

    /* Check if peer exists */
    for (int i = 0; i < PEER_MAX_COUNT; i++) {
        if (s_peers[i].active && strcmp(s_peers[i].hostname, hostname) == 0) {
            /* Update existing */
            strncpy(s_peers[i].ip_addr, ip, sizeof(s_peers[i].ip_addr) - 1);
            strncpy(s_peers[i].group_id, group_id, sizeof(s_peers[i].group_id) - 1);
            s_peers[i].port = port;
            s_peers[i].last_seen = now;
            ESP_LOGD(TAG, "Updated peer: %s (%s) group=%s", hostname, ip, group_id);
            return ESP_OK;
        }
    }

    /* Add new peer */
    for (int i = 0; i < PEER_MAX_COUNT; i++) {
        if (!s_peers[i].active) {
            strncpy(s_peers[i].hostname, hostname, sizeof(s_peers[i].hostname) - 1);
            strncpy(s_peers[i].ip_addr, ip, sizeof(s_peers[i].ip_addr) - 1);
            strncpy(s_peers[i].group_id, group_id, sizeof(s_peers[i].group_id) - 1);
            s_peers[i].port = port;
            s_peers[i].last_seen = now;
            s_peers[i].active = true;
            if (i >= s_peer_count) s_peer_count = i + 1;
            ESP_LOGI(TAG, "New peer discovered: %s (%s) group=%s", hostname, ip, group_id);
            return ESP_OK;
        }
    }

    ESP_LOGW(TAG, "Peer list full, cannot add: %s", hostname);
    return ESP_ERR_NO_MEM;
}

void peer_manager_prune(void)
{
    int64_t now = get_time_sec();
    for (int i = 0; i < PEER_MAX_COUNT; i++) {
        if (s_peers[i].active) {
            if ((now - s_peers[i].last_seen) > PEER_TIMEOUT_SEC) {
                ESP_LOGI(TAG, "Peer timed out: %s", s_peers[i].hostname);
                s_peers[i].active = false;
            }
        }
    }
}

char *peer_manager_get_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *peers = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "peers", peers);

    int64_t now = get_time_sec();

    for (int i = 0; i < PEER_MAX_COUNT; i++) {
        if (s_peers[i].active) {
            cJSON *p = cJSON_CreateObject();
            cJSON_AddStringToObject(p, "hostname", s_peers[i].hostname);
            cJSON_AddStringToObject(p, "ip", s_peers[i].ip_addr);
            cJSON_AddStringToObject(p, "group", s_peers[i].group_id);
            cJSON_AddNumberToObject(p, "port", s_peers[i].port);
            cJSON_AddNumberToObject(p, "last_seen_ago", (double)(now - s_peers[i].last_seen));
            cJSON_AddItemToArray(peers, p);
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

const peer_t *peer_manager_get_list(int *count)
{
    if (count) *count = PEER_MAX_COUNT;
    return s_peers;
}
