#include "agent_loop.h"
#include "agent/context_builder.h"
#include "mimi_config.h"
#include "bus/message_bus.h"
#include "llm/llm_proxy.h"
#include "memory/session_mgr.h"
#include "tools/tool_registry.h"
#include "telegram/telegram_bot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "cJSON.h"

static const char *TAG = "agent";

#define TOOL_OUTPUT_SIZE  (8 * 1024)

static void log_heap_snapshot(const char *phase)
{
    multi_heap_info_t internal_info = {0};
    multi_heap_info_t psram_info = {0};
    heap_caps_get_info(&internal_info, MALLOC_CAP_INTERNAL);
    heap_caps_get_info(&psram_info, MALLOC_CAP_SPIRAM);

    ESP_LOGI(TAG,
             "Heap[%s] internal_free=%d internal_min=%d internal_largest=%d psram_free=%d psram_largest=%d",
             phase ? phase : "n/a",
             (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (int)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (int)internal_info.largest_free_block,
             (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (int)psram_info.largest_free_block);
}

/* Build the assistant content array from llm_response_t for the messages history.
 * Returns a cJSON array with text and tool_use blocks. */
static cJSON *build_assistant_content(const llm_response_t *resp)
{
    cJSON *content = cJSON_CreateArray();

    /* Text block */
    if (resp->text && resp->text_len > 0) {
        cJSON *text_block = cJSON_CreateObject();
        cJSON_AddStringToObject(text_block, "type", "text");
        cJSON_AddStringToObject(text_block, "text", resp->text);
        cJSON_AddItemToArray(content, text_block);
    }

    /* Tool use blocks */
    for (int i = 0; i < resp->call_count; i++) {
        const llm_tool_call_t *call = &resp->calls[i];
        cJSON *tool_block = cJSON_CreateObject();
        cJSON_AddStringToObject(tool_block, "type", "tool_use");
        cJSON_AddStringToObject(tool_block, "id", call->id);
        cJSON_AddStringToObject(tool_block, "name", call->name);

        cJSON *input = cJSON_Parse(call->input);
        if (input) {
            cJSON_AddItemToObject(tool_block, "input", input);
        } else {
            cJSON_AddItemToObject(tool_block, "input", cJSON_CreateObject());
        }

        cJSON_AddItemToArray(content, tool_block);
    }

    return content;
}

/* Send a status message to the frontend */
static void send_status_msg(const char *channel, const char *chat_id, const char *text)
{
    /* Only send status to WebSocket (WebUI); other channels have native indicators */
    if (strcmp(channel, "websocket") != 0) return;

    /* Construct JSON manually: {"type":"status","content":"...","chat_id":"..."} */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "status");
    cJSON_AddStringToObject(root, "content", text);
    cJSON_AddStringToObject(root, "chat_id", chat_id);
    
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json) {
        mimi_msg_t out = {0};
        strncpy(out.channel, channel, sizeof(out.channel) - 1);
        strncpy(out.chat_id, chat_id, sizeof(out.chat_id) - 1);
        
        /* Prepend \x1F for raw JSON mode */
        size_t jlen = strlen(json);
        out.content = heap_caps_malloc(jlen + 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (out.content) {
            out.content[0] = '\x1F';
            memcpy(out.content + 1, json, jlen);
            out.content[jlen + 1] = '\0';
            message_bus_push_outbound(&out);
        }
        free(json);
    }
}

/* Build the user message with tool_result blocks */
static cJSON *build_tool_results(const llm_response_t *resp, char *tool_output, size_t tool_output_size, const char *channel, const char *chat_id)
{
    cJSON *content = cJSON_CreateArray();

    for (int i = 0; i < resp->call_count; i++) {
        const llm_tool_call_t *call = &resp->calls[i];

        /* Notify frontend */
        char status_buf[64];
        snprintf(status_buf, sizeof(status_buf), "Using tool: %s...", call->name);
        send_status_msg(channel, chat_id, status_buf);

        /* Execute tool */
        tool_output[0] = '\0';
        tool_registry_execute(call->name, call->input, tool_output, tool_output_size);

        ESP_LOGI(TAG, "Tool %s result: %d bytes", call->name, (int)strlen(tool_output));

        /* Build tool_result block */
        cJSON *result_block = cJSON_CreateObject();
        cJSON_AddStringToObject(result_block, "type", "tool_result");
        cJSON_AddStringToObject(result_block, "tool_use_id", call->id);
        cJSON_AddStringToObject(result_block, "content", tool_output);
        cJSON_AddItemToArray(content, result_block);
    }

    return content;
}



/* ── Streaming Helpers ────────────────────────────────────────── */

typedef struct {
    char channel[16];
    char chat_id[32];
    char buf[256];      /* Token buffer */
    size_t len;
} agent_stream_ctx_t;

static void stream_flush(agent_stream_ctx_t *ctx)
{
    if (ctx->len == 0) return;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "token");
    cJSON_AddStringToObject(root, "token", ctx->buf);
    cJSON_AddStringToObject(root, "chat_id", ctx->chat_id);
    
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json) {
        mimi_msg_t out = {0};
        strncpy(out.channel, ctx->channel, sizeof(out.channel) - 1);
        strncpy(out.chat_id, ctx->chat_id, sizeof(out.chat_id) - 1);
        
        /* Prepend \x1F to indicate raw JSON mode for ws_server */
        size_t jlen = strlen(json);
        out.content = heap_caps_malloc(jlen + 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (out.content) {
            out.content[0] = '\x1F';
            memcpy(out.content + 1, json, jlen);
            out.content[jlen + 1] = '\0';
            message_bus_push_outbound(&out);
        }
        free(json); /* json was alloc'd by cJSON_Print */
    }
    
    ctx->len = 0;
    ctx->buf[0] = '\0';
}

