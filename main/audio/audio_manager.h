#ifndef AUDIO_MANAGER_H
#define AUDIO_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the audio manager and ADF pipeline
esp_err_t audio_manager_init(void);

// Play audio from a URL (MP3/AAC/WAV)
esp_err_t audio_manager_play_url(const char *url);

// Play audio from a local file path (SPIFFS/SD)
esp_err_t audio_manager_play_file(const char *path);

// Stop current playback
esp_err_t audio_manager_stop(void);

// Pause playback
esp_err_t audio_manager_pause(void);

// Resume playback
esp_err_t audio_manager_resume(void);

// Set volume (0-100)
esp_err_t audio_manager_set_volume(int volume);

// Get current volume
int audio_manager_get_volume(void);

// Check if audio is currently playing
bool audio_manager_is_playing(void);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_MANAGER_H
