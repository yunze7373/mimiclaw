#include "audio.h"
#include "../mimi_config.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/i2s.h"
#include "driver/gpio.h"

static const char *TAG = "audio";

static bool s_mic_started = false;
static bool s_speaker_started = false;
static bool s_mic_i2s_installed = false;
static bool s_spk_i2s_installed = false;
static int s_volume_percent = 70;
static bool s_muted = false;

static esp_err_t install_mic_i2s(void)
{
    if (s_mic_i2s_installed) {
        return ESP_OK;
    }

    i2s_config_t cfg = {
        .mode = I2S_MODE_MASTER | I2S_MODE_RX,
        .sample_rate = AUDIO_SAMPLE_RATE,
        .bits_per_sample = AUDIO_BITS_PER_SAMPLE,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0,
    };

    i2s_pin_config_t pins = {
        .bck_io_num = MIMI_PIN_I2S0_SCK,
        .ws_io_num = MIMI_PIN_I2S0_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = MIMI_PIN_I2S0_SD,
    };

    esp_err_t ret = i2s_driver_install(AUDIO_MIC_I2S_PORT, &cfg, 0, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Mic i2s_driver_install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2s_set_pin(AUDIO_MIC_I2S_PORT, &pins);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Mic i2s_set_pin failed: %s", esp_err_to_name(ret));
        i2s_driver_uninstall(AUDIO_MIC_I2S_PORT);
        return ret;
    }

    i2s_zero_dma_buffer(AUDIO_MIC_I2S_PORT);
    s_mic_i2s_installed = true;
    ESP_LOGI(TAG, "Mic I2S initialized (port=%d ws=%d sck=%d sd=%d)",
             AUDIO_MIC_I2S_PORT, MIMI_PIN_I2S0_WS, MIMI_PIN_I2S0_SCK, MIMI_PIN_I2S0_SD);
    return ESP_OK;
}

static esp_err_t install_spk_i2s(void)
{
    if (s_spk_i2s_installed) {
        return ESP_OK;
    }

    i2s_config_t cfg = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX,
        .sample_rate = AUDIO_SAMPLE_RATE,
        .bits_per_sample = AUDIO_BITS_PER_SAMPLE,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0,
    };

    i2s_pin_config_t pins = {
        .bck_io_num = MIMI_PIN_I2S1_BCLK,
        .ws_io_num = MIMI_PIN_I2S1_LRC,
        .data_out_num = MIMI_PIN_I2S1_DIN,
        .data_in_num = I2S_PIN_NO_CHANGE,
    };

    esp_err_t ret = i2s_driver_install(AUDIO_SPK_I2S_PORT, &cfg, 0, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Spk i2s_driver_install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2s_set_pin(AUDIO_SPK_I2S_PORT, &pins);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Spk i2s_set_pin failed: %s", esp_err_to_name(ret));
        i2s_driver_uninstall(AUDIO_SPK_I2S_PORT);
        return ret;
    }

    i2s_zero_dma_buffer(AUDIO_SPK_I2S_PORT);
    s_spk_i2s_installed = true;
    ESP_LOGI(TAG, "Speaker I2S initialized (port=%d din=%d bclk=%d lrc=%d)",
             AUDIO_SPK_I2S_PORT, MIMI_PIN_I2S1_DIN, MIMI_PIN_I2S1_BCLK, MIMI_PIN_I2S1_LRC);
    return ESP_OK;
}

esp_err_t audio_init(void)
{
    esp_err_t ret = install_mic_i2s();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = install_spk_i2s();
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "Audio initialized (mic_port=%d, spk_port=%d, rate=%d, bits=%d)",
             AUDIO_MIC_I2S_PORT, AUDIO_SPK_I2S_PORT, AUDIO_SAMPLE_RATE, AUDIO_BITS_PER_SAMPLE);
    return ESP_OK;
}

