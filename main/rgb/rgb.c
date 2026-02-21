#include "rgb/rgb.h"

#include "esp_check.h"
#include "led_strip.h"
#include "mimi_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

#define RGB_GPIO MIMI_PIN_RGB_LED

static led_strip_handle_t s_strip = NULL;

/* Breathing effect state */
static TaskHandle_t s_breathing_task = NULL;
static uint32_t s_breath_period_ms;

static void hsv_to_rgb(float h_deg, float s, float v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    float c = v * s;
    float h = h_deg / 60.0f;
    float x = c * (1.0f - fabsf(fmodf(h, 2.0f) - 1.0f));
    float m = v - c;
    float rf = 0.0f, gf = 0.0f, bf = 0.0f;

    if (h < 1.0f) {
        rf = c; gf = x; bf = 0.0f;
    } else if (h < 2.0f) {
        rf = x; gf = c; bf = 0.0f;
    } else if (h < 3.0f) {
        rf = 0.0f; gf = c; bf = x;
    } else if (h < 4.0f) {
        rf = 0.0f; gf = x; bf = c;
    } else if (h < 5.0f) {
        rf = x; gf = 0.0f; bf = c;
    } else {
        rf = c; gf = 0.0f; bf = x;
    }

    *r = (uint8_t)((rf + m) * 255.0f);
    *g = (uint8_t)((gf + m) * 255.0f);
    *b = (uint8_t)((bf + m) * 255.0f);
}

static void rgb_breathing_task(void *arg)
{
    float phase = 0.0f;
    const float step = (2.0f * M_PI) / (s_breath_period_ms / 20.0f); // 20ms tick
    float hue_deg = 0.0f;
    /* Full hue cycle in about 6 seconds (50Hz -> ~300 ticks). */
    const float hue_step = 360.0f / 300.0f;

    while (1) {
        // Calculate brightness multiplier using sine wave (0.0 to 1.0)
        float brightness = (sinf(phase) + 1.0f) / 2.0f;
        
        // Apply minimum brightness to avoid completely turning off (e.g. 5%)
        brightness = 0.05f + (brightness * 0.95f);

        uint8_t r, g, b;
        hsv_to_rgb(hue_deg, 1.0f, brightness, &r, &g, &b);

        // Update hardware directly (bypassing rgb_set lazy init since task implies init is done)
        if (s_strip) {
            led_strip_set_pixel(s_strip, 0, g, r, b); // Swap R/G
            led_strip_refresh(s_strip);
        }

        phase += step;
        if (phase >= 2.0f * M_PI) {
            phase -= 2.0f * M_PI;
        }
        hue_deg += hue_step;
        if (hue_deg >= 360.0f) {
            hue_deg -= 360.0f;
        }

        vTaskDelay(pdMS_TO_TICKS(20)); // Update at 50Hz for smooth breathing
    }
}

esp_err_t rgb_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = RGB_GPIO,
        .max_leds = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };

    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip), "rgb", "led_strip init failed");
    led_strip_clear(s_strip);
    return ESP_OK;
}

void rgb_set(uint8_t r, uint8_t g, uint8_t b)
{
    /* Stop breathing if setting a solid color */
    rgb_stop_breathing();

    /* Lazy initialization: if not initialized, do it now */
    if (!s_strip) {
        if (rgb_init() != ESP_OK) {
            return;
        }
    }
    
    /* Swap R/G for this board's LED ordering */
    led_strip_set_pixel(s_strip, 0, g, r, b);
    led_strip_refresh(s_strip);
}

void rgb_start_breathing(uint8_t r, uint8_t g, uint8_t b, uint32_t period_ms)
{
    // Ensure initialized
    if (!s_strip) {
        if (rgb_init() != ESP_OK) {
            return;
        }
    }

    (void)r;
    (void)g;
    (void)b;
    s_breath_period_ms = period_ms > 100 ? period_ms : 1000; // minimum 100ms

    if (s_breathing_task == NULL) {
        xTaskCreate(rgb_breathing_task, "rgb_breath", 2048, NULL, 5, &s_breathing_task);
    }
}

void rgb_stop_breathing(void)
{
    if (s_breathing_task != NULL) {
        vTaskDelete(s_breathing_task);
        s_breathing_task = NULL;
    }
}
