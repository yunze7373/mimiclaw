#include "web_ui.h"
#include "../mimi_config.h"
#include "../llm/llm_proxy.h"
#include "../wifi/wifi_manager.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_timer.h"

static const char *TAG = "web_ui";

/* ── HTML Page ─────────────────────────────────────────────────── */

static const char *HTML_PAGE =
"<!DOCTYPE html>"
"<html>"
"<head>"
"  <meta charset='utf-8'>"
"  <meta name='viewport' content='width=device-width, initial-scale=1'>"
"  <title>MimiClaw 管理</title>"
"  <style>"
"    * { box-sizing: border-box; margin: 0; padding: 0; }"
"    body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; background: #f5f5f5; padding: 20px; }"
"    .container { max-width: 600px; margin: 0 auto; }"
"    h1 { color: #333; margin-bottom: 20px; }"
"    .card { background: white; border-radius: 8px; padding: 20px; margin-bottom: 20px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }"
"    .card h2 { font-size: 16px; color: #666; margin-bottom: 15px; border-bottom: 1px solid #eee; padding-bottom: 10px; }"
"    .form-group { margin-bottom: 15px; }"
"    .form-group label { display: block; font-size: 14px; color: #666; margin-bottom: 5px; }"
"    .form-group input, .form-group select { width: 100%; padding: 10px; border: 1px solid #ddd; border-radius: 4px; font-size: 14px; }"
"    .form-group input:focus, .form-group select:focus { outline: none; border-color: #007bff; }"
"    button { background: #007bff; color: white; border: none; padding: 12px 24px; border-radius: 4px; cursor: pointer; font-size: 14px; }"
"    button:hover { background: #0056b3; }"
"    button.danger { background: #dc3545; }"
"    button.danger:hover { background: #c82333; }"
"    .row { display: flex; gap: 10px; }"
"    .row .form-group { flex: 1; }"
"    .status { padding: 10px; background: #e7f3ff; border-radius: 4px; margin-bottom: 15px; font-size: 14px; }"
"    .status.error { background: #ffe7e7; }"
"    .status.success { background: #e7ffe7; }"
"    pre { background: #f8f8f8; padding: 10px; border-radius: 4px; overflow-x: auto; font-size: 12px; }"
"  </style>"
"</head>"
"<body>"
"  <div class='container'>"
"    <h1>MimiClaw 管理</h1>"
"    <p><a href='/chat'>进入聊天</a></p>"

"    <div class='card'>"
"      <h2>状态</h2>"
"      <div id='status'></div>"
"    </div>"

"    <div class='card'>"
"      <h2>LLM 配置</h2>"
"      <div class='form-group'>"
"        <label>提供商</label>"
"        <select id='provider'>"
"          <option value='anthropic'>Anthropic (Claude)</option>"
"          <option value='openai'>OpenAI (GPT)</option>"
"          <option value='minimax'>MiniMax</option>"
"          <option value='minimax_coding'>MiniMax Coding Plan</option>"
"          <option value='ollama'>Ollama (本地)</option>"
"        </select>"
"      </div>"
"      <div class='form-group'>"
"        <label>模型</label>"
"        <input type='text' id='model' placeholder='如: claude-opus-4-5 或 minimax-m2.5:cloud'>"
"      </div>"
"      <div class='form-group'>"
"        <label>API Key</label>"
"        <input type='text' id='api_key' placeholder='API Key'>"
"      </div>"
"      <div class='form-group' id='ollama_host_group' style='display:none'>"
"        <label>Ollama 主机</label>"
"        <input type='text' id='ollama_host' placeholder='如: 192.168.1.100'>"
"      </div>"
"      <div class='form-group' id='ollama_port_group' style='display:none'>"
"        <label>Ollama 端口</label>"
"        <input type='text' id='ollama_port' placeholder='默认: 11434'>"
"      </div>"
"      <button onclick='saveConfig()'>保存配置</button>"
"    </div>"

"    <div class='card'>"
"      <h2>操作</h2>"
"      <button class='danger' onclick='reboot()'>重启设备</button>"
"    </div>"

"  </div>"

"  <script>"
"    const providerSelect = document.getElementById('provider');"
"    providerSelect.addEventListener('change', function() {"
"      const isOllama = this.value === 'ollama';"
"      document.getElementById('ollama_host_group').style.display = isOllama ? 'block' : 'none';"
"      document.getElementById('ollama_port_group').style.display = isOllama ? 'block' : 'none';"
"    });"

