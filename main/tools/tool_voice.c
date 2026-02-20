#include "tools/tool_voice.h"
#include "tools/tool_registry.h"
#include "audio/voice_manager.h"
#include "cJSON.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "tool_voice";

static esp_err_t tool_voice_start(const char *args_json, char *result_buf, size_t result_size) {
    (void)args_json;
    esp_err_t err = voice_manager_start_listening();
    if (err == ESP_OK) {
        snprintf(result_buf, result_size, "{\"status\": \"listening\"}");
    } else {
        snprintf(result_buf, result_size, "{\"error\": \"%s\"}", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t tool_voice_stop(const char *args_json, char *result_buf, size_t result_size) {
    (void)args_json;
    esp_err_t err = voice_manager_stop();
    if (err == ESP_OK) {
        snprintf(result_buf, result_size, "{\"status\": \"stopped\"}");
    } else {
        snprintf(result_buf, result_size, "{\"error\": \"%s\"}", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t tool_voice_status(const char *args_json, char *result_buf, size_t result_size) {
    (void)args_json;
    voice_state_t state = voice_manager_get_state();
    const char *state_str = "unknown";
    switch(state) {
        case VOICE_STATE_IDLE: state_str = "idle"; break;
        case VOICE_STATE_LISTENING: state_str = "listening"; break;
        case VOICE_STATE_PROCESSING: state_str = "processing"; break;
        case VOICE_STATE_SPEAKING: state_str = "speaking"; break;
    }
    snprintf(result_buf, result_size, "{\"state\": \"%s\"}", state_str);
    return ESP_OK;
}

void register_voice_tools(void) {
    static const mimi_tool_t tool_start = {
        .name = "voice_start",
        .description = "Start voice assistant listening. Uses microphone. No input required.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
        .execute = tool_voice_start,
    };
    static const mimi_tool_t tool_stop = {
        .name = "voice_stop",
        .description = "Stop voice assistant listening or speaking. No input required.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
        .execute = tool_voice_stop,
    };
    static const mimi_tool_t tool_status = {
        .name = "voice_status",
        .description = "Get current voice assistant state (idle, listening, processing, speaking) and VAD status.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
        .execute = tool_voice_status,
    };
    static const mimi_tool_t tool_vad_en = {
        .name = "voice_vad_enable",
        .description = "Enable Hands-free Voice Activity Detection (VAD). Will automatically start listening when loud noise is detected.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
        .execute = tool_vad_enable,
    };
    static const mimi_tool_t tool_vad_dis = {
        .name = "voice_vad_disable",
        .description = "Disable Hands-free Voice Activity Detection (VAD).",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
        .execute = tool_vad_disable,
    };

    tool_registry_register(&tool_start);
    tool_registry_register(&tool_stop);
    tool_registry_register(&tool_status);
    tool_registry_register(&tool_vad_en);
    tool_registry_register(&tool_vad_dis);
    
    ESP_LOGI(TAG, "Voice tools registered");
}
