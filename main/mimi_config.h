#pragma once

/* MimiClaw Global Configuration */

/* Build-time secrets (highest priority, override NVS) */
#if __has_include("mimi_secrets.h")
#include "mimi_secrets.h"
#endif

#ifndef MIMI_SECRET_WIFI_SSID
#define MIMI_SECRET_WIFI_SSID       ""
#endif
#ifndef MIMI_SECRET_WIFI_PASS
#define MIMI_SECRET_WIFI_PASS       ""
#endif
#ifndef MIMI_SECRET_TG_TOKEN
#define MIMI_SECRET_TG_TOKEN        ""
#endif
#ifndef MIMI_SECRET_API_KEY
#define MIMI_SECRET_API_KEY         ""
#endif
#ifndef MIMI_SECRET_MODEL
#define MIMI_SECRET_MODEL           ""
#endif
#ifndef MIMI_SECRET_MODEL_PROVIDER
#define MIMI_SECRET_MODEL_PROVIDER  "anthropic"
#endif
#ifndef MIMI_SECRET_PROXY_HOST
#define MIMI_SECRET_PROXY_HOST      ""
#endif
#ifndef MIMI_SECRET_PROXY_PORT
#define MIMI_SECRET_PROXY_PORT      ""
#endif
#ifndef MIMI_SECRET_OLLAMA_HOST
#define MIMI_SECRET_OLLAMA_HOST     ""
#endif
#ifndef MIMI_SECRET_OLLAMA_PORT
#define MIMI_SECRET_OLLAMA_PORT     "11434"
#endif
#ifndef MIMI_SECRET_SEARCH_KEY
#define MIMI_SECRET_SEARCH_KEY      ""
#endif

/* WiFi */
#define MIMI_WIFI_MAX_RETRY          10
#define MIMI_WIFI_RETRY_BASE_MS      1000
#define MIMI_WIFI_RETRY_MAX_MS       30000

/* Telegram Bot */
#define MIMI_TG_POLL_TIMEOUT_S       30
#define MIMI_TG_MAX_MSG_LEN          4096
#define MIMI_TG_POLL_STACK           (8 * 1024)
#define MIMI_TG_POLL_PRIO            5
#define MIMI_TG_POLL_CORE            0

/* Agent Loop */
#define MIMI_AGENT_STACK             (12 * 1024)
#define MIMI_AGENT_PRIO              6
#define MIMI_AGENT_CORE              1
#define MIMI_AGENT_MAX_HISTORY       20
#define MIMI_AGENT_MAX_TOOL_ITER     10
#define MIMI_MAX_TOOL_CALLS          4

/* Timezone (POSIX TZ format) */
#define MIMI_TIMEZONE                "PST8PDT,M3.2.0,M11.1.0"

/* LLM */
#define MIMI_LLM_DEFAULT_MODEL       "claude-opus-4-5"
#define MIMI_LLM_PROVIDER_DEFAULT    "anthropic"
#define MIMI_LLM_MAX_TOKENS          4096
#define MIMI_LLM_API_URL             "https://api.anthropic.com/v1/messages"
#define MIMI_OPENAI_API_URL          "https://api.openai.com/v1/chat/completions"
#define MIMI_MINIMAX_API_URL         "https://api.minimax.io/v1/text/chatcompletion_v2"
#define MIMI_MINIMAX_CODING_URL      "https://api.minimaxi.com/anthropic/v1/messages"
#define MIMI_OLLAMA_API_URL          "http://localhost:11434/v1/chat/completions"
#define MIMI_LLM_API_VERSION         "2023-06-01"
#define MIMI_LLM_STREAM_BUF_SIZE     (32 * 1024)

/* Message Bus */
#define MIMI_BUS_QUEUE_LEN           8
#define MIMI_OUTBOUND_STACK          (6 * 1024)
#define MIMI_OUTBOUND_PRIO           5
#define MIMI_OUTBOUND_CORE           0

/* Memory / SPIFFS */
#define MIMI_SPIFFS_BASE             "/spiffs"
#define MIMI_SPIFFS_CONFIG_DIR       "/spiffs/config"
#define MIMI_SPIFFS_MEMORY_DIR       "/spiffs/memory"
#define MIMI_SPIFFS_SESSION_DIR      "/spiffs/sessions"
#define MIMI_MEMORY_FILE             "/spiffs/memory/MEMORY.md"
#define MIMI_SOUL_FILE               "/spiffs/config/SOUL.md"
#define MIMI_USER_FILE               "/spiffs/config/USER.md"
#define MIMI_CONTEXT_BUF_SIZE        (16 * 1024)
#define MIMI_SESSION_MAX_MSGS        20

