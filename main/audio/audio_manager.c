#include "audio_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mimi_config.h"
#include "esp_heap_caps.h"
#include <ctype.h>

// Check if ADF is available
#if __has_include("audio_pipeline.h")
#define MIMI_HAS_ADF 1
// ADF Includes
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "http_stream.h"
#include "i2s_stream.h"
#include "mp3_decoder.h"
#include "aac_decoder.h"
#include "wav_decoder.h"
#else
#define MIMI_HAS_ADF 0
#warning "ESP-ADF not found. Audio Manager will be stubbed."
#endif

static const char *TAG = "audio_mgr";

#if MIMI_HAS_ADF
static audio_pipeline_handle_t s_pipeline = NULL;
static audio_element_handle_t s_http_stream_reader = NULL;
static audio_element_handle_t s_i2s_stream_writer = NULL;
static audio_element_handle_t s_mp3_decoder = NULL;
static audio_element_handle_t s_aac_decoder = NULL;
static audio_element_handle_t s_wav_decoder = NULL;
static audio_event_iface_handle_t s_evt = NULL;
#else
#include "audio.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

// MINIMP3_IMPLEMENTATION must be defined in exactly one C file
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include <string.h>

static TaskHandle_t s_mp3_task = NULL;
static volatile bool s_mp3_stop = false;
static char *s_current_url = NULL;
#endif // MIMI_HAS_ADF

static bool s_is_playing = false;
static int s_volume = 60;

#if !MIMI_HAS_ADF
static bool str_contains_ci(const char *haystack, const char *needle)
{
    if (!haystack || !needle || !needle[0]) return false;
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nlen) return true;
    }
    return false;
}

static bool url_seems_mp3(const char *url)
{
    return str_contains_ci(url, ".mp3");
}
#endif

#if MIMI_HAS_ADF
// Internal: Create I2S Stream Writer (Speaker)
static audio_element_handle_t create_i2s_writer(void)
{
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    // Use I2S_NUM_0 or I2S_NUM_1 based on config. MIMI uses I2S1 for Speaker
    i2s_cfg.i2s_config.mode = I2S_MODE_MASTER | I2S_MODE_TX;
    i2s_cfg.i2s_config.sample_rate = 44100;
    i2s_cfg.i2s_config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    i2s_cfg.i2s_config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    i2s_cfg.i2s_port = I2S_NUM_1; 
    i2s_cfg.use_alc = true;
    i2s_cfg.volume = s_volume;
    
    return i2s_stream_init(&i2s_cfg);
}

// Internal: Create MP3 Decoder
static audio_element_handle_t create_mp3_decoder(void)
{
    mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
    return mp3_decoder_init(&mp3_cfg);
}
#endif

#if !MIMI_HAS_ADF
static bool wait_mp3_task_exit(uint32_t timeout_ms)
{
    uint32_t waited = 0;
    while (s_mp3_task != NULL && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited += 20;
    }
    return (s_mp3_task == NULL);
}

