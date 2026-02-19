#include "skills/api_skill.h"
#include "mimi_config.h"
#include "tools/tool_registry.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_http_client.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "api_skill";

/* ── In-memory registry of loaded API skills ─────────────────── */

#define MAX_API_SKILLS   4
#define MAX_ENDPOINTS    8

typedef struct {
    char name[32];
    char method[8];            /* GET, POST, PUT, DELETE */
    char path[64];
    char description[128];
    char tool_name[48];        /* Registered tool name: <skill>_<endpoint> */
    char input_schema_json[512];
} api_endpoint_t;

typedef struct {
    bool active;
    char skill_name[32];
    char base_url[128];

    /* Auth */
    enum { AUTH_NONE, AUTH_BEARER, AUTH_API_KEY, AUTH_BASIC } auth_type;
    char auth_token[128];       /* Bearer token or API key value */
    char auth_header[32];       /* Header name for API key (default: Authorization) */
    char auth_user[32];         /* Basic auth user */

    /* Endpoints */
    api_endpoint_t endpoints[MAX_ENDPOINTS];
    int endpoint_count;
} api_skill_t;

static api_skill_t s_skills[MAX_API_SKILLS];

/* ── HTTP event handler for response collection ──────────────── */

typedef struct {
    char *data;
    int   len;
    int   capacity;
} api_buf_t;

static esp_err_t api_http_event_handler(esp_http_client_event_t *evt)
{
    api_buf_t *buf = (api_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && buf) {
        int avail = buf->capacity - buf->len - 1;
        int copy = evt->data_len < avail ? evt->data_len : avail;
        if (copy > 0) {
            memcpy(buf->data + buf->len, evt->data, copy);
            buf->len += copy;
            buf->data[buf->len] = '\0';
        }
    }
    return ESP_OK;
}

/* ── Generic tool executor for API endpoints ─────────────────── */

static esp_err_t api_endpoint_execute(const char *input_json, char *output, size_t output_size)
{
    /* Parse to find which tool_name this is */
    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "{\"error\":\"invalid input\"}");
        return ESP_OK;
    }

    /* Extract _skill and _endpoint from the input to identify which API to call */
    cJSON *j_skill = cJSON_GetObjectItem(input, "_skill");
    cJSON *j_ep = cJSON_GetObjectItem(input, "_endpoint");
    if (!j_skill || !j_ep) {
        cJSON_Delete(input);
        snprintf(output, output_size, "{\"error\":\"missing _skill/_endpoint\"}");
        return ESP_OK;
    }

    const char *skill_name = j_skill->valuestring;
    int ep_idx = j_ep->valueint;

    /* Find the skill */
    api_skill_t *sk = NULL;
    for (int i = 0; i < MAX_API_SKILLS; i++) {
        if (s_skills[i].active && strcmp(s_skills[i].skill_name, skill_name) == 0) {
            sk = &s_skills[i];
            break;
        }
    }

    if (!sk || ep_idx < 0 || ep_idx >= sk->endpoint_count) {
        cJSON_Delete(input);
        snprintf(output, output_size, "{\"error\":\"api skill not found\"}");
        return ESP_OK;
    }

    api_endpoint_t *ep = &sk->endpoints[ep_idx];

    /* Build URL */
    char url[256];
    snprintf(url, sizeof(url), "%s%s", sk->base_url, ep->path);

    /* Append query params for GET */
    if (strcmp(ep->method, "GET") == 0) {
        /* Iterate input JSON fields (skip _skill, _endpoint) and append as query params */
        char query[256] = {0};
        int qlen = 0;
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, input) {
            if (item->string[0] == '_') continue;  /* Skip internal fields */
            if (qlen == 0) {
                qlen += snprintf(query + qlen, sizeof(query) - qlen, "?%s=%s",
                                 item->string,
                                 cJSON_IsString(item) ? item->valuestring : "");
            } else {
                qlen += snprintf(query + qlen, sizeof(query) - qlen, "&%s=%s",
                                 item->string,
                                 cJSON_IsString(item) ? item->valuestring : "");
            }
        }
        if (qlen > 0) {
            strncat(url, query, sizeof(url) - strlen(url) - 1);
        }
    }

    /* Response buffer */
    api_buf_t buf = {0};
    buf.capacity = 4096;
    buf.data = malloc(buf.capacity);
    if (!buf.data) {
        cJSON_Delete(input);
        snprintf(output, output_size, "{\"error\":\"no memory\"}");
        return ESP_OK;
    }
    buf.data[0] = '\0';

    /* Create HTTP client */
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = api_http_event_handler,
        .user_data = &buf,
        .timeout_ms = 15000,
        .buffer_size = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(buf.data);
        cJSON_Delete(input);
        snprintf(output, output_size, "{\"error\":\"http init failed\"}");
        return ESP_OK;
    }

    /* Set method */
    if (strcmp(ep->method, "POST") == 0) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
    } else if (strcmp(ep->method, "PUT") == 0) {
        esp_http_client_set_method(client, HTTP_METHOD_PUT);
    } else if (strcmp(ep->method, "DELETE") == 0) {
        esp_http_client_set_method(client, HTTP_METHOD_DELETE);
    }

    /* Auth headers */
    if (sk->auth_type == AUTH_BEARER) {
        char auth_val[160];
        snprintf(auth_val, sizeof(auth_val), "Bearer %s", sk->auth_token);
        esp_http_client_set_header(client, "Authorization", auth_val);
    } else if (sk->auth_type == AUTH_API_KEY) {
        esp_http_client_set_header(client,
            sk->auth_header[0] ? sk->auth_header : "X-API-Key",
            sk->auth_token);
    }

    /* POST body */
    if (strcmp(ep->method, "POST") == 0 || strcmp(ep->method, "PUT") == 0) {
        /* Build body from non-internal input fields */
        cJSON *body = cJSON_CreateObject();
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, input) {
            if (item->string[0] == '_') continue;
            cJSON_AddItemToObject(body, item->string, cJSON_Duplicate(item, true));
        }
        char *body_str = cJSON_PrintUnformatted(body);
        cJSON_Delete(body);
        if (body_str) {
            esp_http_client_set_header(client, "Content-Type", "application/json");
            esp_http_client_set_post_field(client, body_str, strlen(body_str));
        }

        esp_err_t err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);
        free(body_str);

        if (err != ESP_OK) {
            free(buf.data);
            cJSON_Delete(input);
            snprintf(output, output_size, "{\"error\":\"request failed: %s\"}", esp_err_to_name(err));
            return ESP_OK;
        }

        snprintf(output, output_size, "{\"status\":%d,\"body\":%s}",
                 status, buf.data[0] ? buf.data : "null");
    } else {
        esp_err_t err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        if (err != ESP_OK) {
            free(buf.data);
            cJSON_Delete(input);
            snprintf(output, output_size, "{\"error\":\"request failed: %s\"}", esp_err_to_name(err));
            return ESP_OK;
        }

        snprintf(output, output_size, "{\"status\":%d,\"body\":%s}",
                 status, buf.data[0] ? buf.data : "null");
    }

    free(buf.data);
    cJSON_Delete(input);
    return ESP_OK;
}

