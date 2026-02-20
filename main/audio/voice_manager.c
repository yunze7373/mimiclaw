#include "audio/voice_manager.h"
#include "audio.h"
#include "llm/llm_proxy.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

// Forward declarations for ASR/TTS clients to be implemented
esp_err_t asr_recognize(const uint8_t *audio_data, size_t len, char **out_text);
esp_err_t tts_speak(const char *text);

static const char *TAG = "voice_mgr";

static voice_state_t s_current_state = VOICE_STATE_IDLE;
static TaskHandle_t s_voice_task = NULL;

// Helper to set state
static void set_state(voice_state_t new_state) {
    s_current_state = new_state;
    ESP_LOGI(TAG, "Voice State -> %d", new_state);
}

// Memory tracking
static size_t get_internal_sram_free() {
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

// Helper to interact with the LLM via llm_proxy
static esp_err_t proxy_llm_request(const char *user_text, char **out_response) {
    if (!user_text || !out_response) return ESP_ERR_INVALID_ARG;
    
    // Size check for LLM response buffer
    size_t llm_resp_size = 4096;
    char *resp_buf = heap_caps_calloc(1, llm_resp_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resp_buf) {
        ESP_LOGE(TAG, "Failed to allocate LLM response buffer");
        return ESP_ERR_NO_MEM;
    }

    // Convert text into JSON format expected by llm_chat
    cJSON *messages = cJSON_CreateArray();
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(msg, "content", user_text);
    cJSON_AddItemToArray(messages, msg);
    
    char *messages_json = cJSON_PrintUnformatted(messages);
    cJSON_Delete(messages);

    if (!messages_json) {
        free(resp_buf);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Sending text to LLM: %s", user_text);
    esp_err_t err = llm_chat("You are a helpful voice assistant.", messages_json, resp_buf, llm_resp_size);
    free(messages_json);

    if (err == ESP_OK) {
        *out_response = strdup(resp_buf);
    } else {
        ESP_LOGE(TAG, "LLM Chat failed: %s", esp_err_to_name(err));
    }

    free(resp_buf);
    return err;
}

static void voice_task(void *arg) {
    while (1) {
        if (s_current_state == VOICE_STATE_LISTENING) {
            ESP_LOGI(TAG, "Start Listening... (10 seconds timeout)");

            // We will capture 3 seconds of audio to PSRAM
            // Note: 16000 Hz, 16-bit, mono = 32000 bytes/sec
            // 3 seconds = ~96KB
            size_t duration_sec = 3;
            size_t bytes_per_sec = 16000 * 2 * 1; 
            size_t max_capture_size = duration_sec * bytes_per_sec;
            
            uint8_t *audio_buf = heap_caps_malloc(max_capture_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!audio_buf) {
                ESP_LOGE(TAG, "Failed to allocate PSRAM for ASR recording");
                set_state(VOICE_STATE_IDLE);
                continue;
            }

            // Record from I2S mic
            size_t bytes_read = 0;
            size_t total_read = 0;

            // Optional: Start a beep here to indicate listening

            uint64_t start_time = esp_timer_get_time() / 1000ULL;
            while (s_current_state == VOICE_STATE_LISTENING && total_read < max_capture_size) {
                if (audio_i2s_read((char *)audio_buf + total_read, 1024, &bytes_read, pdMS_TO_TICKS(100)) == ESP_OK) {
                    total_read += bytes_read;
                } else {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }

                if (esp_timer_get_time() / 1000ULL - start_time > duration_sec * 1000 + 1000) {
                     break; // Safety timeout
                }
            }
            
            ESP_LOGI(TAG, "Captured %d bytes of audio", total_read);

            if (s_current_state != VOICE_STATE_LISTENING) {
                 // Cancelled
                 free(audio_buf);
                 continue;
            }

            set_state(VOICE_STATE_PROCESSING);

            // 1. ASR
            char *recognized_text = NULL;
            ESP_LOGI(TAG, "Sending to ASR...");
            esp_err_t err = asr_recognize(audio_buf, total_read, &recognized_text);
            free(audio_buf);

            if (err == ESP_OK && recognized_text && strlen(recognized_text) > 0) {
                 ESP_LOGI(TAG, "ASR Result: %s", recognized_text);
                 
                 // 2. LLM
                 char *llm_resp = NULL;
                 err = proxy_llm_request(recognized_text, &llm_resp);

                 if (err == ESP_OK && llm_resp) {
                     ESP_LOGI(TAG, "LLM Response: %s", llm_resp);
                     
                     // 3. TTS
                     set_state(VOICE_STATE_SPEAKING);
                     tts_speak(llm_resp);
                     free(llm_resp);
                 }
            } else {
                ESP_LOGE(TAG, "ASR recognition failed or empty");
            }

            if (recognized_text) free(recognized_text);
            set_state(VOICE_STATE_IDLE);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

esp_err_t voice_manager_init(void) {
    if (s_voice_task) return ESP_OK; // Already initialized

    // Create the task that handles voice processing
    // High stack to handle HTTP requests gracefully
    if (xTaskCreate(voice_task, "voice_mgr", 8192, NULL, 5, &s_voice_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Voice Manager task");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Voice Manager initialized");
    return ESP_OK;
}

esp_err_t voice_manager_start_listening(void) {
    if (s_current_state != VOICE_STATE_IDLE && s_current_state != VOICE_STATE_LISTENING) {
        ESP_LOGW(TAG, "Cannot start listening, current state is %d", s_current_state);
        return ESP_ERR_INVALID_STATE;
    }
    set_state(VOICE_STATE_LISTENING);
    return ESP_OK;
}

esp_err_t voice_manager_stop(void) {
    set_state(VOICE_STATE_IDLE);
    return ESP_OK;
}

voice_state_t voice_manager_get_state(void) {
    return s_current_state;
}
