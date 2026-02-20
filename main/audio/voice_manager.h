#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VOICE_STATE_IDLE,
    VOICE_STATE_LISTENING,
    VOICE_STATE_PROCESSING,
    VOICE_STATE_SPEAKING
} voice_state_t;

/**
 * @brief Initialize the voice manager state machine
 */
esp_err_t voice_manager_init(void);

/**
 * @brief Force start listening (recording audio)
 */
esp_err_t voice_manager_start_listening(void);

/**
 * @brief Force stop processing/listening/speaking
 */
esp_err_t voice_manager_stop(void);

/**
 * @brief Get the current state of the voice assistant
 */
voice_state_t voice_manager_get_state(void);

#ifdef __cplusplus
}
#endif
