#include "message_bus.h"
#include "mimi_config.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "bus";

static QueueHandle_t s_inbound_queue;
static QueueHandle_t s_outbound_queue;

esp_err_t message_bus_init(void)
{
    s_inbound_queue = xQueueCreate(MIMI_BUS_QUEUE_LEN, sizeof(mimi_msg_t));
    s_outbound_queue = xQueueCreate(MIMI_BUS_QUEUE_LEN, sizeof(mimi_msg_t));

    if (!s_inbound_queue || !s_outbound_queue) {
        ESP_LOGE(TAG, "Failed to create message queues");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Message bus initialized (queue depth %d)", MIMI_BUS_QUEUE_LEN);
    return ESP_OK;
}

esp_err_t message_bus_push_inbound(const mimi_msg_t *msg)
{
    if (xQueueSend(s_inbound_queue, msg, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "Inbound queue full, dropping message");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t message_bus_pop_inbound(mimi_msg_t *msg, uint32_t timeout_ms)
{
    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xQueueReceive(s_inbound_queue, msg, ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t message_bus_pop_inbound_prefer_websocket(mimi_msg_t *msg, uint32_t timeout_ms)
{
    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xQueueReceive(s_inbound_queue, msg, ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (strcmp(msg->channel, MIMI_CHAN_WEBSOCKET) == 0) {
        return ESP_OK;
    }

    int waiting = (int)uxQueueMessagesWaiting(s_inbound_queue);
    if (waiting <= 0) return ESP_OK;

    mimi_msg_t tmp[MIMI_BUS_QUEUE_LEN];
    int count = 0;
    while (count < MIMI_BUS_QUEUE_LEN && xQueueReceive(s_inbound_queue, &tmp[count], 0) == pdTRUE) {
        count++;
    }

    int ws_idx = -1;
    for (int i = 0; i < count; i++) {
        if (strcmp(tmp[i].channel, MIMI_CHAN_WEBSOCKET) == 0) {
            ws_idx = i;
            break;
        }
    }

    if (ws_idx < 0) {
        for (int i = 0; i < count; i++) {
            xQueueSendToBack(s_inbound_queue, &tmp[i], 0);
        }
        return ESP_OK;
    }

    mimi_msg_t original = *msg;
    *msg = tmp[ws_idx];

    for (int i = 0; i < count; i++) {
        if (i == ws_idx) continue;
        xQueueSendToBack(s_inbound_queue, &tmp[i], 0);
    }
    xQueueSendToBack(s_inbound_queue, &original, 0);

    return ESP_OK;
}

int message_bus_inbound_depth(void)
{
    if (!s_inbound_queue) return 0;
    return (int)uxQueueMessagesWaiting(s_inbound_queue);
}

bool message_bus_inbound_contains(const char *channel, const char *chat_id)
{
    if (!s_inbound_queue || !channel || !chat_id) return false;

    int waiting = (int)uxQueueMessagesWaiting(s_inbound_queue);
    if (waiting <= 0) return false;

    mimi_msg_t tmp[MIMI_BUS_QUEUE_LEN];
    int count = 0;
    while (count < MIMI_BUS_QUEUE_LEN && xQueueReceive(s_inbound_queue, &tmp[count], 0) == pdTRUE) {
        count++;
    }

    bool found = false;
    for (int i = 0; i < count; i++) {
        if (strcmp(tmp[i].channel, channel) == 0 && strcmp(tmp[i].chat_id, chat_id) == 0) {
            found = true;
        }
        xQueueSendToBack(s_inbound_queue, &tmp[i], 0);
    }
    return found;
}

bool message_bus_inbound_has_channel(const char *channel)
{
    if (!s_inbound_queue || !channel) return false;

    int waiting = (int)uxQueueMessagesWaiting(s_inbound_queue);
    if (waiting <= 0) return false;

    mimi_msg_t tmp[MIMI_BUS_QUEUE_LEN];
    int count = 0;
    while (count < MIMI_BUS_QUEUE_LEN && xQueueReceive(s_inbound_queue, &tmp[count], 0) == pdTRUE) {
        count++;
    }

    bool found = false;
    for (int i = 0; i < count; i++) {
        if (strcmp(tmp[i].channel, channel) == 0) {
            found = true;
        }
        xQueueSendToBack(s_inbound_queue, &tmp[i], 0);
    }
    return found;
}

esp_err_t message_bus_push_outbound(const mimi_msg_t *msg)
{
    if (xQueueSend(s_outbound_queue, msg, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "Outbound queue full, dropping message");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t message_bus_pop_outbound(mimi_msg_t *msg, uint32_t timeout_ms)
{
    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xQueueReceive(s_outbound_queue, msg, ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}
