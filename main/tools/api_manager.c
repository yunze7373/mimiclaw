#include "tools/api_manager.h"
#include "tools/tool_registry.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "api_mgr";
#define CONFIG_PATH "/spiffs/config/api_skills.json"
#define MAX_API_SKILLS 16

typedef struct {
    char name[32];
    char description[128];
    char method[8];
    char url[256];
    char input_schema[512];
    bool enabled;
} api_skill_t;

static api_skill_t s_skills[MAX_API_SKILLS];
static int s_skill_count = 0;

/* ── Helper: Load Config ─────────────────────────────────────────── */

static void load_config(void)
{
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        ESP_LOGW(TAG, "No API skills config found at %s", CONFIG_PATH);
        return;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *data = malloc(len + 1);
    if (!data) {
        fclose(f);
        return;
    }

    fread(data, 1, len, f);
    data[len] = 0;
    fclose(f);

    cJSON *root = cJSON_Parse(data);
    if (root) {
        cJSON *skills = cJSON_GetObjectItem(root, "skills");
        cJSON *item = NULL;
        s_skill_count = 0;

        cJSON_ArrayForEach(item, skills) {
            if (s_skill_count >= MAX_API_SKILLS) break;
            
            cJSON *name = cJSON_GetObjectItem(item, "name");
            cJSON *desc = cJSON_GetObjectItem(item, "description");
            cJSON *method = cJSON_GetObjectItem(item, "method");
            cJSON *url = cJSON_GetObjectItem(item, "url");
            cJSON *schema = cJSON_GetObjectItem(item, "input_schema");

            if (cJSON_IsString(name) && cJSON_IsString(url)) {
                api_skill_t *sk = &s_skills[s_skill_count++];
                strncpy(sk->name, name->valuestring, sizeof(sk->name)-1);
                strncpy(sk->url, url->valuestring, sizeof(sk->url)-1);
                
                if (cJSON_IsString(desc)) strncpy(sk->description, desc->valuestring, sizeof(sk->description)-1);
                else sk->description[0] = 0;

                if (cJSON_IsString(method)) strncpy(sk->method, method->valuestring, sizeof(sk->method)-1);
                else strcpy(sk->method, "GET");

                if (schema) {
                    char *s = cJSON_PrintUnformatted(schema);
                    if (s) {
                        strncpy(sk->input_schema, s, sizeof(sk->input_schema)-1);
                        free(s);
                    }
                } else {
                    strcpy(sk->input_schema, "{\"type\":\"object\",\"properties\":{}}");
                }
                sk->enabled = true;
                ESP_LOGI(TAG, "Loaded API Skill: %s", sk->name);
            }
        }
        cJSON_Delete(root);
    }
    free(data);
}

/* ── Tool Provider Implementation ────────────────────────────────── */

static char *api_provider_get_tools_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < s_skill_count; i++) {
        cJSON *tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", s_skills[i].name);
        cJSON_AddStringToObject(tool, "description", s_skills[i].description);
        
        cJSON *schema = cJSON_Parse(s_skills[i].input_schema);
        if (schema) {
            cJSON_AddItemToObject(tool, "input_schema", schema);
        }
        cJSON_AddItemToArray(arr, tool);
    }
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return json;
}

typedef struct {
    char *buffer;
    size_t size;
    size_t len;
} response_accum_t;

static esp_err_t client_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        response_accum_t *acc = (response_accum_t *)evt->user_data;
        if (acc && acc->len < acc->size - 1) {
            size_t copy_len = evt->data_len;
            if (acc->len + copy_len >= acc->size) {
                copy_len = acc->size - acc->len - 1;
            }
            memcpy(acc->buffer + acc->len, evt->data, copy_len);
            acc->len += copy_len;
            acc->buffer[acc->len] = 0;
        }
    }
    return ESP_OK;
}

static esp_err_t api_provider_execute_tool(const char *tool_name, const char *input_json, char *output, size_t output_size)
{
    /* Find skill */
    api_skill_t *skill = NULL;
    for (int i = 0; i < s_skill_count; i++) {
        if (strcmp(s_skills[i].name, tool_name) == 0) {
            skill = &s_skills[i];
            break;
        }
    }
    if (!skill) return ESP_ERR_NOT_FOUND;

    ESP_LOGI(TAG, "Executing API Skill: %s", tool_name);

    /* Construct URL (Append query params for GET) */
    /* This is a simple MVP. Does not handle complex parameter substitution or POST body yet. */
    /* Only appends ?key=value */
    
    char final_url[512];
    strncpy(final_url, skill->url, sizeof(final_url)-1);

    if (strcasecmp(skill->method, "GET") == 0 && input_json) {
        cJSON *args = cJSON_Parse(input_json);
        if (args) {
            bool first = (strchr(final_url, '?') == NULL);
            cJSON *item = NULL;
            cJSON_ArrayForEach(item, args) {
                if (cJSON_IsString(item) || cJSON_IsNumber(item)) {
                    char *val_str = cJSON_GetStringValue(item);
                    /* Simple string append (no url encode for MVP, risk!) */
                    strncat(final_url, first ? "?" : "&", sizeof(final_url) - strlen(final_url) - 1);
                    strncat(final_url, item->string, sizeof(final_url) - strlen(final_url) - 1);
                    strncat(final_url, "=", sizeof(final_url) - strlen(final_url) - 1);
                    if (val_str) {
                         strncat(final_url, val_str, sizeof(final_url) - strlen(final_url) - 1);
                    } else if (cJSON_IsNumber(item)) {
                        char numbuf[32];
                        snprintf(numbuf, sizeof(numbuf), "%g", item->valuedouble);
                         strncat(final_url, numbuf, sizeof(final_url) - strlen(final_url) - 1);
                    }
                    first = false;
                }
            }
            cJSON_Delete(args);
        }
    }
    
    /* Prepare HTTP Client */
    response_accum_t acc = { .buffer = output, .size = output_size, .len = 0 };
    output[0] = 0; /* Clear output */

    esp_http_client_config_t config = {
        .url = final_url,
        .method = (strcasecmp(skill->method, "POST") == 0) ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .event_handler = client_event_handler,
        .user_data = &acc,
        .timeout_ms = 10000,
        .buffer_size = 2048, /* Rx buffer */
        .disable_auto_redirect = false,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
         snprintf(output, output_size, "{\"error\":\"Failed to init HTTP client\"}");
         return ESP_OK;
    }

    /* If POST, set body (raw JSON args for MVP) */
    if (config.method == HTTP_METHOD_POST) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, input_json, strlen(input_json));
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status >= 200 && status < 300) {
             /* Result is in acc.buffer via event handler */
             if (acc.len == 0) snprintf(output, output_size, "{\"status\":\"OK\"}");
        } else {
             snprintf(output, output_size, "{\"error\":\"HTTP %d\"}", status);
        }
    } else {
        snprintf(output, output_size, "{\"error\":\"Request failed: %s\"}", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return ESP_OK;
}

static const tool_provider_t s_api_provider = {
    .name = "api_skills",
    .get_tools_json = api_provider_get_tools_json,
    .execute_tool = api_provider_execute_tool
};

/* ── Init ──────────────────────────────────────────────────────────── */

esp_err_t api_manager_init(void)
{
    load_config();
    if (s_skill_count > 0) {
        tool_registry_register_provider(&s_api_provider);
        ESP_LOGI(TAG, "Registered API Manager with %d skills", s_skill_count);
    } else {
        ESP_LOGI(TAG, "No API skills configured");
    }
    return ESP_OK;
}