static void stream_token_cb(const char *token, void *arg)
{
    agent_stream_ctx_t *ctx = (agent_stream_ctx_t *)arg;
    
    size_t tlen = strlen(token);
    /* Flush if buffer full or large enough to send */
    /* Lower threshold to 20 bytes for better responsiveness, 
       or flush immediately if token contains newline */
    if (ctx->len + tlen >= sizeof(ctx->buf) - 1 || ctx->len > 32 || strchr(token, '\n')) {
        stream_flush(ctx);
    }

    /* Append to buffer */
    if (tlen < sizeof(ctx->buf)) {
        strcat(ctx->buf, token);
        ctx->len += tlen;
    } else {
        /* Giant token */
        if (ctx->len > 0) stream_flush(ctx);
        strncpy(ctx->buf, token, sizeof(ctx->buf)-1);
        ctx->len = strlen(ctx->buf);
        stream_flush(ctx);
    }
    
    /* Also flush if buffer has accumulated significant data even if not full */
    if (ctx->len > 20) {
        stream_flush(ctx);
    }
}

static void status_sender_cb(const char *status_text, void *arg)
{
    agent_stream_ctx_t *ctx = (agent_stream_ctx_t *)arg;
    if (!ctx || ctx->channel[0] == '\0') return;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "status");
    cJSON_AddStringToObject(root, "content", status_text);
    cJSON_AddStringToObject(root, "chat_id", ctx->chat_id);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json) {
        mimi_msg_t out = {0};
        strncpy(out.channel, ctx->channel, sizeof(out.channel) - 1);
        strncpy(out.chat_id, ctx->chat_id, sizeof(out.chat_id) - 1);

        size_t jlen = strlen(json);
        out.content = heap_caps_malloc(jlen + 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (out.content) {
            out.content[0] = '\x1F';
            memcpy(out.content + 1, json, jlen);
            out.content[jlen + 1] = '\0';
            message_bus_push_outbound(&out);
        }
        free(json);
    }
}