/* ── Public API ──────────────────────────────────────────────────── */

esp_err_t api_skill_load(const char *name, const char *config_json)
{
    if (!name || !config_json) return ESP_ERR_INVALID_ARG;
    char skill_name_local[sizeof(s_skills[0].skill_name)];
    snprintf(skill_name_local, sizeof(skill_name_local), "%s", name);

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < MAX_API_SKILLS; i++) {
        if (!s_skills[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        ESP_LOGE(TAG, "No free API skill slot");
        return ESP_ERR_NO_MEM;
    }

    cJSON *config = cJSON_Parse(config_json);
    if (!config) {
        ESP_LOGE(TAG, "Invalid config JSON for '%s'", name);
        return ESP_ERR_INVALID_ARG;
    }

    api_skill_t *sk = &s_skills[slot];
    memset(sk, 0, sizeof(*sk));
    snprintf(sk->skill_name, sizeof(sk->skill_name), "%s", skill_name_local);

    /* base_url */
    cJSON *base_url = cJSON_GetObjectItem(config, "base_url");
    if (cJSON_IsString(base_url)) {
        snprintf(sk->base_url, sizeof(sk->base_url), "%s", base_url->valuestring);
    }

    /* auth */
    cJSON *auth = cJSON_GetObjectItem(config, "auth");
    if (auth) {
        cJSON *auth_type = cJSON_GetObjectItem(auth, "type");
        if (cJSON_IsString(auth_type)) {
            if (strcmp(auth_type->valuestring, "bearer") == 0) {
                sk->auth_type = AUTH_BEARER;
                cJSON *token = cJSON_GetObjectItem(auth, "token");
                if (cJSON_IsString(token)) snprintf(sk->auth_token, sizeof(sk->auth_token), "%s", token->valuestring);
            } else if (strcmp(auth_type->valuestring, "api_key") == 0) {
                sk->auth_type = AUTH_API_KEY;
                cJSON *key = cJSON_GetObjectItem(auth, "key");
                if (cJSON_IsString(key)) snprintf(sk->auth_token, sizeof(sk->auth_token), "%s", key->valuestring);
                cJSON *header = cJSON_GetObjectItem(auth, "header");
                if (cJSON_IsString(header)) snprintf(sk->auth_header, sizeof(sk->auth_header), "%s", header->valuestring);
            } else if (strcmp(auth_type->valuestring, "basic") == 0) {
                sk->auth_type = AUTH_BASIC;
                cJSON *user = cJSON_GetObjectItem(auth, "user");
                if (cJSON_IsString(user)) snprintf(sk->auth_user, sizeof(sk->auth_user), "%s", user->valuestring);
                cJSON *pass = cJSON_GetObjectItem(auth, "pass");
                if (cJSON_IsString(pass)) snprintf(sk->auth_token, sizeof(sk->auth_token), "%s", pass->valuestring);
            }
        }
    }

    /* endpoints */
    cJSON *endpoints = cJSON_GetObjectItem(config, "endpoints");
    if (cJSON_IsArray(endpoints)) {
        int count = cJSON_GetArraySize(endpoints);
        if (count > MAX_ENDPOINTS) count = MAX_ENDPOINTS;

        for (int i = 0; i < count; i++) {
            cJSON *ep = cJSON_GetArrayItem(endpoints, i);
            api_endpoint_t *e = &sk->endpoints[i];

            cJSON *ep_name = cJSON_GetObjectItem(ep, "name");
            cJSON *ep_desc = cJSON_GetObjectItem(ep, "description");
            cJSON *ep_method = cJSON_GetObjectItem(ep, "method");
            cJSON *ep_path = cJSON_GetObjectItem(ep, "path");

            if (cJSON_IsString(ep_name)) snprintf(e->name, sizeof(e->name), "%s", ep_name->valuestring);
            if (cJSON_IsString(ep_desc)) snprintf(e->description, sizeof(e->description), "%s", ep_desc->valuestring);
            if (cJSON_IsString(ep_method)) snprintf(e->method, sizeof(e->method), "%s", ep_method->valuestring);
            if (cJSON_IsString(ep_path)) snprintf(e->path, sizeof(e->path), "%s", ep_path->valuestring);

            /* Tool name: <skill_name>_<endpoint_name> */
            snprintf(e->tool_name, sizeof(e->tool_name), "%s_%s", skill_name_local, e->name);

            /* Build tool input schema from params */
            cJSON *params = cJSON_GetObjectItem(ep, "params");
            char schema_buf[512];
            if (params) {
                /* Build JSON object schema with _skill and _endpoint injected */
                int slen = snprintf(schema_buf, sizeof(schema_buf),
                    "{\"type\":\"object\",\"properties\":{"
                    "\"_skill\":{\"type\":\"string\",\"const\":\"%s\"},"
                    "\"_endpoint\":{\"type\":\"integer\",\"const\":%d}",
                    skill_name_local, i);

                cJSON *param = NULL;
                cJSON_ArrayForEach(param, params) {
                    cJSON *ptype = cJSON_GetObjectItem(param, "type");
                    slen += snprintf(schema_buf + slen, sizeof(schema_buf) - slen,
                        ",\"%s\":{\"type\":\"%s\"}",
                        param->string,
                        cJSON_IsString(ptype) ? ptype->valuestring : "string");
                }
                slen += snprintf(schema_buf + slen, sizeof(schema_buf) - slen, "}}");
            } else {
                snprintf(schema_buf, sizeof(schema_buf),
                    "{\"type\":\"object\",\"properties\":{"
                    "\"_skill\":{\"type\":\"string\",\"const\":\"%s\"},"
                    "\"_endpoint\":{\"type\":\"integer\",\"const\":%d}"
                    "}}", skill_name_local, i);
            }

            snprintf(e->input_schema_json, sizeof(e->input_schema_json), "%s", schema_buf);

            /* Register tool */
            mimi_tool_t tool = {
                .name = e->tool_name,
                .description = e->description,
                .input_schema_json = e->input_schema_json,
                .execute = api_endpoint_execute,
            };
            tool_registry_register(&tool);

            ESP_LOGI(TAG, "Registered API tool: %s", e->tool_name);
        }
        sk->endpoint_count = count;
    }

    sk->active = true;
    cJSON_Delete(config);

    ESP_LOGI(TAG, "API skill '%s' loaded (%d endpoints)", skill_name_local, sk->endpoint_count);
    return ESP_OK;
}

esp_err_t api_skill_unload(const char *name)
{
    for (int i = 0; i < MAX_API_SKILLS; i++) {
        if (s_skills[i].active && strcmp(s_skills[i].skill_name, name) == 0) {
            /* Unregister tools */
            for (int j = 0; j < s_skills[i].endpoint_count; j++) {
                tool_registry_unregister(s_skills[i].endpoints[j].tool_name);
            }
            s_skills[i].active = false;
            ESP_LOGI(TAG, "API skill '%s' unloaded", name);
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

bool api_skill_is_loaded(const char *name)
{
    for (int i = 0; i < MAX_API_SKILLS; i++) {
        if (s_skills[i].active && strcmp(s_skills[i].skill_name, name) == 0) {
            return true;
        }
    }
    return false;
}
