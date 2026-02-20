#include "audio_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/task.h"
#include "mimi_config.h"

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
#endif // MIMI_HAS_ADF

static bool s_is_playing = false;
static int s_volume = 60;

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
    ESP_LOGW(TAG, "Audio Manager: ESP-ADF not present. Functionality disabled.");
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
#endif
    s_is_playing = false;
}

esp_err_t audio_manager_play_url(const char *url)
{
    if (s_is_playing) {
        _audio_stop_pipeline();
    }

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
    ESP_LOGE(TAG, "Audio Playback failed: ADF missing");
    return ESP_ERR_NOT_SUPPORTED;
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