esp_err_t audio_mic_start(void)
{
    if (s_mic_started) {
        return ESP_OK;
    }

    if (!s_mic_i2s_installed) {
        esp_err_t init_err = install_mic_i2s();
        if (init_err != ESP_OK) {
            return init_err;
        }
    }

    esp_err_t ret = i2s_start(AUDIO_MIC_I2S_PORT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mic i2s_start failed: %s", esp_err_to_name(ret));
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

    i2s_stop(AUDIO_MIC_I2S_PORT);
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
    esp_err_t ret = i2s_read(AUDIO_MIC_I2S_PORT, buffer, len, &bytes_read, portMAX_DELAY);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S read error: %s", esp_err_to_name(ret));
        return 0;
    }

    return (int)bytes_read;
}

esp_err_t audio_speaker_start(void)
{
    if (s_speaker_started) {
        return ESP_OK;
    }

    if (!s_spk_i2s_installed) {
        esp_err_t init_err = install_spk_i2s();
        if (init_err != ESP_OK) {
            return init_err;
        }
    }

    esp_err_t ret = i2s_start(AUDIO_SPK_I2S_PORT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "speaker i2s_start failed: %s", esp_err_to_name(ret));
        return ret;
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

    i2s_stop(AUDIO_SPK_I2S_PORT);
    s_speaker_started = false;
    ESP_LOGI(TAG, "Speaker stopped");
    return ESP_OK;
}

esp_err_t audio_set_sample_rate(uint32_t rate)
{
    esp_err_t mic_ret = ESP_OK;
    esp_err_t spk_ret = ESP_OK;

    if (s_mic_i2s_installed) {
        mic_ret = i2s_set_sample_rates(AUDIO_MIC_I2S_PORT, rate);
    }
    if (s_spk_i2s_installed) {
        spk_ret = i2s_set_sample_rates(AUDIO_SPK_I2S_PORT, rate);
    }

    if (mic_ret == ESP_OK && spk_ret == ESP_OK) {
        ESP_LOGI(TAG, "I2S sample rate set to %lu Hz (mic=%d spk=%d)",
                 (unsigned long)rate, AUDIO_MIC_I2S_PORT, AUDIO_SPK_I2S_PORT);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to set sample rate (mic=%s spk=%s)",
             esp_err_to_name(mic_ret), esp_err_to_name(spk_ret));
    return (spk_ret != ESP_OK) ? spk_ret : mic_ret;
}

