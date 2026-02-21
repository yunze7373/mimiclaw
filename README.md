# Esp32Claw: Pocket AI Assistant on a $5 Chip

<p align="center">
  <img src="assets/banner.png" alt="Esp32Claw" width="500" />
</p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-yellow.svg" alt="License: MIT"></a>
</p>

<p align="center">
  <strong><a href="README.md">English</a> | <a href="README_CN.md">中文</a> | <a href="README_JA.md">日本語</a></strong>
</p>

**An ESP32-S3 AI Assistant. No Linux. No Node.js. Just pure C**

Esp32Claw turns a tiny ESP32-S3 board into a personal AI assistant. Plug it into USB power, connect to WiFi, and talk to it through Telegram or WebSocket — it handles any task you throw at it and evolves over time with local memory — all on a chip the size of a thumb.

## Meet Esp32Claw

- **Tiny** — No Linux, no Node.js, no bloat — just pure C
- **Handy** — Message it from Telegram, Web UI, or WebSocket
- **Loyal** — Learns from memory, remembers across reboots
- **Energetic** — USB power, 0.5 W, runs 24/7
- **Lovable** — One ESP32-S3 board, $5, nothing else
- **Voice-Enabled** — ASR, TTS, and voice interaction support
- **Extensible** — Lua-based skill engine, MCP protocol support

## How It Works

![](assets/mimiclaw.png)

You send a message on Telegram or WebSocket. The ESP32-S3 picks it up over WiFi, feeds it into an agent loop — the LLM thinks, calls tools, reads memory — and sends the reply back. Supports **Anthropic (Claude)**, **OpenAI (GPT)**, and **MiniMax** as providers, switchable at runtime. Everything runs on a single $5 chip with all your data stored locally on flash.

## Features

### Core AI Capabilities
- **LLM Integration** — Supports Anthropic (Claude), OpenAI (GPT), and MiniMax
- **ReAct Agent Loop** — Multi-turn tool calling with iterative reasoning
- **Tool Registry** — Dynamic tool registration and execution system
- **Memory System** — Persistent memory across reboots (SPIFFS)

### Communication
- **Telegram Bot** — Message from anywhere via Telegram
- **WebSocket Gateway** — Connect from any WebSocket client (port 18789)
- **Web UI** — Built-in single-page application for device control
- **MQTT** — Home Assistant integration with auto-discovery
- **mDNS/SSDP** — Network peer discovery

### Audio & Voice
- **I2S Audio** — Microphone input and speaker output
- **Voice Manager** — VAD, ASR, LLM, and TTS pipeline
- **Native MP3 Playback** — Stream audio from URLs (minimp3)
- **ESP-ADF Support** — Optional Audio Development Framework integration

### Skills & Extensions
- **Skill Engine** — Lua 5.4 runtime for dynamic behaviors
- **Skill Templates** — GPIO, I2C, Timer, and custom templates
- **API Skills** — Register HTTP endpoints as tools
- **MCP Protocol** — Model Context Protocol client for external tools
- **Skill Rollback** — Backup and restore skill versions

### Home Automation
- **Zigbee Gateway** — Control Zigbee devices
- **Home Assistant** — MQTT integration with auto-discovery
- **Peer Federation** — Connect multiple Esp32Claw devices

### System
- **OTA Updates** — Flash firmware over WiFi
- **Dual-Core** — Network I/O and AI on separate cores
- **HTTP Proxy** — CONNECT tunnel support
- **Cron Scheduler** — AI can schedule its own tasks
- **Heartbeat** — Autonomous task execution

## Hardware

### Supported Boards
- ESP32-S3 with 16 MB flash + 8 MB PSRAM
- Recommended: Xiaozhi AI board, ESP32-S3 N16R8 (~$10)

### I2S Audio Pins (Configurable)
- **Microphone (I2S0)**: WS=4, SCK=5, SD=6
- **Speaker (I2S1)**: DIN=7, BCLK=15, LRC=16

## Quick Start

### What You Need

