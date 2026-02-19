#include "buttons/button_driver.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "display/display.h"
#include "ui/config_screen.h"
#include "audio/audio.h"

static const char *TAG = "button";

struct Button BUTTON1;
struct Button BUTTON2;
struct Button BUTTON3;
PressEvent BOOT_KEY_State, VOL_DOWN_State, VOL_UP_State;

static void ESP32_Button_init(void)
{
    const int pins[] = {Button_PIN1, Button_PIN2, Button_PIN3};
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        gpio_reset_pin((gpio_num_t)pins[i]);
        gpio_set_direction((gpio_num_t)pins[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode((gpio_num_t)pins[i], GPIO_PULLUP_ONLY);
    }
}

static uint8_t Read_Button_GPIO_Level(uint8_t button_id)
{
    if (button_id == 0) return (uint8_t)gpio_get_level((gpio_num_t)Button_PIN1);
    if (button_id == 1) return (uint8_t)gpio_get_level((gpio_num_t)Button_PIN2);
    if (button_id == 2) return (uint8_t)gpio_get_level((gpio_num_t)Button_PIN3);
    return 1;
}

static void Timer_Callback(void *arg)
{
    (void)arg;
    button_ticks();
}

static void Button_SINGLE_CLICK_Callback(void *btn)
{
    struct Button *user_button = (struct Button *)btn;

    if (user_button == &BUTTON1) {
        BOOT_KEY_State = SINGLE_CLICK;
        if (config_screen_is_active()) {
            config_screen_scroll_down();
        } else {
            display_cycle_backlight();
        }
        return;
    }

    if (user_button == &BUTTON2) {
        VOL_DOWN_State = SINGLE_CLICK;
        audio_set_muted(false);
        audio_adjust_volume(-5);
        ESP_LOGI(TAG, "Vol- short press -> volume=%d%%", audio_get_volume_percent());
        return;
    }

    if (user_button == &BUTTON3) {
        VOL_UP_State = SINGLE_CLICK;
        audio_set_muted(false);
        audio_adjust_volume(+5);
        ESP_LOGI(TAG, "Vol+ short press -> volume=%d%%", audio_get_volume_percent());
        return;
    }
}

static void Button_DOUBLE_CLICK_Callback(void *btn)
{
    struct Button *user_button = (struct Button *)btn;
    if (user_button == &BUTTON1) {
        BOOT_KEY_State = DOUBLE_CLICK;
    }
}

static void Button_LONG_PRESS_START_Callback(void *btn)
{
    struct Button *user_button = (struct Button *)btn;

    if (user_button == &BUTTON1) {
        BOOT_KEY_State = LONG_PRESS_START;
        return;
    }

    if (user_button == &BUTTON2) {
        VOL_DOWN_State = LONG_PRESS_START;
        audio_set_muted(!audio_is_muted());
        ESP_LOGI(TAG, "Vol- long press -> mute=%s", audio_is_muted() ? "ON" : "OFF");
    }
}

void button_Init(void)
{
    ESP32_Button_init();

    button_init(&BUTTON1, Read_Button_GPIO_Level, 0, 0);
    button_init(&BUTTON2, Read_Button_GPIO_Level, 0, 1);
    button_init(&BUTTON3, Read_Button_GPIO_Level, 0, 2);

    button_attach(&BUTTON1, SINGLE_CLICK, Button_SINGLE_CLICK_Callback);
    button_attach(&BUTTON1, DOUBLE_CLICK, Button_DOUBLE_CLICK_Callback);
    button_attach(&BUTTON1, LONG_PRESS_START, Button_LONG_PRESS_START_Callback);

    button_attach(&BUTTON2, SINGLE_CLICK, Button_SINGLE_CLICK_Callback);
    button_attach(&BUTTON2, LONG_PRESS_START, Button_LONG_PRESS_START_Callback);

    button_attach(&BUTTON3, SINGLE_CLICK, Button_SINGLE_CLICK_Callback);

    const esp_timer_create_args_t clock_tick_timer_args = {
        .callback = &Timer_Callback,
        .name = "button_tick",
        .arg = NULL,
    };

    esp_timer_handle_t clock_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&clock_tick_timer_args, &clock_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(clock_tick_timer, 1000 * 5));

    BOOT_KEY_State = NONE_PRESS;
    VOL_DOWN_State = NONE_PRESS;
    VOL_UP_State = NONE_PRESS;

    button_start(&BUTTON1);
    button_start(&BUTTON2);
    button_start(&BUTTON3);

    ESP_LOGI(TAG, "Buttons initialized: boot=%d vol_down=%d vol_up=%d", Button_PIN1, Button_PIN2, Button_PIN3);
}
