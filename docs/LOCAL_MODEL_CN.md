# 本地模型配置指南

本文档说明如何配置 MimiClaw 使用本地 Ollama 服务器的模型。

## 支持的 LLM 提供商

| 提供商 | 配置值 | 说明 |
|--------|--------|------|
| Anthropic | `anthropic` | Claude API (api.anthropic.com) |
| OpenAI | `openai` | GPT API (api.openai.com) |
| MiniMax | `minimax` | MiniMax API (api.minimax.io) |
| MiniMax Coding | `minimax_coding` | MiniMax Coding Plan (api.minimaxi.com) |
| **Ollama** | **`ollama`** | 本地 Ollama 服务器 |

---

## 快速开始

### 1. 配置编译参数

编辑 `main/mimi_secrets.h`：

```c
// LLM 提供商选择
#define MIMI_SECRET_MODEL_PROVIDER  "ollama"

// 模型名称（使用 Ollama 服务器上的模型名）
#define MIMI_SECRET_MODEL           "minimax-m2.5:cloud"

// API Key（Ollama 可任意填写，如 "ollama-local"）
#define MIMI_SECRET_API_KEY         "ollama-local"

// Ollama 服务器地址
#define MIMI_SECRET_OLLAMA_HOST    "192.168.1.100"
#define MIMI_SECRET_OLLAMA_PORT    "11434"
```

### 2. 编译烧录

```bash
idf.py fullclean
idf.py build
idf.py -p COMX flash monitor
```

> 将 `COMX` 替换为你的设备 COM 端口号。

---

## 配置选项详解

### MIMI_SECRET_MODEL_PROVIDER

选择使用的 LLM 提供商：

- `anthropic` - Anthropic Claude API（默认）
- `openai` - OpenAI GPT API
- `minimax` - MiniMax API
- `minimax_coding` - MiniMax Coding Plan
- `ollama` - 本地 Ollama 服务器

### MIMI_SECRET_MODEL

Ollama 服务器上的模型名称。

**查看可用模型：**

```bash
curl http://192.168.1.100:11434/api/tags
```

**常用模型：**

| 模型名称 | 说明 |
|----------|------|
| `minimax-m2.5:cloud` | MiniMax M2.5 |
| `glm-5:cloud` | GLM-5 |
| `qwen3:8b` | Qwen3 8B |
| `qwen3:1.7b` | Qwen3 1.7B |
| `deepseek-r1:1.5b` | DeepSeek R1 1.5B |
| `qwen3-coder:480b-cloud` | Qwen3 Coder |
| `gpt-oss:120b-cloud` | GPT OSS |

### MIMI_SECRET_OLLAMA_HOST

Ollama 服务器的 IP 地址或主机名。

例如：`192.168.1.100` 或 `macmini.local`

### MIMI_SECRET_OLLAMA_PORT

Ollama 服务器的端口号，默认为 `11434`。

---

## 运行时配置（CLI）

如果已烧录固件，可以通过串口终端配置：

```
mimi> set_model_provider ollama
mimi> set_ollama_host 192.168.1.100
mimi> set_ollama_port 11434
mimi> set_model minimax-m2.5:cloud
```

### 常用 CLI 命令

| 命令 | 说明 | 示例 |
|------|------|------|
| `set_model_provider <provider>` | 设置 LLM 提供商 | `set_model_provider ollama` |
| `set_model <model>` | 设置模型名称 | `set_model qwen3:8b` |
| `set_api_key <key>` | 设置 API Key | `set_api_key ollama-local` |
| `set_ollama_host <host>` | 设置 Ollama 主机 | `set_ollama_host 192.168.1.100` |
| `set_ollama_port <port>` | 设置 Ollama 端口 | `set_ollama_port 11434` |
| `show_config` | 显示当前配置 | `show_config` |

---

## 故障排除

### 无法连接 Ollama 服务器

1. **检查网络连通性：**
   ```bash
   ping 192.168.1.100
   ```

2. **检查 Ollama 服务是否运行：**
   ```bash
   curl http://192.168.1.100:11434/
   # 应返回 "Ollama is running"
   ```

3. **检查模型是否已加载：**
   ```bash
   curl http://192.168.1.100:11434/api/tags
   ```

### 认证失败

Ollama 不需要认证，但某些配置可能需要设置 API Key：

```
mimi> set_api_key ollama-local
```

### 更换模型

只需修改模型名称即可切换模型：

```
mimi> set_model qwen3:8b
```

---

## 完整配置示例

### 示例 1：使用本地 Qwen3 8B 模型

```c
#define MIMI_SECRET_MODEL_PROVIDER  "ollama"
#define MIMI_SECRET_MODEL           "qwen3:8b"
#define MIMI_SECRET_API_KEY         "ollama-local"
#define MIMI_SECRET_OLLAMA_HOST    "192.168.1.100"
#define MIMI_SECRET_OLLAMA_PORT    "11434"
```

### 示例 2：使用 DeepSeek R1 模型

```c
#define MIMI_SECRET_MODEL_PROVIDER  "ollama"
#define MIMI_SECRET_MODEL           "deepseek-r1:1.5b"
#define MIMI_SECRET_API_KEY         "ollama-local"
#define MIMI_SECRET_OLLAMA_HOST    "192.168.1.100"
#define MIMI_SECRET_OLLAMA_PORT    "11434"
```

### 示例 3：使用 MiniMax Coding Plan

```c
#define MIMI_SECRET_MODEL_PROVIDER  "minimax_coding"
#define MIMI_SECRET_MODEL           "MiniMax-M2.5"
#define MIMI_SECRET_API_KEY         "你的MiniMax_API_Key"
```

---

## 相关文档

- [README_CN.md](../README_CN.md) - 基础使用说明
- [ARCHITECTURE.md](./ARCHITECTURE.md) - 架构设计
