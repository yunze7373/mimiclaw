#include "web_ui.h"
#include "../mimi_config.h"
#include "../llm/llm_proxy.h"
#include "../wifi/wifi_manager.h"
#include "../tools/tool_web_search.h"
#include "../cron/cron_service.h"
#include "nvs.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "tools/tool_hardware.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

static const char *TAG = "web_ui";

/* WebSocket port - should match gateway/ws_server.c WS_PORT */
#ifndef WS_PORT
#define WS_PORT 18789
#endif

/* Helper macro for stringification (two-level for macro expansion) */
#define STRINGIFY_HELPER(x) #x
#define STRINGIFY(x) STRINGIFY_HELPER(x)

/* ── SPA HTML Page ───────────────────────────────────────────────── */

static const char *HTML_PAGE =
"<!DOCTYPE html>"
"<html>"
"<head>"
"  <meta charset='utf-8'>"
"  <meta name='viewport' content='width=device-width, initial-scale=1'>"
"  <title>MimiClaw</title>"
"  <link rel='icon' href='data:image/svg+xml,<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 100 100\"><text y=\".9em\" font-size=\"90\">🦊</text></svg>'>"
"  <style>"
"    :root {"
"      --primary: #6366f1; --primary-dark: #4f46e5;"
"      --bg: #f8fafc; --surface: #ffffff;"
"      --text: #1e293b; --text-secondary: #64748b;"
"      --border: #e2e8f0; --success: #22c55e;"
"      --error: #ef4444; --warning: #f59e0b;"
"    }"
"    * { box-sizing: border-box; margin: 0; padding: 0; }"
"    body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; background: var(--bg); color: var(--text); height: 100vh; display: flex; }"
"    /* Sidebar */"
"    .sidebar { width: 220px; background: var(--surface); border-right: 1px solid var(--border); display: flex; flex-direction: column; }"
"    .logo { padding: 20px; font-size: 20px; font-weight: 700; color: var(--primary); border-bottom: 1px solid var(--border); display: flex; align-items: center; gap: 8px; }"
"    .logo-icon { font-size: 24px; }"
"    .nav { flex: 1; padding: 12px; }"
"    .nav-item { display: flex; align-items: center; gap: 10px; padding: 12px 14px; border-radius: 8px; color: var(--text-secondary); cursor: pointer; transition: all 0.2s; margin-bottom: 4px; }"
"    .nav-item:hover { background: var(--bg); color: var(--text); }"
"    .nav-item.active { background: var(--primary); color: white; }"
"    .nav-icon { font-size: 18px; width: 24px; text-align: center; }"
"    .nav-label { font-size: 14px; font-weight: 500; }"
"    .sidebar-footer { padding: 16px; border-top: 1px solid var(--border); }"
"    .ws-status { display: flex; align-items: center; gap: 8px; font-size: 12px; color: var(--text-secondary); }"
"    .ws-dot { width: 8px; height: 8px; border-radius: 50%; background: var(--error); }"
"    .ws-dot.connected { background: var(--success); }"
"    /* Main Content */"
"    .main { flex: 1; overflow-y: auto; }"
"    .header { background: var(--surface); border-bottom: 1px solid var(--border); padding: 16px 24px; display: flex; justify-content: space-between; align-items: center; }"
"    .header h1 { font-size: 18px; font-weight: 600; }"
"    .header-right { display: flex; align-items: center; gap: 16px; }"
"    .ip-badge { background: var(--bg); padding: 6px 12px; border-radius: 6px; font-size: 13px; color: var(--text-secondary); }"
"    .content { padding: 24px; }"
"    /* Cards */"
"    .card { background: var(--surface); border-radius: 12px; padding: 20px; margin-bottom: 16px; box-shadow: 0 1px 3px rgba(0,0,0,0.05); }"
"    .card-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 16px; }"
"    .card-title { font-size: 16px; font-weight: 600; }"
"    /* Forms */"
"    .form-row { display: flex; gap: 16px; margin-bottom: 16px; }"
"    .form-group { flex: 1; }"
"    .form-group label { display: block; font-size: 13px; color: var(--text-secondary); margin-bottom: 6px; }"
"    .form-group input, .form-group select { width: 100%; padding: 10px 12px; border: 1px solid var(--border); border-radius: 8px; font-size: 14px; transition: border-color 0.2s; }"
"    .form-group input:focus, .form-group select:focus { outline: none; border-color: var(--primary); }"
"    /* Buttons */"
"    .btn { padding: 10px 20px; border-radius: 8px; font-size: 14px; font-weight: 500; cursor: pointer; border: none; transition: all 0.2s; }"
"    .btn-primary { background: var(--primary); color: white; }"
"    .btn-primary:hover { background: var(--primary-dark); }"
"    .btn-danger { background: var(--error); color: white; }"
"    .btn-danger:hover { background: #dc2626; }"
"    .btn-sm { padding: 6px 12px; font-size: 12px; }"
"    /* Status Grid */"
"    .status-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 12px; }"
"    .status-item { background: var(--bg); padding: 14px; border-radius: 8px; }"
"    .status-label { font-size: 12px; color: var(--text-secondary); margin-bottom: 4px; }"
"    .status-value { font-size: 15px; font-weight: 600; }"
"    /* Chat */"
"    .chat-container { height: calc(100vh - 140px); display: flex; flex-direction: column; }"
"    .chat-messages { flex: 1; overflow-y: auto; padding: 16px; background: var(--bg); border-radius: 12px; margin-bottom: 16px; }"
"    .chat-message { max-width: 80%; margin-bottom: 16px; padding: 12px 16px; border-radius: 16px; }"
"    .chat-message.user { background: var(--primary); color: white; margin-left: auto; border-bottom-right-radius: 4px; }"
"    .chat-message.assistant { background: var(--surface); border: 1px solid var(--border); border-bottom-left-radius: 4px; }"
"    .chat-message.error { background: #fef2f2; color: var(--error); border: 1px solid #fecaca; }"
"    .chat-message .time { font-size: 11px; opacity: 0.7; margin-top: 6px; }\n"
"    .typing-indicator { display: flex; gap: 4px; padding: 6px 4px; }\n"
"    .typing-dot { width: 6px; height: 6px; background: #94a3b8; border-radius: 50%; animation: typing 1.4s infinite ease-in-out both; }\n"
"    .typing-dot:nth-child(1) { animation-delay: -0.32s; }\n"
"    .typing-dot:nth-child(2) { animation-delay: -0.16s; }\n"
"    @keyframes typing { 0%, 80%, 100% { transform: scale(0); } 40% { transform: scale(1); } }\n"
"    .chat-input-row { display: flex; gap: 12px; align-items: center; }"
"    .chat-input-row select { padding: 12px; border: 1px solid var(--border); border-radius: 8px; font-size: 14px; min-width: 160px; }"
"    .chat-input-row input { flex: 1; padding: 12px 16px; border: 1px solid var(--border); border-radius: 24px; font-size: 14px; }"
"    .chat-input-row input:focus { outline: none; border-color: var(--primary); }"
"    .chat-input-row button { padding: 12px 24px; background: var(--primary); color: white; border: none; border-radius: 24px; cursor: pointer; font-size: 14px; font-weight: 500; }"
"    .chat-input-row button:hover { background: var(--primary-dark); }"
"    .chat-input-row button:disabled { background: #94a3b8; cursor: not-allowed; }"
"    /* Toast */"
"    .toast { position: fixed; top: 20px; right: 20px; padding: 12px 20px; border-radius: 8px; font-size: 14px; z-index: 1000; animation: slideIn 0.3s ease; }"
"    .toast.success { background: var(--success); color: white; }"
"    .toast.error { background: var(--error); color: white; }"
"    .toast.warning { background: var(--warning); color: white; }"
"    @keyframes slideIn { from { transform: translateX(100%); opacity: 0; } to { transform: translateX(0); opacity: 1; } }"
"    /* Views */"
"    .view { display: none; }"
"    .view.active { display: block; }"
"    /* Board Layout */"
"    /* Board Layout (Horizontal) */"
"    .board-layout { display: flex; flex-direction: column; gap: 16px; }"
"    .board-row { display: flex; flex-wrap: wrap; gap: 6px; justify-content: flex-start; background: #fff; padding: 10px; border-radius: 8px; border: 1px solid #e2e8f0; }"
"    .board-row h4 { width: 100%; margin: 0 0 8px 0; font-size: 13px; color: #64748b; border-bottom: 1px solid #f1f5f9; padding-bottom: 4px; }"
"    .pin-card { display: flex; flex-direction: column; align-items: center; width: 64px; padding: 6px 4px; background: #f8fafc; border: 1px solid #cbd5e1; border-radius: 6px; }"
"    .pin-card.restricted { opacity: 0.6; background: #f1f5f9; border-color: #e2e8f0; }"
"    .pin-card.label-only { background: transparent; border: 1px dashed #cbd5e1; }"
"    .pin-lbl { font-family: monospace; font-size: 12px; font-weight: bold; color: #334155; margin-bottom: 4px; }"
"    .btn-group-v { display: flex; flex-direction: column; gap: 2px; width: 100%; }"
"    .btn-xs { padding: 2px 0; font-size: 10px; width: 100%; text-align: center; }"
"    .badge-warn { font-size: 9px; color: #b45309; background: #fef3c7; padding: 2px 4px; border-radius: 3px; width: 100%; text-align: center; border: 1px solid #fcd34d; }"
"  </style>"
"</head>"
"<body>"
"  <!-- Sidebar -->"
"  <div class='sidebar'>"
"    <div class='logo'>"
"      <span class='logo-icon'>🦊</span>"
"      <span>MimiClaw</span>"
"    </div>"
"    <div class='nav'>"
"      <div class='nav-item active' data-view='dashboard'>"
"        <span class='nav-icon'>📊</span>"
"        <span class='nav-label'>仪表盘</span>"
"      </div>"
"      <div class='nav-item' data-view='chat'>"
"        <span class='nav-icon'>💬</span>"
"        <span class='nav-label'>聊天</span>"
"      </div>"
"      <div class='nav-item' data-view='agent'>"
"        <span class='nav-icon'>🤖</span>"
"        <span class='nav-label'>Agent</span>"
"      </div>"
"      <div class='nav-item' data-view='hardware'>"
"        <span class='nav-icon'>🔌</span>"
"        <span class='nav-label'>硬件</span>"
"      </div>"
"      <div class='nav-item' data-view='settings'>"
"        <span class='nav-icon'>⚙️</span>"
"        <span class='nav-label'>设置</span>"
"      </div>"
"      <div class='nav-item' data-view='tools'>"
"        <span class='nav-icon'>🔧</span>"
"        <span class='nav-label'>工具</span>"
"      </div>"
"    </div>"
"    <div class='sidebar-footer'>"
"      <div class='ws-status'>"
"        <div class='ws-dot' id='wsDot'></div>"
"        <span id='wsText'>未连接</span>"
"      </div>"
"    </div>"
"  </div>"
""
"  <!-- Main Content -->"
"  <div class='main'>"
"    <div class='header'>"
"      <h1 id='pageTitle'>仪表盘</h1>"
"      <div class='header-right'>"
"        <span class='ip-badge' id='ipBadge'>获取IP...</span>"
"      </div>"
"    </div>"
""
"    <!-- Dashboard View -->"
"    <div class='view active' id='view-dashboard'>"
"      <div class='content'>"
"        <div class='card'>"
"          <div class='card-header'>"
"            <span class='card-title'>系统状态</span>"
"            <button class='btn btn-sm btn-primary' onclick='refreshStatus()'>刷新</button>"
"          </div>"
"          <div class='status-grid' id='statusGrid'></div>"
"        </div>"
""
"        <div class='card'>"
"          <div class='card-header'>"
"            <span class='card-title'>快速操作</span>"
"          </div>"
"          <div class='form-row'>"
"            <button class='btn btn-primary' onclick='switchView(\"chat\")'>进入聊天</button>"
"            <button class='btn btn-danger' onclick='reboot()'>重启设备</button>"
"          </div>"
"        </div>"
"      </div>"
"    </div>"
""
"    <!-- Chat View -->"
"    <div class='view' id='view-chat'>"
"      <div class='content'>"
"        <div class='chat-container'>"
"          <div class='chat-messages' id='chatMessages'></div>"
"          <div class='chat-input-row'>"
"            <select id='modelSelect'>"
"              <option value=''>默认模型</option>"
"              <option value='claude-opus-4-5'>Claude Opus 4.5</option>"
"              <option value='claude-sonnet-4-5'>Claude Sonnet 4.5</option>"
"              <option value='claude-haiku-3-5'>Claude Haude 3.5</option>"
"              <option value='gpt-4o'>GPT-4o</option>"
"              <option value='gpt-4o-mini'>GPT-4o Mini</option>"
"              <option value='miniMax-Realtime'>MiniMax Realtime</option>"
"              <option value='miniMax-M2.5'>MiniMax M2.5</option>"
"              <option value='ollama:llama3'>Ollama Llama3</option>"
"              <option value='ollama:qwen2.5'>Ollama Qwen2.5</option>"
"            </select>"
"            <input type='text' id='chatInput' placeholder='发送消息...' onkeypress='handleChatKey(event)'>"
"            <button onclick='sendChat()' id='sendBtn'>发送</button>"
"          </div>"
"        </div>"
"      </div>"
"    </div>"
""
"    <!-- Agent View -->"
"    <div class='view' id='view-agent'>"
"      <div class='content'>"
"        <div class='card'>"
"          <div class='card-header'>"
"            <span class='card-title'>Agent 配置</span>"
"            <button class='btn btn-sm btn-primary' onclick='saveAgent()'>保存</button>"
"          </div>"
"          <div class='form-group'>"
"            <label>SOUL.md (性格设定)</label>"
"            <textarea id='agentSoul' rows='6' style='width:100%;font-family:monospace;font-size:13px;padding:8px;border:1px solid #333;border-radius:6px;background:#1a1a2e;color:#e0e0e0;resize:vertical'></textarea>"
"          </div>"
"          <div class='form-group'>"
"            <label>USER.md (用户信息)</label>"
"            <textarea id='agentUser' rows='6' style='width:100%;font-family:monospace;font-size:13px;padding:8px;border:1px solid #333;border-radius:6px;background:#1a1a2e;color:#e0e0e0;resize:vertical'></textarea>"
"          </div>"
"          <div class='form-group'>"
"            <label>MEMORY.md (长期记忆)</label>"
"            <textarea id='agentMemory' rows='6' style='width:100%;font-family:monospace;font-size:13px;padding:8px;border:1px solid #333;border-radius:6px;background:#1a1a2e;color:#e0e0e0;resize:vertical'></textarea>"
"          </div>"
"          <div class='form-group'>"
"            <label>HEARTBEAT.md (定时任务)</label>"
"            <textarea id='agentHeartbeat' rows='6' style='width:100%;font-family:monospace;font-size:13px;padding:8px;border:1px solid #333;border-radius:6px;background:#1a1a2e;color:#e0e0e0;resize:vertical'></textarea>"
"          </div>"
"        </div>"
"      </div>"
"    </div>"
""
"    <!-- Settings View -->"
"    <div class='view' id='view-settings'>"
"      <div class='content'>"
"        <div class='card'>"
"          <div class='card-header'>"
"            <span class='card-title'>LLM 配置</span>"
"            <button class='btn btn-sm btn-primary' onclick='saveSettings()'>保存</button>"
"          </div>"
"          <div class='form-row'>"
"            <div class='form-group'>"
"              <label>提供商</label>"
"              <select id='provider'>"
"                <option value='anthropic'>Anthropic (Claude)</option>"
"                <option value='openai'>OpenAI (GPT)</option>"
"                <option value='minimax'>MiniMax</option>"
"                <option value='minimax_coding'>MiniMax Coding</option>"
"                <option value='ollama'>Ollama (本地)</option>"
"              </select>"
"            </div>"
"            <div class='form-group'>"
"              <label>默认模型</label>"
"              <input type='text' id='model' placeholder='如: claude-opus-4-5'>"
"            </div>"
"          </div>"
"          <div class='form-row'>"
"            <div class='form-group'>"
"              <label>API Key</label>"
"              <input type='password' id='api_key' placeholder='API Key'>"
"            </div>"
"          </div>"
"          <div class='form-row' id='ollamaFields' style='display:none'>"
"            <div class='form-group'>"
"              <label>Ollama 主机</label>"
"              <input type='text' id='ollama_host' placeholder='如: 192.168.1.100'>"
"            </div>"
"            <div class='form-group'>"
"              <label>Ollama 端口</label>"
"              <input type='text' id='ollama_port' placeholder='默认: 11434'>"
"            </div>"
"          </div>"
"        </div>"
""
"        <div class='card'>"
"          <div class='card-header'>"
"            <span class='card-title'>设备操作</span>"
"          </div>"
"          <button class='btn btn-danger' onclick='reboot()'>重启设备</button>"
"        </div>"
"      </div>"
"    </div>"
""
"    <!-- Tools View -->"
"    <div class='view' id='view-tools'>"
"      <div class='content'>"
""
"        <div class='card'>"
"          <div class='card-header'>"
"            <span class='card-title'>🔍 网络搜索 (Brave)</span>"
"            <button class='btn btn-sm btn-primary' onclick='saveSearchKey()'>保存</button>"
"          </div>"
"          <div class='form-group'>"
"            <label>Brave Search API Key</label>"
"            <input type='password' id='searchKey' placeholder='BSA-xxxx...'>"
"          </div>"
"          <div style='font-size:12px;color:#888;margin-top:4px'>从 <a href='https://brave.com/search/api/' style='color:#6C9BD2' target='_blank'>brave.com/search/api</a> 获取免费 API Key</div>"
"        </div>"
""
"        <div class='card'>"
"          <div class='card-header'>"
"            <span class='card-title'>⏰ 定时任务</span>"
"            <button class='btn btn-sm btn-primary' onclick='loadCronJobs()'>刷新</button>"
"          </div>"
"          <div id='cronList' style='font-size:13px;color:#ccc'>加载中...</div>"
"        </div>"
""
"        <div class='card'>"
"          <div class='card-header'>"
"            <span class='card-title'>工具状态</span>"
"          </div>"
"          <div style='font-size:13px;color:#ccc;line-height:2'>"
"            <div>📅 <b>获取时间</b>：通过 SNTP 自动同步，无需配置</div>"
"            <div>📁 <b>文件管理</b>：读 / 写 / 编辑 / 列出 SPIFFS 文件</div>"
"          </div>"
"        </div>"
""
"      </div>"
"    </div>"
""
"    <!-- Hardware View -->"
"    <div class='view' id='view-hardware'>"
"      <div class='content'>"
"        <div class='card'>"
"          <div class='card-header'>"
"            <span class='card-title'>硬件状态</span>"
"            <button class='btn btn-sm btn-primary' onclick='loadHardwareStatus()'>刷新</button>"
"          </div>"
"          <div id='hw-status' style='font-size:13px;color:#ccc'>加载中...</div>"
"        </div>"
""
"        <div class='card'>"
"          <div class='card-header'>"
"            <span class='card-title'>I2C 设备扫描</span>"
"            <button class='btn btn-sm btn-primary' onclick='scanI2C()'>扫描</button>"
"          </div>"
"          <div id='i2c-result' style='font-size:13px;color:#ccc'>点击扫描...</div>"
"        </div>"
""
"        <div class='card'>"
"          <div class='card-header'>"
"            <span class='card-title'>GPIO 控制</span>"
"          </div>"
"          <div id='gpio-grid' style='display:grid;grid-template-columns:repeat(auto-fit, minmax(150px, 1fr));gap:8px;font-size:13px;color:#ccc'></div>"
"        </div>"
"      </div>"
"    </div>"
""
"  </div>"
""
"  <script>\n"
"    const WS_PORT = 18789;\n"
"    let ws = null;\n"
"    let myChatId = 'web_' + Math.random().toString(36).substr(2, 9);\n"
"    let connected = false;\n"
"    let pending = 0;\n"
"    let pendingTimer = null;\n"
"    let currentStreamDiv = null;\n"
"\n"
"    /* Navigation */\n"
"    function switchView(view) {\n"
"      document.querySelectorAll('.view').forEach(v => v.classList.remove('active'));\n"
"      document.querySelectorAll('.nav-item').forEach(n => n.classList.remove('active'));\n"
"      document.getElementById('view-' + view).classList.add('active');\n"
"      document.querySelector('[data-view=' + view + ']').classList.add('active');\n"
"      const titles = { dashboard: '仪表盘', chat: '聊天', agent: 'Agent', settings: '设置', tools: '工具', hardware: '硬件' };\n"
"      document.getElementById('pageTitle').textContent = titles[view] || view;\n"
"    }\n"
"\n"
"    document.querySelectorAll('.nav-item').forEach(item => {\n"
"      item.addEventListener('click', () => switchView(item.dataset.view));\n"
"    });\n"
"\n"
"    /* Toast */\n"
"    function showToast(msg, type) {\n"
"      const toast = document.createElement('div');\n"
"      toast.className = 'toast ' + type;\n"
"      toast.textContent = msg;\n"
"      document.body.appendChild(toast);\n"
"      setTimeout(() => toast.remove(), 3000);\n"
"    }\n"
""
"    /* Status */"
"    async function refreshStatus() {"
"      try {"
"        const resp = await fetch('/api/status');"
"        const data = await resp.json();"
"        const grid = document.getElementById('statusGrid');"
"        grid.innerHTML = '';"
"        const items = ["
"          { label: 'WiFi IP', value: data.wifi_ip || '未连接' },"
"          { label: 'LLM 提供商', value: data.provider || '未知' },"
"          { label: '模型', value: data.model || '未设置' },"
"          { label: '运行时间', value: formatUptime(data.uptime_ms) },"
"        ];"
"        items.forEach(item => {"
"          grid.innerHTML += '<div class=\\'status-item\\'><div class=\\'status-label\\'>' + item.label + '</div><div class=\\'status-value\\'>' + item.value + '</div></div>';"
"        });"
"        document.getElementById('ipBadge').textContent = data.wifi_ip || '无网络';"
"      } catch(e) { showToast('获取状态失败', 'error'); }"
"    }"
""
"    function formatUptime(ms) {"
"      if (!ms) return '0秒';"
"      const s = Math.floor(ms / 1000);"
"      const m = Math.floor(s / 60);"
"      const h = Math.floor(m / 60);"
"      const d = Math.floor(h / 24);"
"      if (h > 0) return h + '小时 ' + (m % 60) + '分钟';"
"      if (m > 0) return m + '分钟 ' + (s % 60) + '秒';"
"      return s + '秒';"
"    }"
""
"    /* Settings */"
"    async function loadSettings() {"
"      try {"
"        const resp = await fetch('/api/config');"
"        const data = await resp.json();"
"        document.getElementById('provider').value = data.provider || 'anthropic';"
"        document.getElementById('model').value = data.model || '';"
"        document.getElementById('api_key').value = data.api_key || '';"
"        document.getElementById('ollama_host').value = data.ollama_host || '';"
"        document.getElementById('ollama_port').value = data.ollama_port || '11434';"
"        updateOllamaFields();"
"      } catch(e) { console.error(e); }"
"    }"
""
"    document.getElementById('provider').addEventListener('change', updateOllamaFields);"
"    function updateOllamaFields() {"
"      const isOllama = document.getElementById('provider').value === 'ollama';"
"      document.getElementById('ollamaFields').style.display = isOllama ? 'flex' : 'none';"
"    }"
""
"    async function saveSettings() {"
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
"        if (resp.ok) { showToast('配置已保存', 'success'); }"
"        else { showToast('保存失败', 'error'); }"
"      } catch(e) { showToast('保存失败: ' + e, 'error'); }"
"    }"
""
"    async function reboot() {"
"      if (!confirm('确定要重启设备吗？')) return;"
"      try {"
"        await fetch('/api/reboot', {method: 'POST'});"
"        showToast('正在重启...', 'warning');"
"      } catch(e) { showToast('重启失败', 'error'); }"
"    }"
""
"    /* Update send button text */"
"    function updateSendBtn() {"
"      var btn = document.getElementById('sendBtn');"
"      if (pending > 0) {"
"        btn.textContent = '思考中(' + pending + ')';"
"      } else {"
"        btn.textContent = '发送';"
"      }"
"    }"
""
"    /* WebSocket & Chat */"
"    function connectWS() {"
"      const protocol = location.protocol === 'https:' ? 'wss:' : 'ws:';"
"      const wsUrl = protocol + '//' + location.hostname + ':' + WS_PORT;"
"      ws = new WebSocket(wsUrl);"
""
"      ws.onopen = function() {"
"        connected = true;"
"        document.getElementById('wsDot').classList.add('connected');"
"        document.getElementById('wsText').textContent = '已连接';"
"      };"
""
"      ws.onmessage = function(event) {"
"        try {"
"          const data = JSON.parse(event.data);"
"          if (data.chat_id !== myChatId) return;"
""
"          if (data.type === 'token') {"
"            if (!currentStreamDiv) {"
"              currentStreamDiv = addChatMessage('assistant', '', true);"
"            }"
"            if(currentStreamDiv) {"
"                const indicator = currentStreamDiv.querySelector('.typing-indicator');"
"                if (indicator) {"
"                    indicator.outerHTML = '<span class=\"content-span\"></span>';"
"                }"
"                const span = currentStreamDiv.querySelector('.content-span');"
"                if (span) span.innerHTML += data.token.replace(/\\n/g, '<br>');"
"                const container = document.getElementById('chatMessages');"
"                container.scrollTop = container.scrollHeight;"
"            }"
"          } else if (data.type === 'status') {"
"             /* Update thinking bubble with status text */"
"             if (currentStreamDiv) {"
"                 const indicator = currentStreamDiv.querySelector('.typing-indicator');"
"                 if (indicator) {"
"                     let statusText = indicator.querySelector('.status-text');"
"                     if (!statusText) {"
"                         statusText = document.createElement('span');"
"                         statusText.className = 'status-text';"
"                         statusText.style.fontSize = '12px';"
"                         statusText.style.color = '#64748b';"
"                         statusText.style.marginRight = '6px';"
"                         indicator.insertBefore(statusText, indicator.firstChild);"
"                     }"
"                     statusText.textContent = data.content;"
"                 }"
"             }"
"          } else if (data.type === 'done') {"
"             if (currentStreamDiv) {"
"                 const indicator = currentStreamDiv.querySelector('.typing-indicator');"
"                 if (indicator) indicator.remove();"
"                 currentStreamDiv = null;"
"             }"
"             if (pending > 0) pending--;"
"             if (pendingTimer && pending === 0) { clearTimeout(pendingTimer); pendingTimer = null; }"
"             updateSendBtn();"
"          } else if (data.type === 'response') {"
"            if (currentStreamDiv) {"
"              currentStreamDiv.remove();"
"              currentStreamDiv = null;"
"            }"
"            addChatMessage('assistant', data.content);"
"            if (pending > 0) pending--;"
"            if (pendingTimer && pending === 0) { clearTimeout(pendingTimer); pendingTimer = null; }"
"            updateSendBtn();"
"          }"
"        } catch(e) {}"
"      };"
""
"      ws.onclose = function() {"
"        connected = false;"
"        document.getElementById('wsDot').classList.remove('connected');"
"        document.getElementById('wsText').textContent = '重连中...';"
"        pending = 0; updateSendBtn();"
"        setTimeout(connectWS, 3000);"
"      };"
""
"      ws.onerror = function() {"
"        document.getElementById('wsText').textContent = '连接错误';"
"      };"
"    }"
""
"    function addChatMessage(role, content, isStream) {"
"      const div = document.createElement('div');"
"      div.className = 'chat-message ' + role;"
"      if (isStream) {"
"        div.innerHTML = '<span class=\"content-span\">' + content.replace(/\\n/g, '<br>') + '</span>';"
"      } else {"
"        div.innerHTML = content.replace(/\\n/g, '<br>');"
"      }"
"      div.innerHTML += '<div class=\"time\">' + new Date().toLocaleTimeString() + '</div>';"
"      document.getElementById('chatMessages').appendChild(div);"
"      document.getElementById('chatMessages').scrollTop = document.getElementById('chatMessages').scrollHeight;"
"      return div;"
"    }"
""
"    function sendChat() {"
"      if (!connected) { showToast('未连接到设备', 'error'); return; }"
"      const msg = document.getElementById('chatInput').value.trim();"
"      if (!msg) return;"
""
"      addChatMessage('user', msg);"
"      document.getElementById('chatInput').value = '';"
"      pending++;"
"      updateSendBtn();"
"      "
"      /* Show thinking animation immediately */"
"      const thinkingHtml = '<div class=\"typing-indicator\"><div class=\"typing-dot\"></div><div class=\"typing-dot\"></div><div class=\"typing-dot\"></div></div>';"
"      currentStreamDiv = addChatMessage('assistant', thinkingHtml, false);"
""
"      if (pendingTimer) clearTimeout(pendingTimer);"
"      pendingTimer = setTimeout(function() { pending = 0; updateSendBtn(); addChatMessage('error', '响应超时，请重试'); }, 120000);"
""
"      const model = document.getElementById('modelSelect').value;"
"      let payload = {type: 'message', content: msg, chat_id: myChatId};"
"      if (model) { payload.model = model; }"
"      ws.send(JSON.stringify(payload));"
"    }"
""
"    function handleChatKey(e) {"
"      if (e.key === 'Enter' && !e.shiftKey) {"
"        e.preventDefault();"
"        sendChat();"
"      }"
"    }"
""
"    /* Agent */"
"    async function loadAgent() {"
"      try {"
"        const resp = await fetch('/api/agent');"
"        const data = await resp.json();"
"        document.getElementById('agentSoul').value = data.soul || '';"
"        document.getElementById('agentUser').value = data.user || '';"
"        document.getElementById('agentMemory').value = data.memory || '';"
"        document.getElementById('agentHeartbeat').value = data.heartbeat || '';"
"      } catch(e) { console.error(e); }"
"    }"
""
"    async function saveAgent() {"
"      const body = {"
"        soul: document.getElementById('agentSoul').value,"
"        user: document.getElementById('agentUser').value,"
"        memory: document.getElementById('agentMemory').value,"
"        heartbeat: document.getElementById('agentHeartbeat').value"
"      };"
"      try {"
"        const resp = await fetch('/api/agent', {"
"          method: 'POST',"
"          headers: {'Content-Type': 'application/json'},"
"          body: JSON.stringify(body)"
"        });"
"        if (resp.ok) { showToast('Agent 配置已保存', 'success'); }"
"        else { showToast('保存失败', 'error'); }"
"      } catch(e) { showToast('保存失败: ' + e, 'error'); }"
"    }"
""
"    /* Tools - Search Key */"
"    async function loadSearchKey() {"
"      try {"
"        const resp = await fetch('/api/tools/search_key');"
"        const data = await resp.json();"
"        document.getElementById('searchKey').value = data.key || '';"
"      } catch(e) { console.error(e); }"
"    }"
""
"    async function saveSearchKey() {"
"      const key = document.getElementById('searchKey').value.trim();"
"      if (!key) { showToast('请输入 API Key', 'error'); return; }"
"      try {"
"        const resp = await fetch('/api/tools/search_key', {"
"          method: 'POST',"
"          headers: {'Content-Type': 'application/json'},"
"          body: JSON.stringify({key: key})"
"        });"
"        if (resp.ok) { showToast('搜索 Key 已保存', 'success'); }"
"        else { showToast('保存失败', 'error'); }"
"      } catch(e) { showToast('保存失败: ' + e, 'error'); }"
"    }"
""
"    /* Tools - Cron Jobs */\n"
"    async function loadCronJobs() {\n"
"      try {\n"
"        const resp = await fetch('/api/tools/cron');\n"
"        const data = await resp.json();\n"
"        const el = document.getElementById('cronList');\n"
"        if (!data.jobs || data.jobs.length === 0) {\n"
"          el.innerHTML = '<div style=\"color:#888\">没有活动的定时任务</div>';\n"
"          return;\n"
"        }\n"
"        let html = '';\n"
"        data.jobs.forEach(function(j) {\n"
"          var sched = j.kind === 'every' ? '每 ' + j.interval_s + ' 秒' : '在 ' + new Date(j.at_epoch * 1000).toLocaleString();\n"
"          html += '<div style=\"display:flex;align-items:center;justify-content:space-between;padding:8px;margin:4px 0;background:#1a1a2e;border-radius:6px\">';\n"
"          html += '<div><b>' + j.name + '</b><br><span style=\"font-size:11px;color:#888\">' + sched + ' | ' + (j.enabled ? '✅ 启用' : '❌ 禁用') + ' | ID: ' + j.id + '</span></div>';\n"
"          html += '<button class=\"btn btn-sm btn-danger\" onclick=\\'deleteCronJob(\"' + j.id + '\")\\'>删除</button>';\n"
"          html += '</div>';\n"
"        });\n"
"        el.innerHTML = html;\n"
"      } catch(e) { document.getElementById('cronList').innerHTML = '加载失败'; }\n"
"    }\n"
"\n"
"    async function deleteCronJob(id) {\n"
"      if (!confirm('确定删除任务 ' + id + ' 吗？')) return;\n"
"      try {\n"
"        const resp = await fetch('/api/tools/cron?id=' + id, { method: 'DELETE' });\n"
"        if (resp.ok) { showToast('已删除', 'success'); loadCronJobs(); }\n"
"        else { showToast('删除失败', 'error'); }\n"
"      } catch(e) { showToast('删除失败: ' + e, 'error'); }\n"
"    }\n"
""
"    async function loadHardwareStatus() {\n"
"      try {\n"
"        const resp = await fetch('/api/hardware/status');\n"
"        const data = await resp.json();\n"
"        let html = '<div style=\"display:grid;grid-template-columns:1fr 1fr;gap:8px;color:#1e293b;font-weight:600\">';\n"
"        html += '<div>CPU: ' + data.cpu_freq_mhz + ' MHz</div>';\n"
"        html += '<div>Temp: ' + data.cpu_temp_c.toFixed(1) + ' °C</div>';\n"
"        html += '<div>Heap (Int): ' + (data.free_heap_internal/1024).toFixed(1) + ' KB</div>';\n"
"        html += '<div>Heap (PSRAM): ' + (data.free_heap_psram/1024).toFixed(1) + ' KB</div>';\n"
"        html += '<div>Uptime: ' + data.uptime_s + ' s</div></div>';\n"
"        document.getElementById('hw-status').innerHTML = html;\n"
"        if(data.gpio) {\n"
"           for (const [p, lvl] of Object.entries(data.gpio)) {\n"
"               const bOn = document.getElementById('btn-gpio-' + p + '-on');\n"
"               const bOff = document.getElementById('btn-gpio-' + p + '-off');\n"
"               if(bOn && bOff) {\n"
"                   bOn.style.opacity = lvl ? '1' : '0.3';\n"
"                   bOff.style.opacity = !lvl ? '1' : '0.3';\n"
"               }\n"
"           }\n"
"        }\n"
"      } catch(e) { document.getElementById('hw-status').textContent = 'Error loading status'; }\n"
"    }\n"
"\n"
"    async function scanI2C() {\n"
"      const el = document.getElementById('i2c-result');\n"
"      el.textContent = 'Scanning...';\n"
"      try {\n"
"        const resp = await fetch('/api/hardware/scan', {method:'POST'});\n"
"        const data = await resp.json();\n"
"        if(data.devices && data.devices.length > 0) {\n"
"           const hex = data.devices.map(d => '0x' + d.toString(16).toUpperCase());\n"
"           el.style.color='#1e293b'; el.style.fontWeight='600';\n"
"           el.textContent = 'Found: ' + hex.join(', ');\n"
"        } else {\n"
"           el.textContent = 'No devices found.';\n"
"        }\n"
"      } catch(e) { el.textContent = 'Error: ' + e; }\n"
"    }\n"
"\n"
"    async function toggleGPIO(pin, state) {\n"
"      try {\n"
"          const resp = await fetch('/api/hardware/gpio', {\n"
"             method: 'POST',\n"
"             headers: {'Content-Type': 'application/json'},\n"
"             body: JSON.stringify({pin: pin, state: state})\n"
"          });\n"
"          const txt = await resp.text();\n"
"          if(resp.ok && !txt.startsWith('Error')) {\n"
"              showToast('GPIO ' + pin + (state?' ON':' OFF'), 'success');\n"
"              // Immediate UI update, standard status fetch will confirm later\n"
"              const bOn = document.getElementById('btn-gpio-' + pin + '-on');\n"
"              const bOff = document.getElementById('btn-gpio-' + pin + '-off');\n"
"              if(bOn && bOff) {\n"
"                  bOn.style.opacity = state ? '1' : '0.3';\n"
"                  bOff.style.opacity = !state ? '1' : '0.3';\n"
"              }\n"
"          } else {\n"
"              showToast(txt, 'error');\n"
"          }\n"
"      } catch(e) { showToast('Error: ' + e, 'error'); }\n"
"    }\n"
"\n"
"    function initGPIO() {\n"
"      // Safe pins per backend logic (2,4,5,12-18,21,38)\n"
"      const safe = [2, 4, 5, 12, 13, 14, 15, 16, 17, 18, 21, 38];\n"
"      \n"
"      // Standard ESP32-S3 DevKitC Layout Approximation\n"
"      // Left Header: 3V3, EN, 4, 5, 6, 7, 15, 16, 17, 18, 8, 19, 20, 3, 46, 9, 10, 11, 12, 13, 14\n"
"      const left = [\n"
"          {l:'3V3'}, {l:'EN'}, {p:4}, {p:5}, {p:6}, {p:7}, {p:15}, {p:16}, {p:17}, {p:18}, {p:8}, {p:19}, {p:20}, {p:3}, {p:46}, {p:9}, {p:10}, {p:11}, {p:12}, {p:13}, {p:14}\n"
"      ];\n"
"      // Right Header: 5V, GND, 0, 1, 2, 42, 41, 40, 39, 38, 37, 36, 35, 45, 48, 47, 21\n"
"      const right = [\n"
"          {l:'5V'}, {l:'GND'}, {p:0}, {p:1}, {p:2}, {p:42}, {p:41}, {p:40}, {p:39}, {p:38}, {p:37}, {p:36}, {p:35}, {p:45}, {p:48}, {p:47}, {p:21}\n"
"      ];\n"
"\n"
"      const renderPin = (item) => {\n"
"          if (item.p === undefined) return `<div class=\"pin-card label-only\"><span class=\"pin-lbl\">${item.l}</span></div>`;\n"
"          const isSafe = safe.includes(item.p);\n"
"          let h = `<div class=\"pin-card ${!isSafe?'restricted':''}\" data-pin=\"${item.p}\">`;\n"
"          h += `<span class=\"pin-lbl\">G${item.p}</span>`;\n"
"          if (isSafe) {\n"
"             h += `<div class=\"btn-group-v\">`;\n"
"             h += `<button id=\"btn-gpio-${item.p}-on\" onclick=\"toggleGPIO(${item.p}, true)\" class=\"btn btn-xs btn-outline-secondary\" style=\"opacity:0.3\">ON</button>`;\n"
"             h += `<button id=\"btn-gpio-${item.p}-off\" onclick=\"toggleGPIO(${item.p}, false)\" class=\"btn btn-xs btn-outline-secondary\" style=\"margin-top:2px;opacity:0.3\">OFF</button>`;\n"
"             h += `</div>`;\n"
"          } else {\n"
"             h += `<span class=\"badge-warn\">RSTR</span>`;\n"
"          }\n"
"          h += `</div>`;\n"
"          return h;\n"
"      };\n"
"\n"
"      let html = '<div class=\"board-layout\">';\n"
"      html += '<div class=\"board-row\"><h4>Left Header</h4>';\n"
"      left.forEach(i => html += renderPin(i));\n"
"      html += '</div><div class=\"board-row\"><h4>Right Header</h4>';\n"
"      right.forEach(i => html += renderPin(i));\n"
"      html += '</div></div>';\n"
"      \n"
"      document.getElementById('gpio-grid').innerHTML = html;\n"
"    }\n"
"\n"
"    /* Init */\n"
"    initGPIO();\n"
"    setInterval(loadHardwareStatus, 2000);\n"
"    loadHardwareStatus();\n"
"    refreshStatus();"
"    loadSettings();"
"    loadAgent();"
"    loadSearchKey();"
"    loadCronJobs();"
"    connectWS();"
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
    len += snprintf(buf + len, sizeof(buf) - len, "  \"provider\": \"%s\",\n", provider ? provider : "unknown");
    len += snprintf(buf + len, sizeof(buf) - len, "  \"model\": \"%s\",\n", model ? model : "not set");
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

