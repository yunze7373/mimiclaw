#pragma once

#include "esp_err.h"
#include "cJSON.h"
#include <stddef.h>
#include <stdbool.h>

#include "mimi_config.h"

/**
 * Initialize the LLM proxy. Reads API key and model from build-time secrets, then NVS.
 */
esp_err_t llm_proxy_init(void);

/**
 * Save the LLM API key to NVS.
 */
esp_err_t llm_set_api_key(const char *api_key);

/**
 * Save the LLM provider to NVS. (e.g. "anthropic", "openai")
 */
esp_err_t llm_set_provider(const char *provider);

/**
 * Save the model identifier to NVS.
 */
esp_err_t llm_set_model(const char *model);

/**
 * Save the Ollama host to NVS.
 */
esp_err_t llm_set_ollama_host(const char *host);

/**
 * Save the Ollama port to NVS.
 */
esp_err_t llm_set_ollama_port(const char *port);

/**
 * Get the current LLM provider.
 */
const char *llm_get_provider(void);

/**
 * Get the current LLM model.
 */
const char *llm_get_model(void);

/**
 * Enable or disable streaming mode. Saved to NVS.
 */
esp_err_t llm_set_streaming(bool enable);

/**
 * Get whether streaming is enabled.
 */
bool llm_get_streaming(void);

/* ── Audio Providers / Endpoints (Phase 1.3) ────────────────────── */

/**
 * Save the OpenAI compatible API key for audio to NVS.
 */
esp_err_t llm_set_openai_api_key_audio(const char *api_key);

/**
 * Save the ASR (Speech-to-Text) endpoint URL to NVS.
 */
esp_err_t llm_set_asr_endpoint(const char *endpoint);

/**
 * Save the TTS (Text-to-Speech) endpoint URL to NVS.
 */
esp_err_t llm_set_tts_endpoint(const char *endpoint);

/**
 * Get the current OpenAI compatible API key for audio.
 */
const char *llm_get_openai_api_key_audio(void);

/**
 * Get the current ASR (Speech-to-Text) endpoint URL.
 */
const char *llm_get_asr_endpoint(void);

/**
 * Get the current TTS (Text-to-Speech) endpoint URL.
 */
const char *llm_get_tts_endpoint(void);

/**
 * Send a chat completion request to the configured LLM API (non-streaming).
 *
 * @param system_prompt  System prompt string
 * @param messages_json  JSON array of messages: [{"role":"user","content":"..."},...]
 * @param response_buf   Output buffer for the complete response text
 * @param buf_size       Size of response_buf
 * @return ESP_OK on success
 */
esp_err_t llm_chat(const char *system_prompt, const char *messages_json,
                   char *response_buf, size_t buf_size);

/* ── Tool Use Support ──────────────────────────────────────────── */

typedef struct {
    char id[64];        /* "toolu_xxx" */
    char name[32];      /* "web_search" */
    char *input;        /* heap-allocated JSON string */
    size_t input_len;
} llm_tool_call_t;

typedef struct {
    char *text;                                  /* accumulated text blocks */
    size_t text_len;
    llm_tool_call_t calls[MIMI_MAX_TOOL_CALLS];
    int call_count;
    bool tool_use;                               /* stop_reason == "tool_use" */
} llm_response_t;

void llm_response_free(llm_response_t *resp);

/**
 * Send a chat completion request with tools to the configured LLM API (non-streaming).
 *
 * @param system_prompt  System prompt string
 * @param messages       cJSON array of messages (caller owns)
 * @param tools_json     Pre-built JSON string of tools array, or NULL for no tools
 * @param resp           Output: structured response with text and tool calls
 * @return ESP_OK on success
 */
esp_err_t llm_chat_tools(const char *system_prompt,
                         cJSON *messages,
                         const char *tools_json,
                         llm_response_t *resp);

/* ── Streaming Support ────────────────────────────────────────── */

/* Callback for streaming tokens.
 * token: null-terminated string of the new content delta.
 * ctx: user-provided context.
 */
typedef void (*llm_stream_cb_t)(const char *token, void *ctx);

/**
 * Set a progress status callback for HTTP event notifications.
 * The callback receives status strings like "ð å·²è¿æ¥", "ð¤ åéä¸­...", etc.
 * Set cb=NULL to clear.
 */
void llm_set_status_cb(llm_stream_cb_t cb, void *ctx);

/**
 * Send a chat completion request with streaming enabled.
 *
 * @param system_prompt  System prompt string
 * @param messages       cJSON array of messages
 * @param tools_json     Tools definition (NULL if none)
 * @param on_token       Callback for token generation
 * @param ctx            Context for callback
 * @param resp           Output: final aggregated response (optional, can be NULL if not needed)
 * @return ESP_OK on success
 */
esp_err_t llm_chat_stream(const char *system_prompt,
                          cJSON *messages,
                          const char *tools_json,
                          void (*on_token)(const char *token, void *ctx),
                          void *ctx,
                          llm_response_t *resp);
