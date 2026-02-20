#include "tools/tool_registry.h"
#include "audio/audio_manager.h"
#include "cJSON.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "tool_audio";

/* -------------------------------------------------------------------------
 * Tool: audio_play_url
 * Input: {"url": "https://..."}
 * ------------------------------------------------------------------------- */
static esp_err_t tool_audio_play_url(const char *input, char *output, size_t out_len)
{
    cJSON *root = cJSON_Parse(input);
    if (!root) {
        snprintf(output, out_len, "Error: Invalid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *url_item = cJSON_GetObjectItem(root, "url");
    if (cJSON_IsString(url_item) && (url_item->valuestring != NULL)) {
        esp_err_t err = audio_manager_play_url(url_item->valuestring);
        if (err == ESP_OK) {
            snprintf(output, out_len, "Started playing: %s", url_item->valuestring);
        } else {
            snprintf(output, out_len, "Failed to start playback (Error %d)", err);
        }
        cJSON_Delete(root);
        return err;
    } else {
        snprintf(output, out_len, "Error: 'url' parameter missing");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
}

/* -------------------------------------------------------------------------
 * Tool: audio_stop
 * Input: {}
 * ------------------------------------------------------------------------- */
static esp_err_t tool_audio_stop(const char *input, char *output, size_t out_len)
{
    (void)input;
    audio_manager_stop();
    snprintf(output, out_len, "Audio stopped.");
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Tool: audio_volume
 * Input: {"volume": 50}
 * ------------------------------------------------------------------------- */
static esp_err_t tool_audio_volume(const char *input, char *output, size_t out_len)
{
    cJSON *root = cJSON_Parse(input);
    int vol = -1;
    if (root) {
        cJSON *vol_item = cJSON_GetObjectItem(root, "volume");
        if (cJSON_IsNumber(vol_item)) {
            vol = vol_item->valueint;
        }
        cJSON_Delete(root);
    }

    if (vol >= 0 && vol <= 100) {
        audio_manager_set_volume(vol);
        snprintf(output, out_len, "Volume set to %d", vol);
        return ESP_OK;
    } else {
        snprintf(output, out_len, "Current volume: %d (Usage: {\"volume\": 0-100})", 
                 audio_manager_get_volume());
        return ESP_OK;
    }
}

/* -------------------------------------------------------------------------
 * Registration
 * ------------------------------------------------------------------------- */
void register_audio_tools(void)
{
    static const mimi_tool_t tool_play_url = {
        .name = "audio_play_url",
        .description = "Play audio from a URL. Input: {\"url\": \"https://...\"}. Supports MP3.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"}},\"required\":[\"url\"]}",
        .execute = tool_audio_play_url,
    };
    static const mimi_tool_t tool_stop = {
        .name = "audio_stop",
        .description = "Stop current audio playback.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
        .execute = tool_audio_stop,
    };
    static const mimi_tool_t tool_volume = {
        .name = "audio_volume",
        .description = "Set audio volume. Input: {\"volume\": 0-100}.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"volume\":{\"type\":\"integer\"}},\"required\":[]}",
        .execute = tool_audio_volume,
    };

    tool_registry_register(&tool_play_url);
    tool_registry_register(&tool_stop);
    tool_registry_register(&tool_volume);
    ESP_LOGI(TAG, "Audio tools registered");
}