/* ── Agent file helpers ───────────────────────────────────────── */

static int read_spiffs_file(const char *path, char *buf, size_t size)
{
    FILE *f = fopen(path, "r");
    if (!f) { buf[0] = '\0'; return 0; }
    int n = fread(buf, 1, size - 1, f);
    fclose(f);
    if (n < 0) n = 0;
    buf[n] = '\0';
    return n;
}

static void write_spiffs_file(const char *path, const char *data, size_t len)
{
    FILE *f = fopen(path, "w");
    if (!f) { ESP_LOGE(TAG, "Cannot write %s", path); return; }
    fwrite(data, 1, len, f);
    fclose(f);
}

/* JSON-escape a string into buf, return bytes written */
static int json_escape(const char *src, char *buf, size_t size)
{
    int pos = 0;
    for (const char *p = src; *p && pos < (int)size - 2; p++) {
        if (*p == '"')       { if (pos + 2 >= (int)size) break; buf[pos++] = '\\'; buf[pos++] = '"'; }
        else if (*p == '\\') { if (pos + 2 >= (int)size) break; buf[pos++] = '\\'; buf[pos++] = '\\'; }
        else if (*p == '\n') { if (pos + 2 >= (int)size) break; buf[pos++] = '\\'; buf[pos++] = 'n'; }
        else if (*p == '\r') { /* skip */ }
        else if (*p == '\t') { if (pos + 2 >= (int)size) break; buf[pos++] = '\\'; buf[pos++] = 't'; }
        else { buf[pos++] = *p; }
    }
    buf[pos] = '\0';
    return pos;
}

