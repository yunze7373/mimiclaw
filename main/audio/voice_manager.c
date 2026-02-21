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

#include <math.h>

// Configurable VAD params
#define VAD_ENERGY_THRESHOLD 3000  // Normal talking is ~1000-5000 RMS
#define VAD_DURATION_MS 300 // Duration of loud noise to trigger wake

static const char *TAG = "voice_mgr";

static voice_state_t s_current_state = VOICE_STATE_IDLE;
static TaskHandle_t s_voice_task = NULL;
static TaskHandle_t s_vad_task = NULL;
static bool s_vad_enabled = false;

// Helper to set state
static void set_state(voice_state_t new_state) {
    s_current_state = new_state;
    ESP_LOGI(TAG, "Voice State -> %d", new_state);
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
            size_t total_read = 0;

            // Optional: Start a beep here to indicate listening

            uint64_t start_time = esp_timer_get_time() / 1000ULL;
            audio_mic_start();
            while (s_current_state == VOICE_STATE_LISTENING && total_read < max_capture_size) {
                int chunk_read = audio_mic_read(audio_buf + total_read, 1024);
                if (chunk_read > 0) {
                    total_read += (size_t)chunk_read;
                } else {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }

                if (esp_timer_get_time() / 1000ULL - start_time > duration_sec * 1000 + 1000) {
                     break; // Safety timeout
                }
            }
            audio_mic_stop();
            
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

static void vad_task(void *arg) {
    ESP_LOGI(TAG, "VAD background task started");
    const int chunk_size = 1024;
    int16_t *buf = heap_caps_malloc(chunk_size, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate VAD buffer");
        vTaskDelete(NULL);
    }

    uint32_t active_ticks = 0;
    const uint32_t required_ticks = VAD_DURATION_MS / 10; // Assuming ~10ms per loop
    bool last_vad_enabled = false;

    while (1) {
        if (!s_vad_enabled || s_current_state != VOICE_STATE_IDLE) {
            if (last_vad_enabled && !s_vad_enabled && s_current_state == VOICE_STATE_IDLE) {
                audio_mic_stop();
            }
            last_vad_enabled = s_vad_enabled;
            vTaskDelay(pdMS_TO_TICKS(100));
            active_ticks = 0;
            continue;
        }
        last_vad_enabled = s_vad_enabled;

        audio_mic_start();
        // Try to read a small chunk from mic
        int read_bytes = audio_mic_read((uint8_t*)buf, chunk_size);
        if (read_bytes > 0) {
            int samples = read_bytes / 2;
            int64_t sum_squares = 0;
            for (int i=0; i < samples; i++) {
                sum_squares += buf[i] * buf[i];
            }
            int32_t rms = (int32_t)sqrt(sum_squares / samples);

            if (rms > VAD_ENERGY_THRESHOLD) {
                active_ticks++;
                if (active_ticks >= required_ticks) {
                    ESP_LOGI(TAG, "VAD Triggered! (RMS: %ld > %d)", (long)rms, VAD_ENERGY_THRESHOLD);
                    active_ticks = 0;
                    voice_manager_start_listening();
                }
            } else {
                active_ticks = 0; // reset if quiet
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
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
    
    if (xTaskCreate(vad_task, "vad_mgr", 4096, NULL, 4, &s_vad_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create VAD task");
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

esp_err_t voice_vad_enable(bool enable) {
    s_vad_enabled = enable;
    if (enable && s_current_state == VOICE_STATE_IDLE) {
       ESP_LOGW(TAG, "Please talk loudly into the microphone when VAD is enabled!");
    }
    ESP_LOGI(TAG, "VAD %s", s_vad_enabled ? "enabled" : "disabled");
    return ESP_OK;
}
