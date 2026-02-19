#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "ha_integration.h"
#include "mimi_config.h"
#include "web_ui/web_ui.h"
#include "skills/skill_engine.h"
#include "skills/skill_types.h"
#include "tools/tool_registry.h"

static const char *TAG = "ha_integration";

/* Helper: Sanitize skill name for HA entity ID */
static void sanitize_entity_id(const char *name, char *out, size_t size)
{
    size_t i = 0;
    while (name[i] && i < size - 1) {
        char c = name[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
            out[i] = c;
        } else if (c >= 'A' && c <= 'Z') {
            out[i] = c + 32; /* tolower */
        } else {
            out[i] = '_';
        }
        i++;
    }
    out[i] = '\0';
}

/* 
 * API: GET /api/ha/state
 * Returns current state of exposed entities by iterating skills
 */
static esp_err_t ha_state_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    
    /* Device Info */
    cJSON_AddStringToObject(root, "device_id", "mimiclaw_s3");
    cJSON_AddStringToObject(root, "sw_version", "1.0.0");
    
    /* Iterate Skills and map to Entities */
    int count = skill_engine_get_count(); // just for loop limit hint
    (void)count;

    for (int i = 0; i < SKILL_MAX_SLOTS; i++) {
        const skill_slot_t *slot = skill_engine_get_slot(i);
        if (!slot) continue;

        char entity_id[64];
        char safe_name[32];
        sanitize_entity_id(slot->name, safe_name, sizeof(safe_name));

        if (slot->category == SKILL_CAT_SENSOR) {
            snprintf(entity_id, sizeof(entity_id), "sensor.%s", safe_name);
            cJSON *ent = cJSON_CreateObject();
            cJSON_AddStringToObject(ent, "entity_id", entity_id);
            cJSON_AddStringToObject(ent, "state", "unknown"); // TODO: Poll
            cJSON_AddStringToObject(ent, "attributes", slot->description);
            cJSON_AddItemToObject(root, safe_name, ent); // Key is short name
        } 
        else if (slot->category == SKILL_CAT_ACTUATOR) {
            snprintf(entity_id, sizeof(entity_id), "switch.%s", safe_name);
            cJSON *ent = cJSON_CreateObject();
            cJSON_AddStringToObject(ent, "entity_id", entity_id);
            cJSON_AddStringToObject(ent, "state", "off"); // TODO: Poll
            cJSON_AddItemToObject(root, safe_name, ent);
        }
    }

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
 * Body: {"entity_id": "switch.skill_name", "state": "on", ...}
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
    if (!cJSON_IsString(entity_id)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing entity_id");
        return ESP_FAIL;
    }

    /* Parse entity_id "switch.skill_name" -> "skill_name" */
    const char *dot = strchr(entity_id->valuestring, '.');
    if (!dot) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid entity_id format");
        return ESP_FAIL;
    }
    const char *skill_name_ptr = dot + 1;

    /* Find skill */
    const skill_slot_t *target_slot = NULL;
    for (int i = 0; i < SKILL_MAX_SLOTS; i++) {
        const skill_slot_t *slot = skill_engine_get_slot(i);
        if (slot) {
            char safe_name[32];
            sanitize_entity_id(slot->name, safe_name, sizeof(safe_name));
            if (strcmp(safe_name, skill_name_ptr) == 0) {
                target_slot = slot;
                break;
            }
        }
    }

    if (!target_slot) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Skill not found");
        return ESP_FAIL;
    }

    /* Find a suitable tool to execute */
    /* Heuristic: Exec the first tool for now, or look for 'control' */
    if (target_slot->tool_count == 0) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Skill has no tools");
        return ESP_FAIL;
    }

    /* Pass the whole JSON body as args to the tool */
    char output[512];
    /* Construct tool name: "skill_name.tool_name"? No, tool_registry names are just "tool_name" 
       BUT skill tools are usually registered with simple names. 
       Wait, skill_engine uses `register_tool_ctx` which registers with `slot->tool_names[i]`.
       If that name is not prefixed, we might have collisions, but currently it seems 1:1. 
    */
    const char *tool_name = target_slot->tool_names[0]; 
    if (tool_registry_execute(tool_name, buf, output, sizeof(output)) != ESP_OK) {
         ESP_LOGE(TAG, "Failed to execute tool %s", tool_name);
         cJSON_Delete(json);
         httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Tool execution failed");
         return ESP_FAIL;
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
    ESP_LOGI(TAG, "Initializing HA Integration (Dynamic)");
    return ESP_OK;
}

esp_err_t ha_integration_start(void)
{
    httpd_handle_t server = web_ui_get_server();
    if (!server) {
        ESP_LOGW(TAG, "HTTP Server not ready, cannot register HA endpoints");
        return ESP_FAIL;
    }

    esp_err_t err = httpd_register_uri_handler(server, &ha_state_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register %s: %s", ha_state_uri.uri, esp_err_to_name(err));
        return err;
    }

    err = httpd_register_uri_handler(server, &ha_control_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register %s: %s", ha_control_uri.uri, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "HA Integration started. Endpoints registered.");
    return ESP_OK;
}
