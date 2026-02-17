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
    setenv("TZ", MIMI_TIMEZONE, 1);
    tzset();

    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    strftime(out, out_size, "%Y-%m-%d %H:%M:%S %Z (%A)", &local);
}

esp_err_t tool_get_time_execute(const char *input_json, char *output, size_t output_size)
{
    ESP_LOGI(TAG, "Fetching current time...");

    /* Set timezone */
    setenv("TZ", MIMI_TIMEZONE, 1);
    tzset();

    /* If system clock is already synced, return immediately */
    if (time_is_valid()) {
        format_local_time(output, output_size);
        ESP_LOGI(TAG, "Time (cached): %s", output);
        return ESP_OK;
    }

    /* Start SNTP and wait for sync */
    ensure_sntp();

    int retries = 0;
    const int max_retries = 20; /* 10 seconds total */
    while (!time_is_valid() && retries < max_retries) {
        vTaskDelay(500 / portTICK_PERIOD_MS);
        retries++;
    }

    if (!time_is_valid()) {
        snprintf(output, output_size, "Error: NTP sync timeout, clock not set");
        ESP_LOGE(TAG, "%s", output);
        return ESP_ERR_TIMEOUT;
    }

    format_local_time(output, output_size);
    ESP_LOGI(TAG, "Time (SNTP): %s", output);
    return ESP_OK;
}
