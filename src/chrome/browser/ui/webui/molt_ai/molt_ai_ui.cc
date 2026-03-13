// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/webui/molt_ai/molt_ai_ui.h"

#include "base/memory/ref_counted_memory.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"

namespace {

// Custom URL data source that serves inline HTML
class MoltAIDataSource : public content::URLDataSource {
 public:
  MoltAIDataSource() = default;
  ~MoltAIDataSource() override = default;

  std::string GetSource() override { return chrome::kChromeUIMoltAIHost; }

  std::string GetMimeType(const GURL& url) override {
    return "text/html";
  }

  void StartDataRequest(
      const GURL& url,
      const content::WebContents::Getter& wc_getter,
      content::URLDataSource::GotDataCallback callback) override {
    const std::string html = R"HTML(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>MoltBrowser AI</title>
<style>
*{margin:0;padding:0;box-sizing:border-box;}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#0a0a0a;color:#e0e0e0;min-height:100vh;display:flex;flex-direction:column;align-items:center;padding:40px 20px;}
.logo{font-size:36px;font-weight:700;background:linear-gradient(135deg,#6366f1,#8b5cf6,#a855f7);-webkit-background-clip:text;-webkit-text-fill-color:transparent;margin-bottom:8px;}
.subtitle{color:#888;margin-bottom:40px;font-size:14px;}
.chat-container{width:100%;max-width:720px;flex:1;display:flex;flex-direction:column;}
.messages{flex:1;overflow-y:auto;padding:20px 0;}
.message{margin-bottom:20px;padding:16px 20px;border-radius:12px;line-height:1.6;font-size:15px;max-width:90%;}
.message.user{background:#1a1a2e;border:1px solid #2a2a4a;margin-left:auto;}
.message.ai{background:#111;border:1px solid #222;}
.message .label{font-size:11px;font-weight:600;text-transform:uppercase;letter-spacing:1px;margin-bottom:8px;color:#6366f1;}
.message.user .label{color:#8b5cf6;}
.model-badge{display:inline-block;padding:2px 8px;border-radius:4px;background:#1a1a2e;color:#8b5cf6;font-size:11px;margin-bottom:8px;}
.input-area{display:flex;gap:12px;padding:20px 0;border-top:1px solid #222;}
.input-area input{flex:1;padding:14px 20px;border-radius:12px;border:1px solid #333;background:#111;color:#e0e0e0;font-size:15px;outline:none;transition:border-color 0.2s;}
.input-area input:focus{border-color:#6366f1;}
.input-area button{padding:14px 28px;border-radius:12px;border:none;background:linear-gradient(135deg,#6366f1,#8b5cf6);color:white;font-size:15px;font-weight:600;cursor:pointer;transition:opacity 0.2s;}
.input-area button:hover{opacity:0.85;}
</style>
</head>
<body>
<div class="logo">MoltBrowser AI</div>
<div class="subtitle">Local AI &#8212; Private by Design &#8212; Powered by llama.cpp</div>
<div class="chat-container">
<div class="messages" id="messages">
<div class="message ai">
<div class="label">MoltBrowser AI</div>
<div class="model-badge">Local LLM</div>
<div>Hello! I'm your local AI assistant. I run entirely on your device. Ask me anything about the current page, or give me a task.</div>
</div>
</div>
<div class="input-area">
<input type="text" id="prompt" placeholder="Ask MoltBrowser AI anything..." autofocus>
<button onclick="sendPrompt()">Send</button>
</div>
</div>
<script>
var params=new URLSearchParams(window.location.search);
var iq=params.get('q');
if(iq){document.getElementById('prompt').value=decodeURIComponent(iq);sendPrompt();}
document.getElementById('prompt').addEventListener('keydown',function(e){if(e.key==='Enter')sendPrompt();});
function sendPrompt(){var i=document.getElementById('prompt');var t=i.value.trim();if(!t)return;addMsg('You',t,'user');i.value='';setTimeout(function(){addMsg('MoltBrowser AI','AI inference pipeline is connecting to llama.cpp. You asked: "'+t+'"','ai');},500);}
function addMsg(s,t,type){var m=document.getElementById('messages');var d=document.createElement('div');d.className='message '+type;d.innerHTML='<div class="label">'+s+'</div>'+(type==='ai'?'<div class="model-badge">Local LLM</div>':'')+'<div>'+esc(t)+'</div>';m.appendChild(d);m.scrollTop=m.scrollHeight;}
function esc(t){var d=document.createElement('div');d.textContent=t;return d.innerHTML;}
</script>
</body>
</html>)HTML";

    std::move(callback).Run(
        base::MakeRefCounted<base::RefCountedString>(std::move(html)));
  }

  bool ShouldReplaceExistingSource() override { return true; }
};

}  // namespace

MoltAIUI::MoltAIUI(content::WebUI* web_ui)
    : content::WebUIController(web_ui) {
  content::URLDataSource::Add(
      web_ui->GetWebContents()->GetBrowserContext(),
      std::make_unique<MoltAIDataSource>());
}

MoltAIUI::~MoltAIUI() = default;
