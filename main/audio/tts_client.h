#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Send text to the TTS endpoint and stream audio to the speaker
 * 
 * @param text The text to convert to speech
 * @return esp_err_t ESP_OK on success
 */
esp_err_t tts_speak(const char *text);

#ifdef __cplusplus
}
#endif
