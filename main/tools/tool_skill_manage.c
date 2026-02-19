#include "tools/tool_skill_manage.h"
#include "cJSON.h"
#include "skills/skill_engine.h"
#include "skills/skill_rollback.h"
#include <string.h>
#include <stdio.h>

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
        /* List installed skills via current public engine API */
        char *json = skill_engine_list_json();
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
            if (err == ESP_OK || err == ESP_ERR_NOT_FOUND) {
                if (err == ESP_ERR_NOT_FOUND) {
                    snprintf(output, output_size, "Skill '%s' already removed (not found).", name);
                } else {
                snprintf(output, output_size, "Skill '%s' deleted successfully.", name);
                }
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
