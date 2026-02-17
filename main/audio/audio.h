#pragma once

#include "esp_err.h"

/**
 * Audio Configuration for INMP441 Microphone and MAX98357 Speaker
 */

/* I2S Configuration */
#define AUDIO_I2S_WS_PIN         15
#define AUDIO_I2S_SCK_PIN        16
#define AUDIO_I2S_SD_PIN         7
#define AUDIO_I2S_DOUT_PIN       17  // Default speaker pin (try 17, 18, 6 if no sound)
#define AUDIO_I2S_PORT           I2S_NUM_1

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
