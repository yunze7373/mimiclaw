#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "ha_integration.h"
#include "mimi_config.h"
#include "rgb/rgb.h"
#include "web_ui/web_ui.h"

static const char *TAG = "ha_integration";

/* 
 * API: GET /api/ha/state
 * Returns current state of exposed entities
 */
static esp_err_t ha_state_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    
    /* Device Info */
    cJSON_AddStringToObject(root, "device_id", "mimiclaw_s3");
    cJSON_AddStringToObject(root, "sw_version", "1.0.0");
    
    /* RGB Light Entity */
    cJSON *rgb = cJSON_CreateObject();
    cJSON_AddStringToObject(rgb, "entity_id", "light.mimiclaw_rgb");
    cJSON_AddStringToObject(rgb, "state", "on"); // TODO: Get actual state
    
    // Get actual color
    uint8_t r, g, b;
    // Assuming a getter exists or we track it. For now, placeholder.
    r = 0; g = 255; b = 0; 
    
    cJSON *color = cJSON_CreateArray();
    cJSON_AddItemToArray(color, cJSON_CreateNumber(r));
    cJSON_AddItemToArray(color, cJSON_CreateNumber(g));
    cJSON_AddItemToArray(color, cJSON_CreateNumber(b));
    cJSON_AddItemToObject(rgb, "rgb_color", color);
    
    cJSON_AddItemToObject(root, "light", rgb);

    /* System Sensor */
    cJSON *sensor = cJSON_CreateObject();
    cJSON_AddStringToObject(sensor, "entity_id", "sensor.mimiclaw_status");
    cJSON_AddStringToObject(sensor, "state", "online");
    cJSON_AddNumberToObject(sensor, "uptime", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddItemToObject(root, "sensor", sensor);

    const char *resp = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    if (resp) {
        httpd_resp_send(req, resp, strlen(resp));
        free((void *)resp);
    } else {
        httpd_resp_send(req, "{}", 2);
    }
    
    cJSON_Delete(root);
    return ESP_OK;
}

/* 
 * API: POST /api/ha/control
 * Body: {"entity_id": "light.mimiclaw_rgb", "state": "on", "attributes": {"rgb_color": [255,0,0]}}
 */
static esp_err_t ha_control_handler(httpd_req_t *req)
{
    char buf[1024];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON *entity_id = cJSON_GetObjectItem(json, "entity_id");
    if (cJSON_IsString(entity_id) && strcmp(entity_id->valuestring, "light.mimiclaw_rgb") == 0) {
        cJSON *state = cJSON_GetObjectItem(json, "state");
        cJSON *attrs = cJSON_GetObjectItem(json, "attributes");
        
        if (state && strcmp(state->valuestring, "off") == 0) {
            rgb_set(0, 0, 0);
        } else if (attrs) {
            cJSON *rgb = cJSON_GetObjectItem(attrs, "rgb_color");
            if (cJSON_IsArray(rgb) && cJSON_GetArraySize(rgb) == 3) {
                int r = cJSON_GetArrayItem(rgb, 0)->valueint;
                int g = cJSON_GetArrayItem(rgb, 1)->valueint;
                int b = cJSON_GetArrayItem(rgb, 2)->valueint;
                rgb_set(r, g, b);
            }
        }
        ESP_LOGI(TAG, "HA Control: RGB updated");
    }

    cJSON_Delete(json);
    httpd_resp_send(req, "{\"status\":\"ok\"}", 15);
    return ESP_OK;
}

static const httpd_uri_t ha_state_uri = {
    .uri       = "/api/ha/state",
    .method    = HTTP_GET,
    .handler   = ha_state_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t ha_control_uri = {
    .uri       = "/api/ha/control",
    .method    = HTTP_POST,
    .handler   = ha_control_handler,
    .user_ctx  = NULL
};

esp_err_t ha_integration_init(void)
{
    ESP_LOGI(TAG, "Initializing HA Integration");
    // Registration happens in start(), assuming server is ready then
    return ESP_OK;
}

esp_err_t ha_integration_start(void)
{
    httpd_handle_t server = web_ui_get_server();
    if (!server) {
        ESP_LOGW(TAG, "HTTP Server not ready, cannot register HA endpoints");
        return ESP_FAIL;
    }
    
    httpd_register_uri_handler(server, &ha_state_uri);
    httpd_register_uri_handler(server, &ha_control_uri);
    
    ESP_LOGI(TAG, "HA Integration started. Endpoints registered.");
    return ESP_OK;
}
