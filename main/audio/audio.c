#include "audio.h"
#include "../mimi_config.h"

#include <string.h>
#include "esp_log.h"
#include "driver/i2s.h"
#include "driver/gpio.h"

static const char *TAG = "audio";

static bool s_mic_started = false;
static bool s_speaker_started = false;

/* ── I2S Microphone (INMP441) + Speaker (MAX98357) ───────────── */

static bool s_i2s_installed = false;

esp_err_t audio_init(void)
{
    // I2S configuration for BOTH microphone and speaker
    // Using legacy API for compatibility - works in ESP-IDF 5.x
    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX,
        .sample_rate = AUDIO_SAMPLE_RATE,
        .bits_per_sample = AUDIO_BITS_PER_SAMPLE,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0,
    };

    i2s_pin_config_t i2s_pins = {
        .bck_io_num = AUDIO_I2S_SCK_PIN,
        .ws_io_num = AUDIO_I2S_WS_PIN,
        .data_out_num = AUDIO_I2S_DOUT_PIN,
        .data_in_num = AUDIO_I2S_SD_PIN,
    };

    // Install and start I2S
    esp_err_t ret = i2s_driver_install(AUDIO_I2S_PORT, &i2s_config, 0, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_driver_install failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = i2s_set_pin(AUDIO_I2S_PORT, &i2s_pins);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_set_pin failed: %s", esp_err_to_name(ret));
        return ret;
    }
    i2s_zero_dma_buffer(AUDIO_I2S_PORT);
    s_i2s_installed = true;

    ESP_LOGI(TAG, "Audio initialized (I2S=%d, Rate=%d, Bits=%d)",
             AUDIO_I2S_PORT, AUDIO_SAMPLE_RATE, AUDIO_BITS_PER_SAMPLE);

    return ESP_OK;
}

esp_err_t audio_mic_start(void)
{
    if (s_mic_started) {
        return ESP_OK;
    }
    if (!s_i2s_installed) {
        ESP_LOGE(TAG, "I2S not installed, cannot start mic");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = i2s_start(AUDIO_I2S_PORT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_start failed: %s", esp_err_to_name(ret));
        return ret;
    }
    s_mic_started = true;
    ESP_LOGI(TAG, "Microphone started");
    return ESP_OK;
}

esp_err_t audio_mic_stop(void)
{
    if (!s_mic_started) {
        return ESP_OK;
    }

    i2s_stop(AUDIO_I2S_PORT);
    s_mic_started = false;
    ESP_LOGI(TAG, "Microphone stopped");
    return ESP_OK;
}

int audio_mic_read(uint8_t *buffer, size_t len)
{
    if (!s_mic_started) {
        return 0;
    }

    size_t bytes_read = 0;
    esp_err_t ret = i2s_read(AUDIO_I2S_PORT, buffer, len, &bytes_read, portMAX_DELAY);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S read error: %s", esp_err_to_name(ret));
        return 0;
    }

    return bytes_read;
}

esp_err_t audio_speaker_start(void)
{
    if (s_speaker_started) {
        return ESP_OK;
    }
    if (!s_i2s_installed) {
        ESP_LOGE(TAG, "I2S not installed, cannot start speaker");
        return ESP_ERR_INVALID_STATE;
    }

    s_speaker_started = true;
    ESP_LOGI(TAG, "Speaker started");
    return ESP_OK;
}

esp_err_t audio_speaker_stop(void)
{
    if (!s_speaker_started) {
        return ESP_OK;
    }

    s_speaker_started = false;
    ESP_LOGI(TAG, "Speaker stopped");
    return ESP_OK;
}

esp_err_t audio_set_sample_rate(uint32_t rate)
{
    if (!s_i2s_installed) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = i2s_set_sample_rates(AUDIO_I2S_PORT, rate);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "I2S sample rate set to %lu Hz", (unsigned long)rate);
    } else {
        ESP_LOGE(TAG, "Failed to set sample rate: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t audio_speaker_write(const uint8_t *data, size_t len)
{
    if (!s_speaker_started || !s_i2s_installed) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t bytes_written = 0;
    esp_err_t ret = i2s_write(AUDIO_I2S_PORT, data, len, &bytes_written, portMAX_DELAY);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S write error: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t audio_get_info(char *output, size_t output_size)
{
    snprintf(output, output_size,
        "{\"mic\":{\"started\":%s,\"sample_rate\":%d,\"bits\":%d,\"i2s_port\":%d},"
        "\"speaker\":{\"started\":%s}}",
        s_mic_started ? "true" : "false",
        AUDIO_SAMPLE_RATE,
        AUDIO_BITS_PER_SAMPLE,
        AUDIO_I2S_PORT,
        s_speaker_started ? "true" : "false");

    return ESP_OK;
}

void audio_test_pin(int gpio)
{
    if (!s_i2s_installed) return;
    
    // Switch data out pin
    i2s_pin_config_t pins = {
        .bck_io_num = AUDIO_I2S_SCK_PIN,
        .ws_io_num = AUDIO_I2S_WS_PIN,
        .data_out_num = gpio,
        .data_in_num = AUDIO_I2S_SD_PIN
    };
    i2s_set_pin(AUDIO_I2S_PORT, &pins);
    
    // Play tone (400Hz square wave)
    size_t len = 24000 * 2; // 1 second, 16-bit mono
    int16_t *buf = malloc(len);
    if (!buf) return;
    
    for (int i = 0; i < len / 2; i++) {
        // 24000Hz / 400Hz = 60 samples per cycle
        buf[i] = ((i % 60) < 30) ? 3000 : -3000;
    }
    
    ESP_LOGI(TAG, "Testing GPIO %d...", gpio);
    size_t written;
    i2s_zero_dma_buffer(AUDIO_I2S_PORT);
    i2s_start(AUDIO_I2S_PORT);
    i2s_write(AUDIO_I2S_PORT, buf, len, &written, portMAX_DELAY);
    // don't stop I2S, just let it run silence
    
    free(buf);
}