/* Cron Service */
#define MIMI_CRON_FILE               "/spiffs/config/cron.json"
#define MIMI_CRON_CHECK_INTERVAL_MS  (30 * 1000)
#define MIMI_CRON_MAX_JOBS           8

/* Heartbeat */
#define MIMI_HEARTBEAT_FILE          "/spiffs/config/HEARTBEAT.md"
#define MIMI_HEARTBEAT_INTERVAL_MS   (30 * 60 * 1000)

/* WebSocket Gateway */
#define MIMI_WS_PORT                 18789
#define MIMI_WS_MAX_CLIENTS          4

/* Serial CLI */
#define MIMI_CLI_STACK               (4 * 1024)
#define MIMI_CLI_PRIO                3
#define MIMI_CLI_CORE                0

/* NVS Namespaces */
#define MIMI_NVS_WIFI                "wifi_config"
#define MIMI_NVS_TG                  "tg_config"
#define MIMI_NVS_LLM                 "llm_config"
#define MIMI_NVS_PROXY               "proxy_config"
#define MIMI_NVS_SEARCH              "search_config"

/* NVS Keys */
#define MIMI_NVS_KEY_SSID            "ssid"
#define MIMI_NVS_KEY_PASS            "password"
#define MIMI_NVS_KEY_TG_TOKEN        "bot_token"
#define MIMI_NVS_KEY_API_KEY         "api_key"
#define MIMI_NVS_KEY_MODEL           "model"
#define MIMI_NVS_KEY_PROVIDER        "provider"
#define MIMI_NVS_KEY_PROXY_HOST      "host"
#define MIMI_NVS_KEY_PROXY_PORT      "port"
#define MIMI_NVS_KEY_OLLAMA_HOST     "ollama_host"
#define MIMI_NVS_KEY_OLLAMA_PORT     "ollama_port"

/* ========================================
 * Hardware Feature Flags
 * ======================================== */
#define MIMI_HAS_LCD                 0    /* 1 = board has SPI LCD (ST7789T) */

/* ========================================
 * Hardware Pin Map — ESP32-S3 Audio Dev Board
 * See HARD_WRITING.md for wiring details
 * ======================================== */
/* On-board RGB LED */
#define MIMI_PIN_RGB_LED             48   /* WS2812 Data */

/* Volume Buttons */
#define MIMI_PIN_VOL_DOWN           39   /* Volume down / mute button */
#define MIMI_PIN_VOL_UP             40   /* Volume up button */

/* I2C0 - SSD1306 OLED Display */
#define MIMI_PIN_I2C0_SDA          41
#define MIMI_PIN_I2C0_SCL          42
#define MIMI_I2C0_FREQ_HZ          400000

/* I2S0 - INMP441 Microphone */
#define MIMI_PIN_I2S0_WS            4    /* WS / LRCK */
#define MIMI_PIN_I2S0_SCK          5    /* SCK / BCLK */
#define MIMI_PIN_I2S0_SD           6    /* SD / DIN */

/* I2S1 - MAX98357A Amplifier */
#define MIMI_PIN_I2S1_DIN           7
#define MIMI_PIN_I2S1_BCLK         15
#define MIMI_PIN_I2S1_LRC          16

/* Boot Button */
#define MIMI_PIN_BOOT_KEY            0    /* Boot Button (active low) */
/* UART0 - USB Serial */
#define MIMI_PIN_UART0_TX           43   /* USB-Serial CH343P TX */
#define MIMI_PIN_UART0_RX           44   /* USB-Serial CH343P RX */

/* ADC Configuration */
#define MIMI_ADC_UNIT                ADC_UNIT_1
#define MIMI_ADC_DEFAULT_ATTEN       ADC_ATTEN_DB_12  /* 0~3.1V range */
#define MIMI_ADC_DEFAULT_BITWIDTH    ADC_BITWIDTH_12

/* PWM (LEDC) Configuration */
#define MIMI_PWM_TIMER               LEDC_TIMER_1  /* TIMER_0 reserved for LCD backlight */
#define MIMI_PWM_MODE                LEDC_LOW_SPEED_MODE
#define MIMI_PWM_MAX_CHANNELS        4
#define MIMI_PWM_DEFAULT_FREQ_HZ     5000
#define MIMI_PWM_DUTY_RESOLUTION     LEDC_TIMER_13_BIT  /* 0~8191 */

/* BLE Scan */
#define MIMI_BLE_SCAN_DURATION_S     5
