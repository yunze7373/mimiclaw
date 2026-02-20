#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Register all audio-related tools:
 * - audio_play_url
 * - audio_stop
 * - audio_volume
 * - audio_status
 */
void register_audio_tools(void);