"    async function loadConfig() {"
"      try {"
"        const resp = await fetch('/api/config');"
"        const data = await resp.json();"
"        document.getElementById('provider').value = data.provider || 'anthropic';"
"        document.getElementById('model').value = data.model || '';"
"        document.getElementById('api_key').value = data.api_key || '';"
"        document.getElementById('ollama_host').value = data.ollama_host || '';"
"        document.getElementById('ollama_port').value = data.ollama_port || '11434';"
"        providerSelect.dispatchEvent(new Event('change'));"
"      } catch(e) { showStatus('加载配置失败', 'error'); }"
"    }"

"    async function loadStatus() {"
"      try {"
"        const resp = await fetch('/api/status');"
"        const data = await resp.json();"
"        document.getElementById('status').innerHTML = '<pre>' + JSON.stringify(data, null, 2) + '</pre>';"
"      } catch(e) { document.getElementById('status').innerText = '获取状态失败'; }"
"    }"

"    async function saveConfig() {"
"      const config = {"
"        provider: document.getElementById('provider').value,"
"        model: document.getElementById('model').value,"
"        api_key: document.getElementById('api_key').value,"
"        ollama_host: document.getElementById('ollama_host').value,"
"        ollama_port: document.getElementById('ollama_port').value"
"      };"
"      try {"
"        const resp = await fetch('/api/config', {"
"          method: 'POST',"
"          headers: {'Content-Type': 'application/json'},"
"          body: JSON.stringify(config)"
"        });"
"        if (resp.ok) { showStatus('配置已保存', 'success'); }"
"        else { showStatus('保存失败', 'error'); }"
"      } catch(e) { showStatus('保存失败: ' + e, 'error'); }"
"    }"

"    async function reboot() {"
"      if (!confirm('确定要重启设备吗？')) return;"
"      try {"
"        await fetch('/api/reboot', {method: 'POST'});"
"        showStatus('正在重启...', 'success');"
"      } catch(e) { showStatus('重启失败', 'error'); }"
"    }"

"    function showStatus(msg, type) {"
"      document.getElementById('status').innerHTML = '<div class=\"status ' + type + '\">' + msg + '</div>';"
"    }"

"    loadConfig();"
"    loadStatus();"
"  </script>"
"</body>"
"</html>";

/* ── Chat Page ──────────────────────────────────────────────────── */

