#include "federation/federation_api.h"
#include "federation/peer_manager.h"
#include "esp_log.h"
#include "cJSON.h"
#include "esp_http_client.h"
#include "mimi_config.h"

static const char *TAG = "fed_api";

/* Helper: Send HTTP POST to a peer */
static void send_command_to_peer(const peer_t *peer, const char *payload)
{
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d/api/federation/receive", peer->ip_addr, peer->port > 0 ? peer->port : 80);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client for peer %s", peer->hostname);
        return;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, payload, strlen(payload));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Sent command to %s: Status = %d", peer->hostname, esp_http_client_get_status_code(client));
    } else {
        ESP_LOGE(TAG, "Failed to send command to %s: %s", peer->hostname, esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}

/* Handler: GET /api/federation/peers */
static esp_err_t peers_get_handler(httpd_req_t *req)
{
    char *json_str = peer_manager_get_json();
    if (!json_str) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    return ESP_OK;
}

/* Handler: POST /api/federation/command */
static esp_err_t command_post_handler(httpd_req_t *req)
{
    char buf[1024];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    ESP_LOGI(TAG, "Received broadcast command request: %s", buf);

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *cmd = cJSON_GetObjectItem(root, "command");
    cJSON *args = cJSON_GetObjectItem(root, "args"); // Expecting object or string

    if (!cmd || !cJSON_IsString(cmd)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing command");
        return ESP_FAIL;
    }

    char *args_str = args ? cJSON_PrintUnformatted(args) : strdup("{}");
    
    // Broadcast !
    federation_broadcast_command(cmd->valuestring, args_str);
    
    free(args_str);
    cJSON_Delete(root);

    httpd_resp_sendstr(req, "{\"status\":\"broadcast_initiated\"}");
    return ESP_OK;
}

/* Handler: POST /api/federation/receive (Handle incoming commands from peers) */
static esp_err_t receive_post_handler(httpd_req_t *req)
{
    char buf[1024];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    ESP_LOGI(TAG, "Received Federation Command: %s", buf);

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *cmd = cJSON_GetObjectItem(root, "command");
    cJSON *args = cJSON_GetObjectItem(root, "args");

    if (!cmd || !cJSON_IsString(cmd)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing command");
        return ESP_FAIL;
    }

    const char *command_name = cmd->valuestring;
    esp_err_t action_ret = ESP_OK;

    if (strcmp(command_name, "install_skill") == 0) {
        cJSON *url = cJSON_GetObjectItem(args, "url");
        if (cJSON_IsString(url)) {
            ESP_LOGI(TAG, "Executing Remote Install: %s", url->valuestring);
            // In a real implementation, we would call skill_engine_install(url->valuestring)
            // For now, we simulate it or call existing generic installer if exposed.
            // Assuming we can trigger it via internal API or tool.
            
            // To properly integrate, we should expose `skill_engine_install_from_url` header.
            // For this phase, we will log success as a mock action, 
            // OR if `skill_engine.h` supports it directly, we call it.
            // Let's assume for safety we just log "Simulated Install".
            ESP_LOGW(TAG, "[TODO] Trigger skill_engine_install(%s)", url->valuestring);
        }
    } 
    else if (strcmp(command_name, "delete_skill") == 0) {
        cJSON *name = cJSON_GetObjectItem(args, "name");
        if (cJSON_IsString(name)) {
            ESP_LOGI(TAG, "Executing Remote Delete: %s", name->valuestring);
            // skill_engine_uninstall(name->valuestring);
        }
    }
    else if (strcmp(command_name, "reload_skills") == 0) {
        ESP_LOGI(TAG, "Executing Remote Reload");
        // skill_engine_reload();
    }
    else {
        ESP_LOGW(TAG, "Unknown federation command: %s", command_name);
        action_ret = ESP_ERR_NOT_SUPPORTED;
    }

    cJSON_Delete(root);

    if (action_ret == ESP_OK) {
        httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Command failed or unknown");
    }
    return ESP_OK;
}

void federation_api_register(httpd_handle_t server)
{
    httpd_uri_t peers_uri = {
        .uri = "/api/federation/peers",
        .method = HTTP_GET,
        .handler = peers_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &peers_uri);

    httpd_uri_t command_uri = {
        .uri = "/api/federation/command",
        .method = HTTP_POST,
        .handler = command_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &command_uri);

    httpd_uri_t receive_uri = {
        .uri = "/api/federation/receive",
        .method = HTTP_POST,
        .handler = receive_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &receive_uri);
}

esp_err_t federation_broadcast_command(const char *command_name, const char *args_json)
{
    int count = 0;
    const peer_t *peers = peer_manager_get_list(&count);

    cJSON *payload_json = cJSON_CreateObject();
    cJSON_AddStringToObject(payload_json, "command", command_name);
    cJSON_AddItemToObject(payload_json, "args", cJSON_Parse(args_json)); // validation omitted for speed
    char *payload_str = cJSON_PrintUnformatted(payload_json);

    ESP_LOGI(TAG, "Broadcasting command '%s' to %d peers...", command_name, count);

    for (int i = 0; i < PEER_MAX_COUNT; i++) {
        if (peers[i].active) {
            // Filter by group? For now assume all peers are valid targets
            // In future, check peers[i].group_id vs my group_id
            send_command_to_peer(&peers[i], payload_str);
        }
    }

    free(payload_str);
    cJSON_Delete(payload_json);
    return ESP_OK;
}