static esp_err_t agent_get_handler(httpd_req_t *req)
{
    /* Read each file — use PSRAM if available */
    char *fbuf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *ebuf = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *resp = heap_caps_malloc(40960, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!fbuf || !ebuf || !resp) {
        free(fbuf); free(ebuf); free(resp);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int off = 0;
    off += snprintf(resp + off, 40960 - off, "{\n");

    read_spiffs_file(MIMI_SOUL_FILE, fbuf, 4096);
    json_escape(fbuf, ebuf, 8192);
    off += snprintf(resp + off, 40960 - off, "  \"soul\": \"%s\",\n", ebuf);

    read_spiffs_file(MIMI_USER_FILE, fbuf, 4096);
    json_escape(fbuf, ebuf, 8192);
    off += snprintf(resp + off, 40960 - off, "  \"user\": \"%s\",\n", ebuf);

    read_spiffs_file(MIMI_MEMORY_FILE, fbuf, 4096);
    json_escape(fbuf, ebuf, 8192);
    off += snprintf(resp + off, 40960 - off, "  \"memory\": \"%s\",\n", ebuf);

    read_spiffs_file(MIMI_HEARTBEAT_FILE, fbuf, 4096);
    json_escape(fbuf, ebuf, 8192);
    off += snprintf(resp + off, 40960 - off, "  \"heartbeat\": \"%s\"\n", ebuf);

    off += snprintf(resp + off, 40960 - off, "}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, off);
    free(fbuf); free(ebuf); free(resp);
    return ESP_OK;
}

/* Extract a JSON string value, converting \n back to real newlines */
static int extract_json_string(const char *json, const char *key, char *out, size_t out_size)
{
    char search[32];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *start = strstr(json, search);
    if (!start) return 0;

    const char *colon = strchr(start + strlen(search), ':');
    if (!colon) return 0;
    const char *q1 = strchr(colon + 1, '"');
    if (!q1) return 0;
    q1++;

    int pos = 0;
    for (const char *p = q1; *p && pos < (int)out_size - 1; p++) {
        if (*p == '\\' && *(p+1)) {
            p++;
            if (*p == 'n') out[pos++] = '\n';
            else if (*p == 't') out[pos++] = '\t';
            else if (*p == '"') out[pos++] = '"';
            else if (*p == '\\') out[pos++] = '\\';
            else { out[pos++] = '\\'; if (pos < (int)out_size - 1) out[pos++] = *p; }
        } else if (*p == '"') {
            break;
        } else {
            out[pos++] = *p;
        }
    }
    out[pos] = '\0';
    return pos;
}

static esp_err_t agent_post_handler(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 32768) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
        return ESP_FAIL;
    }

    char *buf = heap_caps_malloc(total + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *val = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf || !val) {
        free(buf); free(val);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int received = 0;
    while (received < total) {
        int n = httpd_req_recv(req, buf + received, total - received);
        if (n <= 0) { free(buf); free(val); return ESP_FAIL; }
        received += n;
    }
    buf[received] = '\0';

    int len;
    len = extract_json_string(buf, "soul", val, 8192);
    if (len > 0) write_spiffs_file(MIMI_SOUL_FILE, val, len);

    len = extract_json_string(buf, "user", val, 8192);
    if (len > 0) write_spiffs_file(MIMI_USER_FILE, val, len);

    len = extract_json_string(buf, "memory", val, 8192);
    if (len > 0) write_spiffs_file(MIMI_MEMORY_FILE, val, len);

    len = extract_json_string(buf, "heartbeat", val, 8192);
    if (len > 0) write_spiffs_file(MIMI_HEARTBEAT_FILE, val, len);

    free(buf); free(val);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":true}", 16);
    return ESP_OK;
}