static const char *CHAT_PAGE =
"<!DOCTYPE html>"
"<html>"
"<head>"
"  <meta charset='utf-8'>"
"  <meta name='viewport' content='width=device-width, initial-scale=1'>"
"  <title>MimiClaw 聊天</title>"
"  <style>"
"    * { box-sizing: border-box; margin: 0; padding: 0; }"
"    body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; background: #f5f5f5; height: 100vh; display: flex; flex-direction: column; }"
"    .header { background: #fff; padding: 12px 20px; border-bottom: 1px solid #ddd; display: flex; justify-content: space-between; align-items: center; }"
"    .header h1 { font-size: 18px; color: #333; }"
"    .header a { color: #007bff; text-decoration: none; font-size: 14px; }"
"    .chat-container { flex: 1; overflow-y: auto; padding: 20px; max-width: 800px; margin: 0 auto; width: 100%; }"
"    .message { margin-bottom: 16px; padding: 12px 16px; border-radius: 12px; max-width: 80%; }"
"    .message.user { background: #007bff; color: white; margin-left: auto; }"
"    .message.assistant { background: white; color: #333; }"
"    .message.error { background: #ffe7e7; color: #dc3545; }"
"    .message .time { font-size: 11px; opacity: 0.7; margin-top: 4px; }"
"    .input-area { background: white; padding: 16px; border-top: 1px solid #ddd; }"
"    .input-row { display: flex; gap: 10px; align-items: flex-end; max-width: 800px; margin: 0 auto; }"
"    .input-row select { padding: 10px; border: 1px solid #ddd; border-radius: 4px; font-size: 14px; min-width: 150px; }"
"    .input-row input { flex: 1; padding: 12px; border: 1px solid #ddd; border-radius: 24px; font-size: 14px; }"
"    .input-row input:focus { outline: none; border-color: #007bff; }"
"    .input-row button { padding: 12px 24px; background: #007bff; color: white; border: none; border-radius: 24px; cursor: pointer; font-size: 14px; }"
"    .input-row button:hover { background: #0056b3; }"
"    .input-row button:disabled { background: #ccc; cursor: not-allowed; }"
"    .model-select { padding: 10px; border: 1px solid #ddd; border-radius: 4px; font-size: 14px; min-width: 180px; background: white; }"
"    .connecting { text-align: center; padding: 20px; color: #666; }"
"  </style>"
"</head>"
"<body>"
"  <div class='header'>"
"    <h1>MimiClaw 聊天</h1>"
"    <a href='/'>管理</a>"
"  </div>"
"  <div class='chat-container' id='chat'></div>"
"  <div class='input-area'>"
"    <div class='input-row'>"
"      <select class='model-select' id='modelSelect'>"
"        <option value=''>默认模型</option>"
"        <option value='claude-opus-4-5'>Claude Opus 4.5</option>"
"        <option value='claude-sonnet-4-5'>Claude Sonnet 4.5</option>"
"        <option value='claude-haiku-3-5'>Claude Haiku 3.5</option>"
"        <option value='gpt-4o'>GPT-4o</option>"
"        <option value='gpt-4o-mini'>GPT-4o Mini</option>"
"        <option value='gpt-4-turbo'>GPT-4 Turbo</option>"
"        <option value='miniMax-Realtime'>MiniMax Realtime</option>"
"        <option value='miniMax-M2.5'>MiniMax M2.5</option>"
"        <option value='ollama:llama3'>Ollama Llama3</option>"
"        <option value='ollama:qwen2.5'>Ollama Qwen2.5</option>"
"        <option value='ollama:mistral'>Ollama Mistral</option>"
"      </select>"
"      <input type='text' id='input' placeholder='发送消息...' onkeypress='handleKey(event)'>"
"      <button onclick='send()' id='sendBtn'>发送</button>"
"    </div>"
"  </div>"
"  <script>"
"    let ws = null;"
"    const chat = document.getElementById('chat');"
"    const input = document.getElementById('input');"
"    const sendBtn = document.getElementById('sendBtn');"
"    const modelSelect = document.getElementById('modelSelect');"
"    let myChatId = 'web_' + Math.random().toString(36).substr(2, 9);"
"    let connected = false;"
"    let waiting = false;"
"    "
"    function connect() {"
"      const protocol = location.protocol === 'https:' ? 'wss:' : 'ws:';"
"      const wsUrl = protocol + '//' + location.hostname + ':18789';"
"      ws = new WebSocket(wsUrl);"
"      "
"      ws.onopen = function() {"
"        connected = true;"
"        addMessage('system', '已连接到设备');"
"      };"
"      "
"      ws.onmessage = function(event) {"
"        try {"
"          const data = JSON.parse(event.data);"
"          if (data.type === 'response' && data.chat_id === myChatId) {"
"            addMessage('assistant', data.content);"
"            waiting = false;"
"            sendBtn.disabled = false;"
"            sendBtn.textContent = '发送';"
"          }"
"        } catch(e) {}"
"      };"
"      "
"      ws.onclose = function() {"
"        connected = false;"
"        addMessage('error', '连接断开，3秒后重连...');"
"        setTimeout(connect, 3000);"
"      };"
"      "
"      ws.onerror = function() {"
"        addMessage('error', '连接错误');"
"      };"
"    }"
"    "
"    function addMessage(role, content) {"
"      const div = document.createElement('div');"
"      div.className = 'message ' + role;"
"      div.innerHTML = content.replace(/\\n/g, '<br>');"
"      chat.appendChild(div);"
"      chat.scrollTop = chat.scrollHeight;"
"    }"
"    "
"    function send() {"
"      if (!connected) { addMessage('error', '未连接到设备'); return; }"
"      const msg = input.value.trim();"
"      if (!msg || waiting) return;"
"      "
"      addMessage('user', msg);"
"      input.value = '';"
"      waiting = true;"
"      sendBtn.disabled = true;"
"      sendBtn.textContent = '思考中...';"
"      "
"      const model = modelSelect.value;"
"      let payload = {type: 'message', content: msg, chat_id: myChatId};"
"      if (model) {"
"        payload.model = model;"
"      }"
"      ws.send(JSON.stringify(payload));"
"    }"
"    "
"    function handleKey(e) {"
"      if (e.key === 'Enter' && !e.shiftKey) {"
"        e.preventDefault();"
"        send();"
"      }"
"    }"
"    "
"    connect();"
"  </script>"
"</body>"
"</html>";

/* ── HTTP Handlers ─────────────────────────────────────────────── */

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, HTML_PAGE, strlen(HTML_PAGE));
    return ESP_OK;
}

static esp_err_t chat_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, CHAT_PAGE, strlen(CHAT_PAGE));
    return ESP_OK;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    char buf[512];
    int len = 0;

    const char *ip = wifi_manager_get_ip();
    const char *provider = llm_get_provider();
    const char *model = llm_get_model();
    int64_t uptime_ms = esp_timer_get_time() / 1000;

    len += snprintf(buf + len, sizeof(buf) - len, "{\n");
    len += snprintf(buf + len, sizeof(buf) - len, "  \"wifi_ip\": \"%s\",\n", ip ? ip : "disconnected");
    len += snprintf(buf + len, sizeof(buf) - len, "  \"provider\": \"%s\",\n", provider);
    len += snprintf(buf + len, sizeof(buf) - len, "  \"model\": \"%s\",\n", model);
    len += snprintf(buf + len, sizeof(buf) - len, "  \"uptime_ms\": %lld\n", uptime_ms);
    len += snprintf(buf + len, sizeof(buf) - len, "}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, strlen(buf));
    return ESP_OK;
}

