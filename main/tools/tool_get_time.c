#include "tool_get_time.h"
#include "mimi_config.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_sntp.h"

static const char *TAG = "tool_time";
static bool sntp_started = false;

/* Initialize SNTP if not already started */
static void ensure_sntp(void)
{
    if (sntp_started) return;

    ESP_LOGI(TAG, "Initializing SNTP...");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.nist.gov");
    esp_sntp_setservername(2, "ntp.aliyun.com");
    esp_sntp_init();
    sntp_started = true;
}

#include "nvs_flash.h"
#include "nvs.h"

static char s_timezone[64] = MIMI_TIMEZONE;

/* Initialize timezone from NVS */
void tool_time_init(void)
{
    nvs_handle_t nvs;
    if (nvs_open("mimi_config", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(s_timezone);
        nvs_get_str(nvs, "timezone", s_timezone, &len);
        nvs_close(nvs);
    }
}

/* Check if system time looks valid (year >= 2024) */
static bool time_is_valid(void)
{
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    return (tm.tm_year >= (2024 - 1900));
}

/* Format current local time into output buffer */
static void format_local_time(char *out, size_t out_size)
{
    setenv("TZ", s_timezone, 1);
    tzset();

    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    strftime(out, out_size, "%Y-%m-%d %H:%M:%S %Z (%A)", &local);
}

esp_err_t tool_get_time_execute(const char *input_json, char *output, size_t output_size)
{
    ESP_LOGI(TAG, "Fetching current time...");
    /* Ensure SNTP is running */
    ensure_sntp();
    
    /* Wait if not synced (only short wait) */
    if (!time_is_valid()) {
        int retries = 0;
        while (!time_is_valid() && retries < 10) {
            vTaskDelay(200 / portTICK_PERIOD_MS);
            retries++;
        }
    }

    if (!time_is_valid()) {
        snprintf(output, output_size, "Error: NTP sync timeout. Reading system time anyway.");
    }
    
    format_local_time(output, output_size);
    ESP_LOGI(TAG, "Time: %s (TZ=%s)", output, s_timezone);
    return ESP_OK;
}

esp_err_t tool_set_timezone_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: Invalid JSON input");
        return ESP_FAIL;
    }

    cJSON *tz = cJSON_GetObjectItem(root, "timezone");
    if (!tz || !cJSON_IsString(tz)) {
        snprintf(output, output_size, "Error: Missing 'timezone' string (e.g. 'CST-8')");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    strncpy(s_timezone, tz->valuestring, sizeof(s_timezone) - 1);
    s_timezone[sizeof(s_timezone) - 1] = '\0';
    cJSON_Delete(root);

    /* Save to NVS */
    nvs_handle_t nvs;
    if (nvs_open("mimi_config", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "timezone", s_timezone);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    /* Apply immediately */
    setenv("TZ", s_timezone, 1);
    tzset();

    snprintf(output, output_size, "Timezone set to %s. Current time: ", s_timezone);
    size_t len = strlen(output);
    format_local_time(output + len, output_size - len);
    
    return ESP_OK;
}
