// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/webui/molt_ai/molt_ai_settings_ui.h"

#include "base/memory/ref_counted_memory.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_message_handler.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"
#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/values.h"

namespace {

const char kSettingsFileName[] = "settings.json";

base::FilePath GetSettingsFilePath() {
  base::FilePath home_dir;
  base::PathService::Get(base::DIR_HOME, &home_dir);
  return home_dir.Append(".moltbrowser").Append(kSettingsFileName);
}

// ---- Settings Handler ----
class MoltAISettingsHandler : public content::WebUIMessageHandler {
 public:
  MoltAISettingsHandler() = default;
  ~MoltAISettingsHandler() override = default;

  void RegisterMessages() override {
    web_ui()->RegisterMessageCallback(
        "getSettings",
        base::BindRepeating(&MoltAISettingsHandler::HandleGetSettings,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "saveSettings",
        base::BindRepeating(&MoltAISettingsHandler::HandleSaveSettings,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "resetSettings",
        base::BindRepeating(&MoltAISettingsHandler::HandleResetSettings,
                            base::Unretained(this)));
  }

 private:
  base::DictValue GetDefaultSettings() {
    base::DictValue defaults;
    defaults.Set("max_tokens", 512);
    defaults.Set("temperature", 0.7);
    defaults.Set("top_p", 0.9);
    defaults.Set("top_k", 40);
    defaults.Set("max_history_messages", 16);
    defaults.Set("max_page_content_chars", 4000);
    defaults.Set("auto_load_model", true);
    defaults.Set("default_model", "tinyllama-1.1b");
    defaults.Set("system_prompt",
        "You are MoltBrowser AI, a helpful local AI assistant built into "
        "the MoltBrowser web browser. You run entirely on the user's "
        "device for privacy. Be concise, accurate, and helpful. Format "
        "your responses with markdown when appropriate.");
    return defaults;
  }

  base::DictValue LoadSettings() {
    base::FilePath path = GetSettingsFilePath();
    std::string contents;
    if (base::ReadFileToString(path, &contents)) {
      auto parsed = base::JSONReader::Read(
          contents, base::JSON_ALLOW_TRAILING_COMMAS);
      if (parsed && parsed->is_dict()) {
        // Return loaded settings as DictValue
        base::DictValue loaded;
        for (const auto [key, value] : parsed->GetDict()) {
          loaded.Set(key, value.Clone());
        }
        return loaded;
      }
    }
    return GetDefaultSettings();
  }

  bool SaveSettings(const base::DictValue& settings) {
    base::FilePath path = GetSettingsFilePath();
    base::FilePath dir = path.DirName();
    if (!base::DirectoryExists(dir)) {
      base::CreateDirectory(dir);
    }
    std::string json;
    base::JSONWriter::WriteWithOptions(
        settings, base::JSONWriter::OPTIONS_PRETTY_PRINT, &json);
    return base::WriteFile(path, json);
  }

  void HandleGetSettings(const base::ListValue& args) {
    AllowJavascript();
    CHECK_GE(args.size(), 1u);
    const std::string callback_id = args[0].GetString();

    base::DictValue settings = LoadSettings();
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(settings)));
  }

  void HandleSaveSettings(const base::ListValue& args) {
    AllowJavascript();
    CHECK_GE(args.size(), 2u);
    const std::string callback_id = args[0].GetString();

    if (!args[1].is_dict()) {
      base::DictValue error_result;
      error_result.Set("success", false);
      error_result.Set("error", "Invalid settings format");
      ResolveJavascriptCallback(base::Value(callback_id),
                                base::Value(std::move(error_result)));
      return;
    }

    // Copy provided settings into a DictValue
    base::DictValue current;
    for (const auto [key, value] : args[1].GetDict()) {
      current.Set(key, value.Clone());
    }

    bool success = SaveSettings(current);
    LOG(INFO) << "[MoltAI] Settings saved: " << success;

    base::DictValue result;
    result.Set("success", success);
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
  }

  void HandleResetSettings(const base::ListValue& args) {
    AllowJavascript();
    CHECK_GE(args.size(), 1u);
    const std::string callback_id = args[0].GetString();

    base::DictValue defaults = GetDefaultSettings();
    SaveSettings(defaults);

    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(defaults)));
  }
};

// ---- Data Source ----
class MoltAISettingsDataSource : public content::URLDataSource {
 public:
  MoltAISettingsDataSource() = default;
  ~MoltAISettingsDataSource() override = default;