/* ── Tools API handlers ─────────────────────────────────────── */

static esp_err_t search_key_get_handler(httpd_req_t *req)
{
    char key[128] = {0};
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_SEARCH, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(key);
        nvs_get_str(nvs, MIMI_NVS_KEY_API_KEY, key, &len);
        nvs_close(nvs);
    }

    /* Mask the key for display: show first 4 + last 4 chars */
    char masked[128] = {0};
    size_t klen = strlen(key);
    if (klen > 8) {
        snprintf(masked, sizeof(masked), "%.4s****%s", key, key + klen - 4);
    } else if (klen > 0) {
        snprintf(masked, sizeof(masked), "****");
    }

    char resp[256];
    snprintf(resp, sizeof(resp), "{\"key\":\"%s\",\"configured\":%s}",
             masked, klen > 0 ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

static esp_err_t search_key_post_handler(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    /* Extract key */
    char key[128] = {0};
    extract_json_string(buf, "key", key, sizeof(key));

    if (key[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing key");
        return ESP_FAIL;
    }

    tool_web_search_set_key(key);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":true}", 16);
    return ESP_OK;
}

static esp_err_t cron_get_handler(httpd_req_t *req)
{
    const cron_job_t *jobs;
    int count;
    cron_list_jobs(&jobs, &count);

    char *resp = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int off = 0;
    off += snprintf(resp + off, 8192 - off, "{\"jobs\":[");

    for (int i = 0; i < count; i++) {
        const cron_job_t *j = &jobs[i];
        if (i > 0) off += snprintf(resp + off, 8192 - off, ",");
        off += snprintf(resp + off, 8192 - off,
            "{\"id\":\"%s\",\"name\":\"%s\",\"enabled\":%s,\"kind\":\"%s\","
            "\"interval_s\":%lu,\"at_epoch\":%lld,\"next_run\":%lld,\"last_run\":%lld}",
            j->id, j->name,
            j->enabled ? "true" : "false",
            j->kind == CRON_KIND_EVERY ? "every" : "at",
            (unsigned long)j->interval_s,
            (long long)j->at_epoch,
            (long long)j->next_run,
            (long long)j->last_run);
    }

    off += snprintf(resp + off, 8192 - off, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, off);
    free(resp);
    return ESP_OK;
}

static esp_err_t cron_delete_handler(httpd_req_t *req)
{
    /* Get job ID from query string */
    char query[64] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query");
        return ESP_FAIL;
    }

    char job_id[16] = {0};
    if (httpd_query_key_value(query, "id", job_id, sizeof(job_id)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing id");
        return ESP_FAIL;
    }

    esp_err_t err = cron_remove_job(job_id);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Job not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":true}", 16);
    return ESP_OK;
}

