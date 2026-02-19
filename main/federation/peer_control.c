#include "federation/peer_control.h"
#include "federation/peer_manager.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "peer_ctrl";

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (!evt->user_data) return ESP_OK;
        // Simple accumulation for short responses
        // In a real app, handle large payloads with a proper buffer struct
        char *buf = (char *)evt->user_data;
        int current_len = strlen(buf);
        int max_len = 4096; // Assumption based on typical usage passed in
        
        if (current_len + evt->data_len < max_len) {
            memcpy(buf + current_len, evt->data, evt->data_len);
            buf[current_len + evt->data_len] = '\0';
        }
    }
    return ESP_OK;
}

esp_err_t peer_control_execute_tool(const char *target_ip, const char *tool_name, 
                                    const char *json_args, char *output, size_t output_len)
{
    if (!target_ip || !tool_name || !output) return ESP_ERR_INVALID_ARG;

    memset(output, 0, output_len);

    /* Construct URL */
    char url[64];
    snprintf(url, sizeof(url), "http://%s/api/tools/exec", target_ip);

    /* Construct JSON body: {"tool": "name", "args": ...} */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "tool", tool_name);
    
    /* Argument handling: if json_args is a JSON string, parse it first to embed as object, 
       or just pass as string if tool_registry expects string? 
       Our /api/tools/exec handler expects "args" as an object OR string, 
       but tool_registry needs a string. 
       Let's verify how we implemented server side. 
       Server: cJSON_GetObjectItem(root, "args") -> cJSON_PrintUnformatted.
       So if we send an object here, server gets object and prints it back to string.
       If we send string, server gets string? No cJSON_GetObjectItem works on objects.
       
       Let's try to parse json_args. If valid JSON, add as Item. If not, add as String.
    */
    cJSON *args_obj = cJSON_Parse(json_args);
    if (args_obj) {
        cJSON_AddItemToObject(root, "args", args_obj);
    } else {
        cJSON_AddStringToObject(root, "args", json_args ? json_args : "{}");
    }

    char *post_data = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!post_data) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "Sending tool exec '%s' to %s", tool_name, target_ip);

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = output,
        .timeout_ms = 10000,
        .method = HTTP_METHOD_POST,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status >= 200 && status < 300) {
            ESP_LOGI(TAG, "Success, status=%d", status);
        } else {
            ESP_LOGW(TAG, "Failed, status=%d", status);
            snprintf(output, output_len, "HTTP Error %d", status);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "Request failed: %s", esp_err_to_name(err));
        snprintf(output, output_len, "Connection failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    free(post_data);
    return err;
}
