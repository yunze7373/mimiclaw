#include "tools/tool_skill_manage.h"
#include "cJSON.h"
#include "esp_log.h"
#include "skills/skill_engine.h"
#include "skills/skill_rollback.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "tool_skill_manage";

esp_err_t tool_skill_manage_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: Invalid JSON input");
        return ESP_FAIL;
    }

    cJSON *action_item = cJSON_GetObjectItem(root, "action");
    if (!cJSON_IsString(action_item)) {
        snprintf(output, output_size, "Error: 'action' is required (list|delete|reload)");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    const char *action = action_item->valuestring;
    esp_err_t ret = ESP_OK;

    if (strcmp(action, "list") == 0) {
        /* List all installed skills */
        /* Currently we don't have a public API for getting skill list as JSON object from engine.
           But we can iterate SPIFFS or use an internal helper if available. 
           Wait, there is `skill_engine_get_list_json()`? No? 
           Let's check `web_ui.c` skills_get_handler implementation. 
           It iterates the internal list. 
           Actually `skill_engine.h` exposes `skill_engine_get_count` and `skill_engine_get_skill(i)`.
           Let's use that.
        */
        cJSON *arr = cJSON_CreateArray();
        int count = skill_engine_get_count();
        for (int i = 0; i < count; i++) {
            const skill_t *sk = skill_engine_get_skill(i);
            if (sk) {
                cJSON *obj = cJSON_CreateObject();
                cJSON_AddStringToObject(obj, "name", sk->name);
                cJSON_AddBoolToObject(obj, "enabled", sk->enabled);
                cJSON_AddStringToObject(obj, "status", sk->status == SKILL_STATUS_RUNNING ? "running" : "stopped");
                cJSON_AddItemToArray(arr, obj);
            }
        }
        char *json = cJSON_PrintUnformatted(arr);
        cJSON_Delete(arr);
        if (json) {
            snprintf(output, output_size, "%s", json);
            free(json);
        } else {
            snprintf(output, output_size, "[]");
        }
    } 
    else if (strcmp(action, "delete") == 0) {
        cJSON *name_item = cJSON_GetObjectItem(root, "name");
        if (!cJSON_IsString(name_item)) {
            snprintf(output, output_size, "Error: 'name' is required for delete action");
            ret = ESP_FAIL;
        } else {
            const char *name = name_item->valuestring;
            esp_err_t err = skill_engine_uninstall(name);
            if (err == ESP_OK) {
                snprintf(output, output_size, "Skill '%s' deleted successfully.", name);
            } else {
                snprintf(output, output_size, "Failed to delete skill '%s': %s", name, esp_err_to_name(err));
                ret = ESP_FAIL;
            }
        }
    }
    else if (strcmp(action, "reload") == 0) {
        esp_err_t err = skill_engine_init();
        if (err == ESP_OK) {
            snprintf(output, output_size, "Skill engine reloaded successfully.");
        } else {
            snprintf(output, output_size, "Failed to reload skill engine: %s", esp_err_to_name(err));
            ret = ESP_FAIL;
        }
    }
    else {
        snprintf(output, output_size, "Error: Unknown action '%s'. Supported: list, delete, reload.", action);
        ret = ESP_FAIL;
    }

    cJSON_Delete(root);
    return ret;
}
