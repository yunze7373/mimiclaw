#include "tools/tool_registry.h"
#include "audio/audio_manager.h"
#include "cJSON.h"
#include "esp_log.h"
#include <string.h>
#include <math.h>

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
 * Tool: audio_test
 * Input: {"freq": 440, "duration_ms": 1000}
 * ------------------------------------------------------------------------- */
static esp_err_t tool_audio_test(const char *input, char *output, size_t out_len)
{
    cJSON *root = cJSON_Parse(input);
    int freq = 440;
    int duration_ms = 1000;
    
    if (root) {
        cJSON *f = cJSON_GetObjectItem(root, "freq");
        cJSON *d = cJSON_GetObjectItem(root, "duration_ms");
        if (cJSON_IsNumber(f)) freq = f->valueint;
        if (cJSON_IsNumber(d)) duration_ms = d->valueint;
        cJSON_Delete(root);
    }
    
    extern esp_err_t audio_init(void);
    extern esp_err_t audio_speaker_start(void);
    extern esp_err_t audio_speaker_write(const uint8_t *data, size_t len);
    
    audio_init();
    audio_speaker_start();
    
    // Generate a simple sine wave at 44100Hz 16-bit
    int sample_rate = 44100;
    int num_samples = (sample_rate * duration_ms) / 1000;
    int buf_samples = 1024;
    int16_t *buf = malloc(buf_samples * sizeof(int16_t) * 2); // stereo buffer for MAX98357a
    if (!buf) {
        snprintf(output, out_len, "Error: OOM for test tone");
        return ESP_FAIL;
    }
    
    float phase = 0.0f;
    float phase_inc = (2.0f * 3.14159265f * freq) / sample_rate;
    int samples_played = 0;
    
    ESP_LOGI(TAG, "Playing %dHz test tone for %dms", freq, duration_ms);
    
    while (samples_played < num_samples) {
        int to_play = (num_samples - samples_played > buf_samples) ? buf_samples : (num_samples - samples_played);
        for (int i = 0; i < to_play; i++) {
            // Sine wave amplitude scaled down to avoid clipping/loudness max
            int16_t val = (int16_t)(10000.0f * sinf(phase));
            buf[i*2] = val;     // Left channel
            buf[i*2 + 1] = val; // Right channel duplicated
            phase += phase_inc;
            if (phase >= 2.0f * 3.14159265f) phase -= 2.0f * 3.14159265f;
        }
        audio_speaker_write((uint8_t*)buf, to_play * sizeof(int16_t) * 2);
        samples_played += to_play;
    }
    
    free(buf);
    snprintf(output, out_len, "Test tone played (%d Hz, %d ms)", freq, duration_ms);
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Tool: audio_test_mic
 * Input: {}
 * Description: Reads 500ms of audio and reports volume/RMS to verify mic hardware
 * ------------------------------------------------------------------------- */
static esp_err_t tool_audio_test_mic(const char *input, char *output, size_t out_len)
{
    (void)input; // Ignore input
    extern esp_err_t audio_init(void);
    extern esp_err_t audio_mic_start(void);
    extern esp_err_t audio_mic_stop(void);
    extern int audio_mic_read(uint8_t *buffer, size_t len);

    audio_init();
    esp_err_t err = audio_mic_start();
    if (err != ESP_OK) {
        snprintf(output, out_len, "Error: Failed to start microphone hardware.");
        return ESP_FAIL;
    }

    // Read 500ms of audio (16kHz, 16-bit, 1 channel = 32000 bytes/sec -> 16000 bytes)
    size_t req_bytes = 16000;
    int16_t *buf = malloc(req_bytes);
    if (!buf) {
        snprintf(output, out_len, "Error: OOM allocating mic buffer.");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Recording 500ms of audio for diagnostic test...");
    
    // Discard the first chunk to avoid startup clicks/pops
    audio_mic_read((uint8_t*)buf, 4096);

    int total_read = 0;
    int max_amp = 0;
    int64_t sum_squares = 0;

    while (total_read < req_bytes) {
        int chunk = (req_bytes - total_read > 4096) ? 4096 : (req_bytes - total_read);
        int bytes_read = audio_mic_read(((uint8_t*)buf) + total_read, chunk);
        if (bytes_read <= 0) break;
        total_read += bytes_read;
    }

    audio_mic_stop();

    if (total_read == 0) {
        free(buf);
        snprintf(output, out_len, "Error: Microphone hardware returned 0 bytes.");
        return ESP_FAIL;
    }

    int samples = total_read / 2;
    int zero_samples = 0;

    for (int i = 0; i < samples; i++) {
        int16_t val = buf[i];
        if (val == 0) zero_samples++;
        int amp = abs(val);
        if (amp > max_amp) max_amp = amp;
        sum_squares += (int64_t)val * val;
    }

    free(buf);

    int rms = (int)sqrt(sum_squares / samples);

    ESP_LOGI(TAG, "Mic Test: Samples=%d, Max Amp=%d, RMS=%d, Zeros=%d", samples, max_amp, rms, zero_samples);

    if (max_amp == 0) {
        snprintf(output, out_len, "Mic Error: Hardware connected but recording complete silence (Max=0, RMS=0). Check MIC wiring (SD/WS/SCK) and power.");
    } else if (zero_samples > samples * 0.9) {
        snprintf(output, out_len, "Mic Error: Over 90%% of samples are zero. Audio format or clock issue likely.");
    } else if (rms < 50) {
        snprintf(output, out_len, "Mic Warning: Succeeded, but audio is VERY quiet (RMS=%d, Max=%d). Is the room silent? Wait for a louder sound and try again.", rms, max_amp);
    } else {
        snprintf(output, out_len, "Mic Success: Hardware works! Captured %d samples. RMS Volume=%d, Max Amplitude=%d. Sounds normal.", samples, rms, max_amp);
    }

    return ESP_OK;
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
    static const mimi_tool_t tool_test = {
        .name = "audio_test_tone",
        .description = "Play a pure sine wave test tone to debug speaker hardware. Input: {\"freq\": 440, \"duration_ms\": 1000}",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"freq\":{\"type\":\"integer\"},\"duration_ms\":{\"type\":\"integer\"}},\"required\":[]}",
        .execute = tool_audio_test,
    };
    static const mimi_tool_t tool_test_mic = {
        .name = "audio_test_mic",
        .description = "Read 500ms of audio from the microphone to calculate volume levels and verify hardware.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
        .execute = tool_audio_test_mic,
    };

    tool_registry_register(&tool_play_url);
    tool_registry_register(&tool_stop);
    tool_registry_register(&tool_volume);
    tool_registry_register(&tool_test);
    tool_registry_register(&tool_test_mic);
    ESP_LOGI(TAG, "Audio tools registered");
}
