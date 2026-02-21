#include "audio/asr_client.h"
#include "llm/llm_proxy.h"   // To get config from getters if exposed, or we can just extern them
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "asr_client";

// Use getters from llm_proxy.h instead of extern arrays

esp_err_t asr_recognize(const uint8_t *audio_data, size_t len, char **out_text) {
    const char *api_key = llm_get_openai_api_key_audio();
    const char *endpoint = llm_get_asr_endpoint();

    if (!audio_data || len == 0 || !out_text) return ESP_ERR_INVALID_ARG;
    if (!endpoint || strlen(endpoint) == 0) {
        ESP_LOGE(TAG, "ASR endpoint not configured");
        return ESP_ERR_INVALID_STATE;
    }
    if (!endpoint || strlen(endpoint) == 0) {
        ESP_LOGE(TAG, "ASR endpoint not configured");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Sending %d bytes of audio to ASR endpoint: %s", len, endpoint);

    // Build standard HTTP client request for multipart/form-data
    esp_http_client_config_t config = {
        .url = endpoint,
        .timeout_ms = 30000,
        .method = HTTP_METHOD_POST
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    // Boundary for multipart/form-data
    const char *boundary = "----Esp32ClawBoundary123456";
    char content_type[128];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);

    esp_http_client_set_header(client, "Content-Type", content_type);
    if (api_key && strlen(api_key) > 0) {
        char auth_header[256];
        snprintf(auth_header, sizeof(auth_header), "Bearer %s", api_key);
        esp_http_client_set_header(client, "Authorization", auth_header);
    }

    // Build the payload
    // Part 1: model
    const char *part1 = 
        "------Esp32ClawBoundary123456\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
        "whisper-1\r\n";
    
    // Part 2: file header
    const char *part2 = 
        "------Esp32ClawBoundary123456\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n";
    
    // Part 3: Footer
    const char *part3 = "\r\n------Esp32ClawBoundary123456--\r\n";

    // Create a valid WAV header for the audio_data (assuming 16k 16-bit mono)
    // We just send raw PCM wrapped in a basic WAV header since OpenAI needs .wav format
    uint8_t wav_header[44] = {
        'R', 'I', 'F', 'F', // ChunkID
        0, 0, 0, 0,         // ChunkSize (len + 36)
        'W', 'A', 'V', 'E', // Format
        'f', 'm', 't', ' ', // Subchunk1ID
        16, 0, 0, 0,        // Subchunk1Size (16 for PCM)
        1, 0,               // AudioFormat (1 for PCM)
        1, 0,               // NumChannels (1 mono)
        0x80, 0x3E, 0x00, 0x00, // SampleRate (16000)
        0x00, 0x7D, 0x00, 0x00, // ByteRate (16000 * 1 * 2 = 32000)
        2, 0,               // BlockAlign (1 * 2)
        16, 0,              // BitsPerSample (16)
        'd', 'a', 't', 'a', // Subchunk2ID
        0, 0, 0, 0          // Subchunk2Size (len)
    };
    uint32_t chunk_size = len + 36;
    wav_header[4] = chunk_size & 0xFF;
    wav_header[5] = (chunk_size >> 8) & 0xFF;
    wav_header[6] = (chunk_size >> 16) & 0xFF;
    wav_header[7] = (chunk_size >> 24) & 0xFF;

    wav_header[40] = len & 0xFF;
    wav_header[41] = (len >> 8) & 0xFF;
    wav_header[42] = (len >> 16) & 0xFF;
    wav_header[43] = (len >> 24) & 0xFF;

    int total_len = strlen(part1) + strlen(part2) + sizeof(wav_header) + len + strlen(part3);

    // Because this is chunked or we can just send it manually, let's open stream
    esp_http_client_open(client, total_len);
    esp_http_client_write(client, part1, strlen(part1));
    esp_http_client_write(client, part2, strlen(part2));
    esp_http_client_write(client, (const char *)wav_header, sizeof(wav_header));
    
    // Write audio payload in chunks to avoid watchdog
    size_t written = 0;
    while (written < len) {
        size_t to_write = (len - written > 2048) ? 2048 : len - written;
        esp_http_client_write(client, (const char *)audio_data + written, to_write);
        written += to_write;
    }

    esp_http_client_write(client, part3, strlen(part3));
    
    int content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) {
        ESP_LOGE(TAG, "HTTP client fetch headers failed");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200) {
        ESP_LOGE(TAG, "ASR HTTP Status %d", status_code);
        // Print body for debug
        char err_buf[256] = {0};
        esp_http_client_read(client, err_buf, sizeof(err_buf) - 1);
        ESP_LOGE(TAG, "ASR Error: %s", err_buf);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    char *resp_buf = malloc(4096);
    if (!resp_buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }
    
    int read_len = esp_http_client_read(client, resp_buf, 4095);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (read_len > 0) {
        resp_buf[read_len] = '\0';
        cJSON *root = cJSON_Parse(resp_buf);
        if (root) {
            cJSON *text = cJSON_GetObjectItem(root, "text");
            if (text && cJSON_IsString(text)) {
                *out_text = strdup(text->valuestring);
            }
            cJSON_Delete(root);
        }
    }
    free(resp_buf);

    if (*out_text) return ESP_OK;
    return ESP_FAIL;
}
