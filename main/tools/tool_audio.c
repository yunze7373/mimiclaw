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
static void tool_audio_play_url(const char *input, char *output, size_t out_len)
{
    cJSON *root = cJSON_Parse(input);
    if (!root) {
        snprintf(output, out_len, "Error: Invalid JSON");
        return;
    }

    cJSON *url_item = cJSON_GetObjectItem(root, "url");
    if (cJSON_IsString(url_item) && (url_item->valuestring != NULL)) {
        esp_err_t err = audio_manager_play_url(url_item->valuestring);
        if (err == ESP_OK) {
            snprintf(output, out_len, "Started playing: %s", url_item->valuestring);
        } else {
            snprintf(output, out_len, "Failed to start playback (Error %d)", err);
        }
    } else {
        snprintf(output, out_len, "Error: 'url' parameter missing");
    }
    cJSON_Delete(root);
}

/* -------------------------------------------------------------------------
 * Tool: audio_stop
 * Input: {}
 * ------------------------------------------------------------------------- */
static void tool_audio_stop(const char *input, char *output, size_t out_len)
{
    audio_manager_stop();
    snprintf(output, out_len, "Audio stopped.");
}

/* -------------------------------------------------------------------------
 * Tool: audio_volume
 * Input: {"volume": 50}
 * ------------------------------------------------------------------------- */
static void tool_audio_volume(const char *input, char *output, size_t out_len)
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
    } else {
        snprintf(output, out_len, "Current volume: %d (Usage: {\"volume\": 0-100})", 
                 audio_manager_get_volume());
    }
}

/* -------------------------------------------------------------------------
 * Registration
 * ------------------------------------------------------------------------- */
void register_audio_tools(void)
{
    tool_registry_register("audio_play_url", tool_audio_play_url, 
        "Play audio from a URL. Input: {\"url\": \"https://...\"}. Supports MP3.");
        
    tool_registry_register("audio_stop", tool_audio_stop,
        "Stop current audio playback.");
        
    tool_registry_register("audio_volume", tool_audio_volume,
        "Set audio volume. Input: {\"volume\": 0-100}.");
}