static void mp3_player_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Native MP3 player task started");
    s_is_playing = true;

    char *url_snapshot = NULL;
    if (s_current_url) {
        url_snapshot = strdup(s_current_url);
    }
    if (!url_snapshot) {
        ESP_LOGE(TAG, "No URL to play");
        goto cleanup;
    }
    
    esp_http_client_config_t config = {
        .url = url_snapshot,
        .buffer_size = 4096,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        goto cleanup;
    }

    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= 3; attempt++) {
        err = esp_http_client_open(client, 0);
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "HTTP open attempt %d/3 failed: %s", attempt, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(500 * attempt));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection after retries: %s", esp_err_to_name(err));
        goto cleanup_client;
    }
    
    int content_length = esp_http_client_fetch_headers(client);
    ESP_LOGI(TAG, "HTTP stream opened, length: %d", content_length);

    mp3dec_t *mp3d = calloc(1, sizeof(mp3dec_t));
    if (!mp3d) {
        ESP_LOGE(TAG, "Failed to allocate MP3 decoder");
        goto cleanup_client;
    }
    mp3dec_init(mp3d);
    
    #define MP3_BUF_SIZE 16384
    uint8_t *in_buf = malloc(MP3_BUF_SIZE);
    int16_t *pcm_raw = malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t));
    int16_t *pcm_mono = malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t));
    
    if (!in_buf || !pcm_raw || !pcm_mono) {
        ESP_LOGE(TAG, "Failed to allocate MP3 buffers");
        if(mp3d) free(mp3d);
        if(in_buf) free(in_buf);
        if(pcm_raw) free(pcm_raw);
        if(pcm_mono) free(pcm_mono);
        goto cleanup_client;
    }

    int bytes_in_buf = 0;
    bool eof = false;
    bool rate_set = false;
    mp3dec_frame_info_t info;

    while (!s_mp3_stop && !eof) {
        // Read more data if buffer is less than half full
        if (bytes_in_buf < MP3_BUF_SIZE / 2) {
            int to_read = MP3_BUF_SIZE - bytes_in_buf;
            int read_len = esp_http_client_read(client, (char*)in_buf + bytes_in_buf, to_read);
            if (read_len < 0) {
                ESP_LOGE(TAG, "HTTP read error");
                break;
            } else if (read_len == 0) {
                if (esp_http_client_is_complete_data_received(client)) {
                    eof = true;
                }
            } else {
                bytes_in_buf += read_len;
            }
        }

        if (bytes_in_buf == 0 && eof) break; 

        int samples = mp3dec_decode_frame(mp3d, in_buf, bytes_in_buf, pcm_raw, &info);
        
        if (info.frame_bytes > 0 && info.frame_bytes <= bytes_in_buf) {
            bytes_in_buf -= info.frame_bytes;
            memmove(in_buf, in_buf + info.frame_bytes, bytes_in_buf);
        } else if (info.frame_bytes == 0) {
            if (eof) break;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (samples > 0) {
            if (!rate_set) {
                ESP_LOGI(TAG, "MP3 format: %d Hz, %d channels", info.hz, info.channels);
                audio_set_sample_rate(info.hz);
                rate_set = true;
            }

            int mono_samples = samples;
            const int max_samples = MINIMP3_MAX_SAMPLES_PER_FRAME;
            if (mono_samples > max_samples) mono_samples = max_samples;

            if (info.channels == 2) {
                /* minimp3 sample count can vary by integration; keep strictly bounded */
                if (mono_samples > max_samples / 2) {
                    mono_samples = mono_samples / 2;
                }
                for (int i = 0; i < mono_samples; i++) {
                    int idx = i * 2;
                    pcm_mono[i] = (int16_t)(((int)pcm_raw[idx] + (int)pcm_raw[idx + 1]) / 2);
                }
            } else if (info.channels == 1) {
                memcpy(pcm_mono, pcm_raw, (size_t)mono_samples * sizeof(int16_t));
            } else {
                ESP_LOGW(TAG, "Unsupported channel count: %d", info.channels);
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }

            // Ensure speaker is started before writing
            extern esp_err_t audio_speaker_start(void);
            audio_speaker_start();

            esp_err_t wr_err = audio_speaker_write((uint8_t*)pcm_mono, (size_t)mono_samples * sizeof(int16_t));
            if (wr_err != ESP_OK) {
                ESP_LOGW(TAG, "Speaker write error: %s", esp_err_to_name(wr_err));
            }
        }
        // Yield enough time for Wi-Fi and LwIP to process packets to avoid connection drops
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    if(mp3d) free(mp3d);
    free(in_buf);
    free(pcm_raw);
    free(pcm_mono);

cleanup_client:
    esp_http_client_cleanup(client);
    
cleanup:
    ESP_LOGI(TAG, "MP3 player task finished");
    if (url_snapshot) free(url_snapshot);
    s_is_playing = false;
    s_mp3_task = NULL;
    vTaskDelete(NULL);
}
#endif // !MIMI_HAS_ADF

esp_err_t audio_manager_init(void)
{
#if MIMI_HAS_ADF
    ESP_LOGI(TAG, "Initializing Audio Manager (ESP-ADF)...");
    
    esp_log_level_set("AUDIO_PIPELINE", ESP_LOG_WARN);
    esp_log_level_set("AUDIO_ELEMENT", ESP_LOG_WARN);

    // Create Pipeline
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    s_pipeline = audio_pipeline_init(&pipeline_cfg);
    if (!s_pipeline) {
        ESP_LOGE(TAG, "Failed to init audio pipeline");
        return ESP_FAIL;
    }

    // Create I2S Writer
    s_i2s_stream_writer = create_i2s_writer();
    if (s_i2s_stream_writer) {
        // Manually configure pins for I2S1 (Speaker)
        i2s_pin_config_t pin_config = {
            .bck_io_num = MIMI_PIN_I2S1_BCLK,
            .ws_io_num = MIMI_PIN_I2S1_LRC,
            .data_out_num = MIMI_PIN_I2S1_DIN,
            .data_in_num = -1                                                       
        };
        i2s_set_pin(I2S_NUM_1, &pin_config);
        
        audio_pipeline_register(s_pipeline, s_i2s_stream_writer, "i2s");
    }

    // Create MP3 Decoder
    s_mp3_decoder = create_mp3_decoder();
    if (s_mp3_decoder) {
        audio_pipeline_register(s_pipeline, s_mp3_decoder, "mp3");
    }
    
    // Create HTTP Stream Reader
    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    s_http_stream_reader = http_stream_init(&http_cfg);
    if (s_http_stream_reader) {
        audio_pipeline_register(s_pipeline, s_http_stream_reader, "http");
    }

    // Setup Event Listener
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    s_evt = audio_event_iface_init(&evt_cfg);
    audio_pipeline_set_listener(s_pipeline, s_evt);
#else
    ESP_LOGI(TAG, "Audio Manager: Native MP3 streaming enabled via minimp3.");
#endif
    return ESP_OK;
}

static void _audio_stop_pipeline(void)
{
#if MIMI_HAS_ADF
    audio_pipeline_stop(s_pipeline);
    audio_pipeline_wait_for_stop(s_pipeline);
    audio_pipeline_terminate(s_pipeline);
    audio_pipeline_unlink(s_pipeline);
#else
    if (s_mp3_task != NULL) {
        s_mp3_stop = true;
        // The background task will stop on the next iteration and clear playing state.
    }
#endif
    s_is_playing = false;
}

esp_err_t audio_manager_play_url(const char *url)
{
    // Ensure previous playback is stopped cleanly
    if (s_is_playing) {
        _audio_stop_pipeline();
        // Give time for previous task to fully exit
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // Ensure speaker is started before creating playback task
    // audio_speaker_start is idempotent - safe to call multiple times
    extern esp_err_t audio_speaker_start(void);
    audio_speaker_start();

    ESP_LOGI(TAG, "Playing URL: %s", url);
    
#if MIMI_HAS_ADF
    // Set URL
    audio_element_set_uri(s_http_stream_reader, url);

    // Link: HTTP -> MP3 -> I2S
    // Note: Assuming MP3 for now. Auto-detection requires more logic.
    const char *link_tag[3] = {"http", "mp3", "i2s"};
    audio_pipeline_link(s_pipeline, link_tag, 3);
    
    audio_pipeline_run(s_pipeline);
    s_is_playing = true;
    return ESP_OK;
#else
    if (!url || !url_seems_mp3(url)) {
        ESP_LOGE(TAG, "Only MP3 URLs are supported in native mode: %s", url ? url : "(null)");
        return ESP_ERR_NOT_SUPPORTED;
    }

    // Stop old playback task first and wait for a clean handover.
    if (s_mp3_task != NULL) {
        s_mp3_stop = true;
        if (!wait_mp3_task_exit(3000)) {
            ESP_LOGE(TAG, "Previous MP3 task did not exit in time");
            return ESP_ERR_TIMEOUT;
        }
    }

    s_mp3_stop = false;
    if (s_current_url) {
        free(s_current_url);
        s_current_url = NULL;
    }
    s_current_url = strdup(url);
    if (!s_current_url) {
        ESP_LOGE(TAG, "Failed to allocate URL");
        return ESP_ERR_NO_MEM;
    }

    // Lower priority to 3 so it doesn't starve the LwIP/Wi-Fi stack
    if (xTaskCreate(mp3_player_task, "mp3_player", 16384, NULL, 3, &s_mp3_task) == pdPASS) {
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Failed to create mp3_player task");
        free(s_current_url);
        s_current_url = NULL;
        return ESP_FAIL;
    }
#endif
}

esp_err_t audio_manager_play_file(const char *path)
{
    // TODO: Implement file playback (fatfs_stream or spiffs_stream)
    ESP_LOGW(TAG, "File playback not yet implemented");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_manager_stop(void)
{
    if (s_is_playing) {
        ESP_LOGI(TAG, "Stopping playback");
        _audio_stop_pipeline();
#if !MIMI_HAS_ADF
        if (!wait_mp3_task_exit(3000)) {
            ESP_LOGW(TAG, "MP3 task still running after stop timeout");
        }
#endif
    }
    return ESP_OK;
}

esp_err_t audio_manager_pause(void)
{
#if MIMI_HAS_ADF
    audio_pipeline_pause(s_pipeline);
#endif
    return ESP_OK;
}

esp_err_t audio_manager_resume(void)
{
#if MIMI_HAS_ADF
    audio_pipeline_resume(s_pipeline);
#endif
    return ESP_OK;
}

esp_err_t audio_manager_set_volume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    s_volume = volume;
    
#if MIMI_HAS_ADF
    if (s_i2s_stream_writer) {
        i2s_stream_set_volume(s_i2s_stream_writer, 0, volume);
    }
#endif
    ESP_LOGI(TAG, "Volume set to %d", volume);
    return ESP_OK;
}

int audio_manager_get_volume(void)
{
    return s_volume;
}

bool audio_manager_is_playing(void)
{
    return s_is_playing;
}
