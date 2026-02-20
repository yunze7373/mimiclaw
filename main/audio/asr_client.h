#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Send raw I2S audio data to the ASR endpoint and get recognized text
 * 
 * @param audio_data Pointer to the raw PCM data
 * @param len Length of the data in bytes
 * @param out_text Pointer to a char pointer that will hold the result. Must be freed by caller.
 * @return esp_err_t ESP_OK on success
 */
esp_err_t asr_recognize(const uint8_t *audio_data, size_t len, char **out_text);

#ifdef __cplusplus
}
#endif