static esp_err_t favicon_handler(httpd_req_t *req)
{
    httpd_resp_send(req, "", 0);
    return ESP_OK;
}

static esp_err_t config_get_handler(httpd_req_t *req)
{
    // TODO: read from NVS
    const char *json = "{}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    return ESP_OK;
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    char buf[512];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    // Simple parse - look for key fields
    char *p = buf;

    // Extract provider
    char *provider_start = strstr(p, "\"provider\"");
    if (provider_start) {
        char *colon = strchr(provider_start, ':');
        char *quote = strchr(colon + 1, '\"');
        char *end_quote = strchr(quote + 1, '\"');
        if (colon && quote && end_quote) {
            *end_quote = '\0';
            llm_set_provider(quote + 1);
        }
    }

    // Extract model
    char *model_start = strstr(p, "\"model\"");
    if (model_start) {
        char *colon = strchr(model_start, ':');
        char *quote = strchr(colon + 1, '\"');
        char *end_quote = strchr(quote + 1, '\"');
        if (colon && quote && end_quote) {
            *end_quote = '\0';
            llm_set_model(quote + 1);
        }
    }

    // Extract api_key
    char *key_start = strstr(p, "\"api_key\"");
    if (key_start) {
        char *colon = strchr(key_start, ':');
        char *quote = strchr(colon + 1, '\"');
        char *end_quote = strchr(quote + 1, '\"');
        if (colon && quote && end_quote) {
            *end_quote = '\0';
            llm_set_api_key(quote + 1);
        }
    }

    // Extract ollama_host
    char *host_start = strstr(p, "\"ollama_host\"");
    if (host_start) {
        char *colon = strchr(host_start, ':');
        char *quote = strchr(colon + 1, '\"');
        char *end_quote = strchr(quote + 1, '\"');
        if (colon && quote && end_quote) {
            *end_quote = '\0';
            llm_set_ollama_host(quote + 1);
        }
    }

    // Extract ollama_port
    char *port_start = strstr(p, "\"ollama_port\"");
    if (port_start) {
        char *colon = strchr(port_start, ':');
        char *quote = strchr(colon + 1, '\"');
        char *end_quote = strchr(quote + 1, '\"');
        if (colon && quote && end_quote) {
            *end_quote = '\0';
            llm_set_ollama_port(quote + 1);
        }
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":true}", 16);
    return ESP_OK;
}

static esp_err_t reboot_handler(httpd_req_t *req)
{
    httpd_resp_send(req, "{\"rebooting\":true}", 18);
    vTaskDelay(500 / portTICK_PERIOD_MS);
    esp_restart();
    return ESP_OK;
}

/* ── Server Init ───────────────────────────────────────────────── */

esp_err_t web_ui_init(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;

    httpd_handle_t server = NULL;
    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ret;
    }

    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
    };
    httpd_register_uri_handler(server, &index_uri);

    httpd_uri_t chat_uri = {
        .uri = "/chat",
        .method = HTTP_GET,
        .handler = chat_handler,
    };
    httpd_register_uri_handler(server, &chat_uri);

    httpd_uri_t favicon_uri = {
        .uri = "/favicon.ico",
        .method = HTTP_GET,
        .handler = favicon_handler,
    };
    httpd_register_uri_handler(server, &favicon_uri);

    httpd_uri_t api_status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_handler,
    };
    httpd_register_uri_handler(server, &api_status_uri);

    httpd_uri_t api_config_get_uri = {
        .uri = "/api/config",
        .method = HTTP_GET,
        .handler = config_get_handler,
    };
    httpd_register_uri_handler(server, &api_config_get_uri);

    httpd_uri_t api_config_post_uri = {
        .uri = "/api/config",
        .method = HTTP_POST,
        .handler = config_post_handler,
    };
    httpd_register_uri_handler(server, &api_config_post_uri);

    httpd_uri_t api_reboot_uri = {
        .uri = "/api/reboot",
        .method = HTTP_POST,
        .handler = reboot_handler,
    };
    httpd_register_uri_handler(server, &api_reboot_uri);

    ESP_LOGI(TAG, "Web UI started on port 80");
    return ESP_OK;
}

esp_err_t web_ui_stop(void)
{
    // TODO: implement stop
    return ESP_OK;
}
