#include "system_manager.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_system.h"
#include "esp_heap_caps.h"

#define TAG "SYS_MGR"
#define NVS_NAMESPACE "system"
#define KEY_BOOT_COUNT "boot_count"
#define MAX_BOOT_ATTEMPTS 3
#define BOOT_SUCCESS_TIMEOUT_MS 60000 // 60 seconds

// BOOT Button is typically GPIO 0s on ESP32
#define PIN_BOOT_BUTTON GPIO_NUM_0

static bool s_safe_mode = false;
static esp_timer_handle_t s_boot_timer;

// Callback to reset boot count after successful runtime
static void boot_success_callback(void* arg)
{
    ESP_LOGI(TAG, "System stable for %d ms. Resetting boot count.", BOOT_SUCCESS_TIMEOUT_MS);
    
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err == ESP_OK) {
        nvs_set_i32(my_handle, KEY_BOOT_COUNT, 0);
        nvs_commit(my_handle);
        nvs_close(my_handle);
    }
}

void system_manager_init(void)
{
    // 1. Check Boot Button (GPIO 0)
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_BOOT_BUTTON),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    
    // Check if button is pressed (Active Low)
    if (gpio_get_level(PIN_BOOT_BUTTON) == 0) {
        ESP_LOGw(TAG, "BOOT Button held detected! Forcing Safe Mode.");
        s_safe_mode = true;
    }

    // 2. NVS Boot Counter Check
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return;
    }

    int32_t boot_count = 0;
    err = nvs_get_i32(my_handle, KEY_BOOT_COUNT, &boot_count);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        boot_count = 0;
    }

    ESP_LOGI(TAG, "Boot Count: %ld", boot_count);

    if (boot_count >= MAX_BOOT_ATTEMPTS) {
        ESP_LOGE(TAG, "Crash loop detected (%ld consecutive boots). Entering Safe Mode.", boot_count);
        s_safe_mode = true;
    }

    // Increment boot count for next time
    // If we crash before 60s, this count will remain high.
    // If we survive 60s, timer resets it to 0.
    boot_count++;
    nvs_set_i32(my_handle, KEY_BOOT_COUNT, boot_count);
    nvs_commit(my_handle);
    nvs_close(my_handle);

    // 3. Start Success Timer (if not already strictly in safe mode from button)
    // Actually, even in safe mode we might want to reset the counter? 
    // Yes, if safe mode boots successfully, we should probably reset the crash counter so next normal boot has a chance.
    const esp_timer_create_args_t periodic_timer_args = {
        .callback = &boot_success_callback,
        .name = "boot_success"
    };

    ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &s_boot_timer));
    ESP_ERROR_CHECK(esp_timer_start_once(s_boot_timer, BOOT_SUCCESS_TIMEOUT_MS * 1000));

    if (s_safe_mode) {
        // Blink LED or other indication could go here
        ESP_LOGW(TAG, "==========================================");
        ESP_LOGW(TAG, "             SYSTEM IN SAFE MODE          ");
        ESP_LOGW(TAG, "   Skills and Agent will NOT actiavted.   ");
        ESP_LOGW(TAG, "==========================================");
    }
}

bool system_is_safe_mode(void)
{
    return s_safe_mode;
}

void system_mark_boot_successful(void)
{
    // Can be exposed if we want manual triggering, but timer handles it.
    boot_success_callback(NULL);
}

char *system_get_health_json(void)
{
    cJSON *root = cJSON_CreateObject();
    
    // System Info
    cJSON_AddBoolToObject(root, "safe_mode", s_safe_mode);
    cJSON_AddNumberToObject(root, "uptime", esp_timer_get_time() / 1000000); // seconds
    
    // Memory
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "min_free_heap", esp_get_minimum_free_heap_size());
#ifdef CONFIG_SPIRAM
    cJSON_AddNumberToObject(root, "free_psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#endif

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str;
}
