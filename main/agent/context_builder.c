#include "context_builder.h"
#include "mimi_config.h"
#include "memory/memory_store.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "context";

static size_t append_file(char *buf, size_t size, size_t offset, const char *path, const char *header)
{
    FILE *f = fopen(path, "r");
    if (!f) return offset;

    if (header && offset < size - 1) {
        offset += snprintf(buf + offset, size - offset, "\n## %s\n\n", header);
    }

    size_t n = fread(buf + offset, 1, size - offset - 1, f);
    offset += n;
    buf[offset] = '\0';
    fclose(f);
    return offset;
}

esp_err_t context_build_system_prompt(char *buf, size_t size)
{
    size_t off = 0;

    off += snprintf(buf + off, size - off,
        "# MimiClaw\n"
        "You are MimiClaw on ESP32-S3. Be helpful, accurate, and concise.\n\n"
        "## Tools\n"
        "- web_search: Search web for facts/news.\n"
        "- get_current_time: Get date/time. Use this instead of guessing.\n"
        "- read_file: Read SPIFFS file.\n"
        "- write_file: Write/overwrite SPIFFS file.\n"
        "- edit_file: Find/replace in SPIFFS file.\n"
        "- list_dir: List SPIFFS files.\n"
        "- cron_add: Schedule tasks.\n"
        "- cron_list: List tasks.\n"
        "- cron_remove: Remove task.\n"
        "- set_timezone: Set system timezone.\n\n"
        "## Memory\n"
        "- Long-term: /spiffs/memory/MEMORY.md\n"
        "- Daily: /spiffs/memory/daily/<YYYY-MM-DD>.md\n"
        "Update MEMORY.md with new user info. Append daily notes for important events.\n"
        "Always read_file before writing.\n");

    /* Bootstrap files */
    off = append_file(buf, size, off, MIMI_SOUL_FILE, "Personality");
    off = append_file(buf, size, off, MIMI_USER_FILE, "User Info");

    /* Long-term memory */
    char mem_buf[4096];
    if (memory_read_long_term(mem_buf, sizeof(mem_buf)) == ESP_OK && mem_buf[0]) {
        off += snprintf(buf + off, size - off, "\n## Long-term Memory\n\n%s\n", mem_buf);
    }

    /* Recent daily notes (last 1 day) */
    char recent_buf[2048];
    if (memory_read_recent(recent_buf, sizeof(recent_buf), 1) == ESP_OK && recent_buf[0]) {
        off += snprintf(buf + off, size - off, "\n## Recent Notes\n\n%s\n", recent_buf);
    }

    ESP_LOGI(TAG, "System prompt built: %d bytes", (int)off);
    return ESP_OK;
}

esp_err_t context_build_messages(const char *history_json, const char *user_message,
                                 char *buf, size_t size)
{
    /* Parse existing history */
    cJSON *history = cJSON_Parse(history_json);
    if (!history) {
        history = cJSON_CreateArray();
    }

    /* Append current user message */
    cJSON *user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", user_message);
    cJSON_AddItemToArray(history, user_msg);

    /* Serialize */
    char *json_str = cJSON_PrintUnformatted(history);
    cJSON_Delete(history);

    if (json_str) {
        strncpy(buf, json_str, size - 1);
        buf[size - 1] = '\0';
        free(json_str);
    } else {
        snprintf(buf, size, "[{\"role\":\"user\",\"content\":\"%s\"}]", user_message);
    }

    return ESP_OK;
}