esp_err_t audio_speaker_write(const uint8_t *data, size_t len)
{
    if (!s_speaker_started || !s_spk_i2s_installed) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_muted) {
        static const uint8_t zeros[256] = {0};
        size_t remaining = len;
        while (remaining > 0) {
            size_t chunk = remaining > sizeof(zeros) ? sizeof(zeros) : remaining;
            size_t bytes_written = 0;
            esp_err_t zret = i2s_write(AUDIO_SPK_I2S_PORT, zeros, chunk, &bytes_written, portMAX_DELAY);
            if (zret != ESP_OK) {
                ESP_LOGE(TAG, "I2S write error (mute): %s", esp_err_to_name(zret));
                return zret;
            }
            remaining -= chunk;
        }
        return ESP_OK;
    }

    if (s_volume_percent >= 100 || (len % 2) != 0) {
        size_t bytes_written = 0;
        esp_err_t ret = i2s_write(AUDIO_SPK_I2S_PORT, data, len, &bytes_written, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2S write error: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    const int gain = s_volume_percent;
    const int16_t *in = (const int16_t *)data;
    size_t samples = len / sizeof(int16_t);
    size_t offset = 0;
    int16_t tmp[128];
    while (offset < samples) {
        size_t n = (samples - offset) > 128 ? 128 : (samples - offset);
        for (size_t i = 0; i < n; i++) {
            int32_t v = (int32_t)in[offset + i];
            v = (v * gain) / 100;
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            tmp[i] = (int16_t)v;
        }
        size_t bytes_written = 0;
        esp_err_t ret = i2s_write(AUDIO_SPK_I2S_PORT, tmp, n * sizeof(int16_t), &bytes_written, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2S write error (gain): %s", esp_err_to_name(ret));
            return ret;
        }
        offset += n;
    }
    return ESP_OK;
}

esp_err_t audio_get_info(char *output, size_t output_size)
{
    snprintf(output, output_size,
        "{\"mic\":{\"started\":%s,\"sample_rate\":%d,\"bits\":%d,\"i2s_port\":%d,\"ws\":%d,\"sck\":%d,\"sd\":%d},"
        "\"speaker\":{\"started\":%s,\"i2s_port\":%d,\"din\":%d,\"bclk\":%d,\"lrc\":%d}}",
        s_mic_started ? "true" : "false",
        AUDIO_SAMPLE_RATE,
        AUDIO_BITS_PER_SAMPLE,
        AUDIO_MIC_I2S_PORT,
        MIMI_PIN_I2S0_WS, MIMI_PIN_I2S0_SCK, MIMI_PIN_I2S0_SD,
        s_speaker_started ? "true" : "false",
        AUDIO_SPK_I2S_PORT,
        MIMI_PIN_I2S1_DIN, MIMI_PIN_I2S1_BCLK, MIMI_PIN_I2S1_LRC);

    return ESP_OK;
}

esp_err_t audio_set_volume_percent(int volume_percent)
{
    if (volume_percent < 0) volume_percent = 0;
    if (volume_percent > 100) volume_percent = 100;
    s_volume_percent = volume_percent;
    ESP_LOGI(TAG, "Speaker volume set to %d%%", s_volume_percent);
    return ESP_OK;
}

int audio_get_volume_percent(void)
{
    return s_volume_percent;
}

esp_err_t audio_adjust_volume(int delta_percent)
{
    return audio_set_volume_percent(s_volume_percent + delta_percent);
}

esp_err_t audio_set_muted(bool muted)
{
    s_muted = muted;
    ESP_LOGI(TAG, "Speaker mute: %s", s_muted ? "ON" : "OFF");
    return ESP_OK;
}

bool audio_is_muted(void)
{
    return s_muted;
}

void audio_test_pin(int gpio)
{
    if (!s_spk_i2s_installed && install_spk_i2s() != ESP_OK) {
        return;
    }

    i2s_pin_config_t pins = {
        .bck_io_num = MIMI_PIN_I2S1_BCLK,
        .ws_io_num = MIMI_PIN_I2S1_LRC,
        .data_out_num = gpio,
        .data_in_num = I2S_PIN_NO_CHANGE
    };
    i2s_set_pin(AUDIO_SPK_I2S_PORT, &pins);

    size_t len = AUDIO_SAMPLE_RATE * 2;
    int16_t *buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = malloc(len);
    }
    if (!buf) {
        return;
    }

    for (int i = 0; i < (int)(len / 2); i++) {
        buf[i] = ((i % 60) < 30) ? 3000 : -3000;
    }

    ESP_LOGI(TAG, "Testing GPIO %d...", gpio);
    size_t written = 0;
    i2s_zero_dma_buffer(AUDIO_SPK_I2S_PORT);
    i2s_start(AUDIO_SPK_I2S_PORT);
    i2s_write(AUDIO_SPK_I2S_PORT, buf, len, &written, portMAX_DELAY);

    free(buf);

    i2s_pin_config_t default_pins = {
        .bck_io_num = MIMI_PIN_I2S1_BCLK,
        .ws_io_num = MIMI_PIN_I2S1_LRC,
        .data_out_num = MIMI_PIN_I2S1_DIN,
        .data_in_num = I2S_PIN_NO_CHANGE
    };
    i2s_set_pin(AUDIO_SPK_I2S_PORT, &default_pins);
}
