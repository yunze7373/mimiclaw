#pragma once

#include "esp_err.h"
#include "driver/i2s.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/**
 * Audio Configuration for INMP441 Microphone and MAX98357 Speaker
 */

/* I2S Ports:
 * Mic  -> I2S0 (pins from mimi_config: MIMI_PIN_I2S0_*)
 * Spk  -> I2S1 (pins from mimi_config: MIMI_PIN_I2S1_*)
 */
#define AUDIO_MIC_I2S_PORT       I2S_NUM_0
#define AUDIO_SPK_I2S_PORT       I2S_NUM_1

/* Audio Parameters */
#define AUDIO_SAMPLE_RATE        24000
#define AUDIO_BITS_PER_SAMPLE    16
#define AUDIO_CHANNELS           1

/* Buffer Sizes */
#define AUDIO_BUF_SIZE           4096
#define AUDIO_PCM_SIZE           640  // 20ms of 16kHz mono audio

/**
 * Initialize audio subsystem (I2S for mic and speaker)
 */
esp_err_t audio_init(void);

/**
 * Start microphone capture
 */
esp_err_t audio_mic_start(void);

/**
 * Stop microphone capture
 */
esp_err_t audio_mic_stop(void);

/**
 * Read audio data from microphone
 * Returns number of bytes read
 */
int audio_mic_read(uint8_t *buffer, size_t len);

/**
 * Start speaker playback
 */
esp_err_t audio_speaker_start(void);

/**
 * Stop speaker playback
 */
esp_err_t audio_speaker_stop(void);

/**
 * Set I2S sample rate dynamically (for matching TTS output)
 */
esp_err_t audio_set_sample_rate(uint32_t rate);

/**
 * Test a specific GPIO pin for speaker output (plays 1s tone)
 */
void audio_test_pin(int gpio);

/**
 * Write PCM data to speaker
 */
esp_err_t audio_speaker_write(const uint8_t *data, size_t len);

/**
 * Get audio configuration info
 */
esp_err_t audio_get_info(char *output, size_t output_size);

/**
 * Volume and mute control (speaker path).
 */
esp_err_t audio_set_volume_percent(int volume_percent);
int audio_get_volume_percent(void);
esp_err_t audio_adjust_volume(int delta_percent);
esp_err_t audio_set_muted(bool muted);
bool audio_is_muted(void);