/* ── Server Init ───────────────────────────────────────────────── */

esp_err_t web_ui_init(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.max_open_sockets = 3;  /* keep low — only serves HTML/JSON */
    config.max_uri_handlers = 16;

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

    httpd_uri_t api_agent_get_uri = {
        .uri = "/api/agent",
        .method = HTTP_GET,
        .handler = agent_get_handler,
    };
    httpd_register_uri_handler(server, &api_agent_get_uri);

    httpd_uri_t api_agent_post_uri = {
        .uri = "/api/agent",
        .method = HTTP_POST,
        .handler = agent_post_handler,
    };
    httpd_register_uri_handler(server, &api_agent_post_uri);

    httpd_uri_t api_search_key_get = {
        .uri = "/api/tools/search_key",
        .method = HTTP_GET,
        .handler = search_key_get_handler,
    };
    httpd_register_uri_handler(server, &api_search_key_get);

    httpd_uri_t api_search_key_post = {
        .uri = "/api/tools/search_key",
        .method = HTTP_POST,
        .handler = search_key_post_handler,
    };
    httpd_register_uri_handler(server, &api_search_key_post);

    httpd_uri_t api_cron_get = {
        .uri = "/api/tools/cron",
        .method = HTTP_GET,
        .handler = cron_get_handler,
    };
    httpd_register_uri_handler(server, &api_cron_get);

    httpd_uri_t api_cron_del = {
        .uri = "/api/tools/cron",
        .method = HTTP_DELETE,
        .handler = cron_delete_handler,
    };
    httpd_register_uri_handler(server, &api_cron_del);

    tool_hardware_register_handlers(server);

    ESP_LOGI(TAG, "Web UI started on port 80");
    return ESP_OK;
}

esp_err_t web_ui_stop(void)
{
    // TODO: implement stop
    return ESP_OK;
}