void agent_loop_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Agent loop started on core %d", xPortGetCoreID());

    /* Allocate large buffers from PSRAM */
    char *system_prompt = heap_caps_calloc(1, MIMI_CONTEXT_BUF_SIZE, MALLOC_CAP_SPIRAM);
    char *history_json = heap_caps_calloc(1, MIMI_LLM_STREAM_BUF_SIZE, MALLOC_CAP_SPIRAM);
    char *tool_output = heap_caps_calloc(1, TOOL_OUTPUT_SIZE, MALLOC_CAP_SPIRAM);

    if (!system_prompt || !history_json || !tool_output) {
        ESP_LOGE(TAG, "Failed to allocate PSRAM buffers");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        mimi_msg_t msg;
        esp_err_t err = message_bus_pop_inbound_prefer_websocket(&msg, UINT32_MAX);
        if (err != ESP_OK) continue;

        ESP_LOGI(TAG, "Processing message from %s:%s", msg.channel, msg.chat_id);


        /* 1. Build system prompt */
        context_build_system_prompt(system_prompt, MIMI_CONTEXT_BUF_SIZE);

        /* 2. Load session history into cJSON array */
        session_get_history_json(msg.chat_id, history_json,
                                 MIMI_LLM_STREAM_BUF_SIZE, MIMI_AGENT_MAX_HISTORY);

        cJSON *messages = cJSON_Parse(history_json);
        if (!messages) messages = cJSON_CreateArray();

        /* 3. Append current user message */
        cJSON *user_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(user_msg, "role", "user");
        cJSON_AddStringToObject(user_msg, "content", msg.content);
        cJSON_AddItemToArray(messages, user_msg);

        /* 4. ReAct loop */
        char *final_text = NULL;
        int iteration = 0;
        bool is_ws = (strcmp(msg.channel, "websocket") == 0);
        bool use_stream = is_ws && llm_get_streaming();

        while (iteration < MIMI_AGENT_MAX_TOOL_ITER) {
            const char *tools_json = tool_registry_get_tools_json();

            /* Send "working" indicator before each API call */
            agent_stream_ctx_t stream_ctx = {0};
            
            /* Always populate ctx for status messages on WebSocket */
            if (is_ws) {
                strncpy(stream_ctx.channel, msg.channel, sizeof(stream_ctx.channel) - 1);
                strncpy(stream_ctx.chat_id, msg.chat_id, sizeof(stream_ctx.chat_id) - 1);
            }
            if (!is_ws) {
                if (strcmp(msg.channel, MIMI_CHAN_TELEGRAM) == 0) {
                    /* Telegram: use native typing indicator */
                    telegram_send_chat_action(msg.chat_id, "typing");
                } else if (strcmp(msg.channel, MIMI_CHAN_SYSTEM) == 0) {
                    /* System channel: suppress verbose status spam in logs */
                } else {
                    /* Other non-streaming channels: send working text */
                    static const char *working_phrases[] = {
                        "mimi\xF0\x9F\x98\x97is working...",
                        "mimi\xF0\x9F\x90\xBE is thinking...",
                        "mimi\xF0\x9F\x92\xAD is pondering...",
                        "mimi\xF0\x9F\x8C\x99 is on it...",
                        "mimi\xE2\x9C\xA8 is cooking...",
                    };
                    const int phrase_count = sizeof(working_phrases) / sizeof(working_phrases[0]);
                    mimi_msg_t status = {0};
                    strncpy(status.channel, msg.channel, sizeof(status.channel) - 1);
                    strncpy(status.chat_id, msg.chat_id, sizeof(status.chat_id) - 1);
                    const char *phrase = working_phrases[esp_random() % phrase_count];
                    size_t plen = strlen(phrase);
                    status.content = heap_caps_malloc(plen + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                    if (status.content) memcpy(status.content, phrase, plen + 1);
                    if (status.content) message_bus_push_outbound(&status);
                }
            }

            /* Set status callback for HTTP progress */
            if (is_ws) {
                llm_set_status_cb(status_sender_cb, &stream_ctx);
                /* Send initial connecting status */
                status_sender_cb("Connecting...", &stream_ctx);
            }

            llm_response_t resp;

            if (use_stream) {
                /* Streaming path: tokens arrive via callback */
                err = llm_chat_stream(system_prompt, messages, tools_json, 
                                      stream_token_cb, &stream_ctx, &resp);
            } else {
                /* Non-streaming path: full response at once */
                err = llm_chat_tools(system_prompt, messages, tools_json, &resp);
            }

            /* Clear status callback */
            if (is_ws) llm_set_status_cb(NULL, NULL);

            /* Flush any remaining tokens */
            if (use_stream) stream_flush(&stream_ctx);

            if (err != ESP_OK) {
                ESP_LOGE(TAG, "LLM call failed: %s", esp_err_to_name(err));
                break;
            }

            if (!resp.tool_use) {
                /* Normal completion — save final text and break */
                if (resp.text && resp.text_len > 0) {
                    size_t tlen = resp.text_len;
                    final_text = heap_caps_malloc(tlen + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                    if (final_text) { memcpy(final_text, resp.text, tlen); final_text[tlen] = '\0'; }
                }
                llm_response_free(&resp);
                break;
            }

            ESP_LOGI(TAG, "Tool use iteration %d: %d calls", iteration + 1, resp.call_count);

            /* Append assistant message with content array */
            cJSON *asst_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(asst_msg, "role", "assistant");
            cJSON_AddItemToObject(asst_msg, "content", build_assistant_content(&resp));
            cJSON_AddItemToArray(messages, asst_msg);

            /* Execute tools and append results */
            cJSON *tool_results = build_tool_results(&resp, tool_output, TOOL_OUTPUT_SIZE, msg.channel, msg.chat_id);
            cJSON *result_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(result_msg, "role", "user");
            cJSON_AddItemToObject(result_msg, "content", tool_results);
            cJSON_AddItemToArray(messages, result_msg);

            log_heap_snapshot("after_tool_iteration");
            llm_response_free(&resp);
            iteration++;
        }

        cJSON_Delete(messages);

        if (!final_text && iteration >= MIMI_AGENT_MAX_TOOL_ITER) {
            const char *limit_msg = "The task is still running and reached the current tool-iteration limit. Please retry or simplify the request.";
            size_t mlen = strlen(limit_msg);
            final_text = heap_caps_malloc(mlen + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (final_text) {
                memcpy(final_text, limit_msg, mlen + 1);
            }
            ESP_LOGW(TAG, "Reached tool iteration limit: %d", MIMI_AGENT_MAX_TOOL_ITER);
        }

        /* 5. Send response */
        if (final_text && final_text[0]) {
            /* Save to session (only user text + final assistant text) */
            session_append(msg.chat_id, "user", msg.content);
            session_append(msg.chat_id, "assistant", final_text);

            /* Push response to outbound */
            if (is_ws) {
                if (!use_stream) {
                    /* Non-streaming on WebSocket: send full text as JSON response */
                    cJSON *rjson = cJSON_CreateObject();
                    cJSON_AddStringToObject(rjson, "type", "response");
                    cJSON_AddStringToObject(rjson, "content", final_text);
                    cJSON_AddStringToObject(rjson, "chat_id", msg.chat_id);
                    char *rstr = cJSON_PrintUnformatted(rjson);
                    cJSON_Delete(rjson);
                    if (rstr) {
                        mimi_msg_t out = {0};
                        strncpy(out.channel, msg.channel, sizeof(out.channel) - 1);
                        strncpy(out.chat_id, msg.chat_id, sizeof(out.chat_id) - 1);
                        size_t rlen = strlen(rstr);
                        out.content = heap_caps_malloc(rlen + 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                        if (out.content) {
                            out.content[0] = '\x1F';
                            memcpy(out.content + 1, rstr, rlen);
                            out.content[rlen + 1] = '\0';
                            message_bus_push_outbound(&out);
                        }
                        free(rstr);
                    }
                }
                /* Send done marker for WS (needed for both modes to stop thinking animation) */
                mimi_msg_t done = {0};
                strncpy(done.channel, msg.channel, sizeof(done.channel) - 1);
                strncpy(done.chat_id, msg.chat_id, sizeof(done.chat_id) - 1);
                /* Build done marker directly in PSRAM */
                char json_buf[128];
                int jlen = snprintf(json_buf, sizeof(json_buf),
                    "{\"type\":\"done\",\"chat_id\":\"%s\"}", msg.chat_id);
                if (jlen > 0) {
                    done.content = heap_caps_malloc(jlen + 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                    if (done.content) {
                        done.content[0] = '\x1F';
                        memcpy(done.content + 1, json_buf, jlen);
                        done.content[jlen + 1] = '\0';
                        message_bus_push_outbound(&done);
                    }
                }
            } else {
                /* Non-WebSocket channels: send full text */
                mimi_msg_t out = {0};
                strncpy(out.channel, msg.channel, sizeof(out.channel) - 1);
                strncpy(out.chat_id, msg.chat_id, sizeof(out.chat_id) - 1);
                size_t ftlen = strlen(final_text);
                out.content = heap_caps_malloc(ftlen + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (out.content) {
                    memcpy(out.content, final_text, ftlen + 1);
                    message_bus_push_outbound(&out);
                }
            }
            free(final_text);
        } else {
            /* Error or empty response */
            free(final_text);
            mimi_msg_t out = {0};
            strncpy(out.channel, msg.channel, sizeof(out.channel) - 1);
            strncpy(out.chat_id, msg.chat_id, sizeof(out.chat_id) - 1);
            const char *errmsg = "Sorry, I encountered an error.";
            out.content = heap_caps_malloc(strlen(errmsg) + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (out.content) memcpy(out.content, errmsg, strlen(errmsg) + 1);
            if (out.content) {
                message_bus_push_outbound(&out);
            }
        }

        /* Free inbound message content */
        free(msg.content);

        /* Log memory status */
        log_heap_snapshot("after_message");
    }
}

esp_err_t agent_loop_init(void)
{
    ESP_LOGI(TAG, "Agent loop initialized");
    return ESP_OK;
}

esp_err_t agent_loop_start(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(
        agent_loop_task, "agent_loop",
        MIMI_AGENT_STACK, NULL,
        MIMI_AGENT_PRIO, NULL, MIMI_AGENT_CORE);

    return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}