  std::string GetSource() override {
    return chrome::kChromeUIMoltAISettingsHost;
  }

  std::string GetMimeType(const GURL& url) override {
    return "text/html";
  }

  std::string GetContentSecurityPolicy(
      const network::mojom::CSPDirectiveName directive) override {
    if (directive == network::mojom::CSPDirectiveName::ScriptSrc) {
      return "script-src chrome://resources 'self' 'unsafe-inline';";
    }
    if (directive == network::mojom::CSPDirectiveName::StyleSrc) {
      return "style-src 'self' 'unsafe-inline';";
    }
    if (directive == network::mojom::CSPDirectiveName::TrustedTypes) {
      return "";
    }
    if (directive ==
        network::mojom::CSPDirectiveName::RequireTrustedTypesFor) {
      return "";
    }
    return content::URLDataSource::GetContentSecurityPolicy(directive);
  }

  void StartDataRequest(
      const GURL& url,
      const content::WebContents::Getter& wc_getter,
      content::URLDataSource::GotDataCallback callback) override {
    const std::string html = R"HTML(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>MoltBrowser AI Settings</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#0a0a0a;color:#e0e0e0;min-height:100vh;padding:40px 20px}
.container{max-width:640px;margin:0 auto}
.logo{font-size:28px;font-weight:700;background:linear-gradient(135deg,#6366f1,#a855f7);-webkit-background-clip:text;-webkit-text-fill-color:transparent;margin-bottom:4px}
.subtitle{color:#888;margin-bottom:24px;font-size:14px}
.nav{display:flex;gap:12px;margin-bottom:24px}
.nav a{color:#6366f1;text-decoration:none;font-size:13px;padding:6px 12px;border:1px solid #333;border-radius:8px;transition:all 0.2s}
.nav a:hover{border-color:#6366f1;background:#111}
.section{background:#111;border:1px solid #222;border-radius:12px;padding:20px;margin-bottom:16px}
.section h2{font-size:16px;font-weight:600;margin-bottom:16px;color:#e0e0e0;display:flex;align-items:center;gap:8px}
.section h2 .icon{font-size:18px}
.field{margin-bottom:16px}
.field:last-child{margin-bottom:0}
.field label{display:block;font-size:13px;font-weight:600;color:#aaa;margin-bottom:6px}
.field .desc{font-size:11px;color:#666;margin-bottom:6px}
.field input[type="number"],.field input[type="text"],.field textarea{width:100%;padding:10px 14px;border-radius:8px;border:1px solid #333;background:#0d0d0d;color:#e0e0e0;font-size:13px;outline:none;transition:border-color 0.2s;font-family:inherit}
.field input:focus,.field textarea:focus{border-color:#6366f1}
.field textarea{resize:vertical;min-height:80px}
.field select{padding:10px 14px;border-radius:8px;border:1px solid #333;background:#0d0d0d;color:#e0e0e0;font-size:13px;outline:none;width:100%;cursor:pointer}
.field .range-wrap{display:flex;align-items:center;gap:12px}
.field input[type="range"]{flex:1;accent-color:#6366f1}
.field .range-val{font-size:13px;color:#6366f1;min-width:40px;text-align:right;font-weight:600}
.toggle{display:flex;align-items:center;gap:10px;cursor:pointer}
.toggle input{display:none}
.toggle .track{width:40px;height:22px;border-radius:11px;background:#333;position:relative;transition:background 0.2s}
.toggle input:checked + .track{background:#6366f1}
.toggle .track::after{content:'';position:absolute;top:2px;left:2px;width:18px;height:18px;border-radius:50%;background:#e0e0e0;transition:transform 0.2s}
.toggle input:checked + .track::after{transform:translateX(18px)}
.toggle .label{font-size:13px}
.actions{display:flex;gap:10px;margin-top:20px}
.btn{padding:10px 24px;border-radius:10px;border:none;font-size:14px;font-weight:600;cursor:pointer;transition:all 0.2s}
.btn.primary{background:linear-gradient(135deg,#6366f1,#8b5cf6);color:white}
.btn.primary:hover{opacity:0.85}
.btn.secondary{background:#111;border:1px solid #333;color:#aaa}
.btn.secondary:hover{border-color:#6366f1;color:#e0e0e0}
.btn.danger{background:transparent;border:1px solid #f87171;color:#f87171}
.btn.danger:hover{background:#2a1111}
.toast{position:fixed;bottom:24px;right:24px;padding:12px 20px;border-radius:10px;background:#1a2e1a;color:#4ade80;font-size:13px;font-weight:600;border:1px solid #2a4a2a;transform:translateY(100px);opacity:0;transition:all 0.3s}
.toast.show{transform:translateY(0);opacity:1}
.model-dir{font-family:monospace;font-size:12px;color:#888;padding:8px 12px;background:#0a0a0a;border-radius:6px;border:1px solid #1a1a1a;margin-top:6px}
</style>
</head>
<body>
<div class="container">
  <div class="logo">AI Settings</div>
  <div class="subtitle">Configure MoltBrowser's local AI assistant</div>
  <div class="nav">
    <a href="chrome://molt-ai/">AI Chat</a>
    <a href="chrome://molt-ai-chat/">Side Panel</a>
  </div>

  <div class="section">
    <h2><span class="icon">&#9881;</span> Generation</h2>
    <div class="field">
      <label>Max Tokens</label>
      <div class="desc">Maximum number of tokens to generate per response (64-2048)</div>
      <div class="range-wrap">
        <input type="range" id="maxTokens" min="64" max="2048" step="64" value="512" oninput="document.getElementById('maxTokensVal').textContent=this.value">
        <span class="range-val" id="maxTokensVal">512</span>
      </div>
    </div>
    <div class="field">
      <label>Temperature</label>
      <div class="desc">Higher values make output more random (0.1-2.0)</div>
      <div class="range-wrap">
        <input type="range" id="temperature" min="0.1" max="2.0" step="0.1" value="0.7" oninput="document.getElementById('tempVal').textContent=parseFloat(this.value).toFixed(1)">
        <span class="range-val" id="tempVal">0.7</span>
      </div>
    </div>
    <div class="field">
      <label>Top P</label>
      <div class="desc">Nucleus sampling threshold (0.1-1.0)</div>
      <div class="range-wrap">
        <input type="range" id="topP" min="0.1" max="1.0" step="0.05" value="0.9" oninput="document.getElementById('topPVal').textContent=parseFloat(this.value).toFixed(2)">
        <span class="range-val" id="topPVal">0.90</span>
      </div>
    </div>
    <div class="field">
      <label>Top K</label>
      <div class="desc">Limit token selection to top K candidates (1-100)</div>
      <div class="range-wrap">
        <input type="range" id="topK" min="1" max="100" step="1" value="40" oninput="document.getElementById('topKVal').textContent=this.value">
        <span class="range-val" id="topKVal">40</span>
      </div>
    </div>
  </div>

  <div class="section">
    <h2><span class="icon">&#128172;</span> Conversation</h2>
    <div class="field">
      <label>Max History Messages</label>
      <div class="desc">Number of previous messages to include for context (4-32)</div>
      <div class="range-wrap">
        <input type="range" id="maxHistory" min="4" max="32" step="2" value="16" oninput="document.getElementById('histVal').textContent=this.value">
        <span class="range-val" id="histVal">16</span>
      </div>
    </div>
    <div class="field">
      <label>Max Page Content (chars)</label>
      <div class="desc">Maximum characters to extract from page for context (1000-8000)</div>
      <div class="range-wrap">
        <input type="range" id="maxPageContent" min="1000" max="8000" step="500" value="4000" oninput="document.getElementById('pageVal').textContent=this.value">
        <span class="range-val" id="pageVal">4000</span>
      </div>
    </div>
    <div class="field">
      <label>System Prompt</label>
      <div class="desc">Customize the AI's behavior and personality</div>
      <textarea id="systemPrompt" rows="4"></textarea>
    </div>
  </div>

  <div class="section">
    <h2><span class="icon">&#129302;</span> Model</h2>
    <div class="field">
      <label>Default Model</label>
      <select id="defaultModel">
        <option value="tinyllama-1.1b">TinyLlama 1.1B (Fastest)</option>
        <option value="phi-3.5-3b">Phi-3.5 3B (Balanced)</option>
        <option value="mistral-7b">Mistral 7B (Quality)</option>
        <option value="llama-3.1-8b">LLaMA 3.1 8B (Best)</option>
        <option value="qwen2.5-7b">Qwen2.5 7B</option>
        <option value="gemma-2-9b">Gemma 2 9B</option>
      </select>
    </div>
    <div class="field">
      <label class="toggle">
        <input type="checkbox" id="autoLoadModel" checked>
        <span class="track"></span>
        <span class="label">Auto-load model on first prompt</span>
      </label>
    </div>
    <div class="field">
      <label>Model Directory</label>
      <div class="model-dir">~/.moltbrowser/models/</div>
    </div>
  </div>

  <div class="actions">
    <button class="btn primary" onclick="saveSettings()">Save Settings</button>
    <button class="btn danger" onclick="resetSettings()">Reset to Defaults</button>
  </div>
</div>

<div class="toast" id="toast">Settings saved!</div>

<script>
var idCounter = 0;
var pendingCbs = {};

function sendWithPromise(method) {
  var args = Array.prototype.slice.call(arguments, 1);
  var id = method + '_' + (++idCounter);
  return new Promise(function(resolve, reject) {
    pendingCbs[id] = {resolve: resolve, reject: reject};
    chrome.send(method, [id].concat(args));
  });
}

window.cr = window.cr || {};
cr.webUIResponse = function(id, ok, resp) {
  var cb = pendingCbs[id]; if (cb) { delete pendingCbs[id]; ok ? cb.resolve(resp) : cb.reject(resp); }
};

function showToast(msg) {
  var t = document.getElementById('toast');
  t.textContent = msg;
  t.classList.add('show');
  setTimeout(function() { t.classList.remove('show'); }, 2000);
}

function loadSettingsIntoUI(s) {
  document.getElementById('maxTokens').value = s.max_tokens || 512;
  document.getElementById('maxTokensVal').textContent = s.max_tokens || 512;
  document.getElementById('temperature').value = s.temperature || 0.7;
  document.getElementById('tempVal').textContent = (s.temperature || 0.7).toFixed(1);
  document.getElementById('topP').value = s.top_p || 0.9;
  document.getElementById('topPVal').textContent = (s.top_p || 0.9).toFixed(2);
  document.getElementById('topK').value = s.top_k || 40;
  document.getElementById('topKVal').textContent = s.top_k || 40;
  document.getElementById('maxHistory').value = s.max_history_messages || 16;
  document.getElementById('histVal').textContent = s.max_history_messages || 16;
  document.getElementById('maxPageContent').value = s.max_page_content_chars || 4000;
  document.getElementById('pageVal').textContent = s.max_page_content_chars || 4000;
  document.getElementById('systemPrompt').value = s.system_prompt || '';
  document.getElementById('defaultModel').value = s.default_model || 'tinyllama-1.1b';
  document.getElementById('autoLoadModel').checked = s.auto_load_model !== false;
}

function gatherSettings() {
  return {
    max_tokens: parseInt(document.getElementById('maxTokens').value),
    temperature: parseFloat(document.getElementById('temperature').value),
    top_p: parseFloat(document.getElementById('topP').value),
    top_k: parseInt(document.getElementById('topK').value),
    max_history_messages: parseInt(document.getElementById('maxHistory').value),
    max_page_content_chars: parseInt(document.getElementById('maxPageContent').value),
    system_prompt: document.getElementById('systemPrompt').value,
    default_model: document.getElementById('defaultModel').value,
    auto_load_model: document.getElementById('autoLoadModel').checked
  };
}

function saveSettings() {
  var s = gatherSettings();
  sendWithPromise('saveSettings', s).then(function(r) {
    if (r.success) showToast('Settings saved!');
    else showToast('Error saving settings');
  }).catch(function() { showToast('Error saving settings'); });
}

function resetSettings() {
  sendWithPromise('resetSettings').then(function(s) {
    loadSettingsIntoUI(s);
    showToast('Settings reset to defaults');
  });
}

// Init
sendWithPromise('getSettings').then(function(s) {
  loadSettingsIntoUI(s);
});
</script>
</body>
</html>)HTML";

    std::move(callback).Run(
        base::MakeRefCounted<base::RefCountedString>(std::move(html)));
  }

  bool ShouldReplaceExistingSource() override { return true; }
};

}  // namespace

MoltAISettingsUI::MoltAISettingsUI(content::WebUI* web_ui)
    : content::WebUIController(web_ui) {
  content::URLDataSource::Add(
      web_ui->GetWebContents()->GetBrowserContext(),
      std::make_unique<MoltAISettingsDataSource>());

  web_ui->AddMessageHandler(std::make_unique<MoltAISettingsHandler>());
}

MoltAISettingsUI::~MoltAISettingsUI() = default;
