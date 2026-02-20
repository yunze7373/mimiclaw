#include "audio/tts_client.h"
#include "audio/audio.h" // Legacy I2S driver for direct PCM writes
#include "llm/llm_proxy.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "tts_client";

// Expose these from llm_proxy
extern char s_openai_api_key_audio[];
extern char s_tts_endpoint[];

// Helper to handle the audio stream coming back from OpenAI
// We request PCM, 24kHz or 16k, so we can just blast it into i2s_write
static esp_err_t _http_event_handle_tts(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (evt->data_len > 0) {
                // OpenAI returns raw PCM or WAV if we select it
                // We just stream evt->data directly to the I2S speaker buffer
                
                // For a robust implementation, we might want to skip the 44-byte WAV header
                // But as a quick MVP we can push it all.
                
                // I2S write block
                audio_speaker_write((const uint8_t *)evt->data, (size_t)evt->data_len);
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

esp_err_t tts_speak(const char *text) {
    if (!text || strlen(text) == 0) return ESP_ERR_INVALID_ARG;
    if (strlen(s_openai_api_key_audio) == 0) {
        ESP_LOGE(TAG, "Audio API key not configured");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Sending text to TTS: %.50s...", text);

    esp_http_client_config_t config = {
        .url = s_tts_endpoint,
        .timeout_ms = 30000,
        .method = HTTP_METHOD_POST,
        .event_handler = _http_event_handle_tts
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", s_openai_api_key_audio);

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth_header);

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", "tts-1");
    cJSON_AddStringToObject(body, "input", text);
    cJSON_AddStringToObject(body, "voice", "alloy");
    // Request raw PCM to avoid having to decode MP3
    // OpenAI supports: mp3, opus, aac, flac, wav, pcm
    cJSON_AddStringToObject(body, "response_format", "pcm");
    
    char *post_data = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    
    if (!post_data) {
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "TTS HTTP Status = %d", status_code);
        if (status_code != 200) {
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "TTS HTTP Perform failed: %s", esp_err_to_name(err));
    }

    free(post_data);
    esp_http_client_cleanup(client);
    return err;
}
