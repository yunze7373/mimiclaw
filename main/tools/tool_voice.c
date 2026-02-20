#include "tools/tool_voice.h"
#include "tools/tool_registry.h"
#include "audio/voice_manager.h"
#include "cJSON.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "tool_voice";

static void tool_voice_start(const char *args_json, char *result_buf, size_t result_size) {
    esp_err_t err = voice_manager_start_listening();
    if (err == ESP_OK) {
        snprintf(result_buf, result_size, "{\"status\": \"listening\"}");
    } else {
        snprintf(result_buf, result_size, "{\"error\": \"%s\"}", esp_err_to_name(err));
    }
}

static void tool_voice_stop(const char *args_json, char *result_buf, size_t result_size) {
    esp_err_t err = voice_manager_stop();
    if (err == ESP_OK) {
        snprintf(result_buf, result_size, "{\"status\": \"stopped\"}");
    } else {
        snprintf(result_buf, result_size, "{\"error\": \"%s\"}", esp_err_to_name(err));
    }
}

static void tool_voice_status(const char *args_json, char *result_buf, size_t result_size) {
    voice_state_t state = voice_manager_get_state();
    const char *state_str = "unknown";
    switch(state) {
        case VOICE_STATE_IDLE: state_str = "idle"; break;
        case VOICE_STATE_LISTENING: state_str = "listening"; break;
        case VOICE_STATE_PROCESSING: state_str = "processing"; break;
        case VOICE_STATE_SPEAKING: state_str = "speaking"; break;
    }
    snprintf(result_buf, result_size, "{\"state\": \"%s\"}", state_str);
}

void register_voice_tools(void) {
    tool_registry_register("voice_start", tool_voice_start, 
        "Start voice assistant listening. Uses microphone. No input required.");
    
    tool_registry_register("voice_stop", tool_voice_stop, 
        "Stop voice assistant listening or speaking. No input required.");
    
    tool_registry_register("voice_status", tool_voice_status, 
        "Get current voice assistant state (idle, listening, processing, speaking).");
    
    ESP_LOGI(TAG, "Voice tools registered");
}