- An **ESP32-S3 dev board** with 16 MB flash and 8 MB PSRAM
- A **USB Type-C cable**
- A **Telegram bot token** — talk to [@BotFather](https://t.me/BotFather)
- An **LLM API key** — Anthropic, OpenAI, or MiniMax

### Install

```bash
# You need ESP-IDF v5.5+ installed first:
# https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32s3/get-started/

git clone https://github.com/yunze7373/mimiclaw.git
cd mimiclaw

idf.py set-target esp32s3
```

### Configure

```bash
cp main/mimi_secrets.h.example main/mimi_secrets.h
```

Edit `main/mimi_secrets.h`:

```c
#define MIMI_SECRET_WIFI_SSID       "YourWiFiName"
#define MIMI_SECRET_WIFI_PASS       "YourWiFiPassword"
#define MIMI_SECRET_TG_TOKEN        "123456:ABC-DEF1234ghIkl-zyx57W2v1u123ew11"
#define MIMI_SECRET_API_KEY         "sk-ant-api03-xxxxx"
#define MIMI_SECRET_MODEL_PROVIDER  "anthropic"     // "anthropic", "openai", or "minimax"
```

Then build and flash:

```bash
idf.py fullclean && idf.py build

# Find your serial port
idf.py -p /dev/ttyACM0 flash monitor
```

## CLI Commands

Connect via serial to configure or debug.

**Runtime config** (saved to NVS):

```
mimi> wifi_set MySSID MyPassword   # change WiFi network
mimi> set_tg_token 123456:ABC...   # change Telegram bot token
mimi> set_api_key sk-ant-api03-... # change API key
mimi> set_model_provider openai    # switch provider (anthropic|openai|minimax)
mimi> set_model gpt-4o             # change LLM model
mimi> set_proxy 127.0.0.1 7897     # set HTTP proxy
mimi> clear_proxy                  # remove proxy
mimi> config_show                  # show all config (masked)
mimi> config_reset                 # clear NVS
```

**Debug & maintenance:**

```
mimi> wifi_status              # am I connected?
mimi> memory_read              # see what the bot remembers
mimi> memory_write "content"   # write to MEMORY.md
mimi> heap_info                # how much RAM is free?
mimi> session_list             # list all chat sessions
mimi> session_clear 12345       # wipe a conversation
mimi> restart                  # reboot
```

## Memory Files

| File | What it is |
|------|------------|
| `SOUL.md` | The bot's personality |
| `USER.md` | Info about you |
| `MEMORY.md` | Long-term memory |
| `HEARTBEAT.md` | Autonomous task list |
| `cron.json` | Scheduled jobs |
| `daily/<YYYY-MM-DD>.md` | Daily notes |

## Tools

| Tool | Description |
|------|-------------|
| `web_search` | Search the web via Brave Search |
| `get_current_time` | Fetch current date/time |
| `cron_add` | Schedule recurring/one-shot tasks |
| `cron_list` | List scheduled jobs |
| `cron_remove` | Remove a cron job |
| `audio_play_url` | Play audio from URL |
| `audio_volume` | Set volume (0-100) |
| `audio_stop` | Stop playback |
| `audio_test_tone` | Test speaker hardware |
| `audio_test_mic` | Test microphone |
| `gpio_control` | Control GPIO pins |
| `i2c_scan` | Scan I2C devices |
| `adc_read` | Read ADC values |
| `pwm_control` | PWM control |
| `zigbee_devices` | List Zigbee devices |
| `zigbee_onoff` | Control Zigbee device |
| `skill_install` | Install a skill |
| `skill_list` | List installed skills |
| `skill_uninstall` | Remove a skill |

## Architecture

```
┌─────────────────────────────────────────────────────┐
│                   ESP32-S3                         │
├─────────────────────────────────────────────────────┤
│  Web UI  │  Telegram  │  WebSocket  │  MQTT       │
├─────────────────────────────────────────────────────┤
│              Agent Loop (ReAct)                     │
│  ┌──────────────────────────────────────────────┐  │
│  │  LLM (Claude/GPT/MiniMax)                    │  │
│  │  Tool Registry  │  Context Builder            │  │
│  └──────────────────────────────────────────────┘  │
├─────────────────────────────────────────────────────┤
│  Tools: Audio, GPIO, I2C, Cron, Memory, Skills   │
├─────────────────────────────────────────────────────┤
│  Audio: Voice Manager │ ASR │ TTS │ MP3 Player    │
├─────────────────────────────────────────────────────┤
│  Skills: Lua Engine │ API Skills │ MCP Client      │
├─────────────────────────────────────────────────────┤
│  Storage: SPIFFS │ NVS │ Flash                    │
└─────────────────────────────────────────────────────┘
```

## Documentation

- **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** — System design and module map
- **[docs/TODO.md](docs/TODO.md)** — Roadmap and feature tracking

## License

MIT

## Acknowledgments

Inspired by [OpenClaw](https://github.com/openclaw/openclaw) and [Nanobot](https://github.com/HKUDS/nanobot).

## Star History

[![Star History Chart](https://api.star-history.com/svg?repos=yunze7373/mimiclaw&type=Date)](https://star-history.com/#yunze7373/mimiclaw&Date)
