// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/webui/molt_ai/molt_ai_chat_ui.h"

#include "base/memory/ref_counted_memory.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"

namespace {

// Custom URL data source that serves inline HTML for the chat sidebar
class MoltAIChatDataSource : public content::URLDataSource {
 public:
  MoltAIChatDataSource() = default;
  ~MoltAIChatDataSource() override = default;

  std::string GetSource() override { return chrome::kChromeUIMoltAIChatHost; }

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
<title>AI Chat</title>
<style>
*{margin:0;padding:0;box-sizing:border-box;}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#0d0d0d;color:#e0e0e0;height:100vh;display:flex;flex-direction:column;}
.header{padding:12px 16px;border-bottom:1px solid #222;display:flex;align-items:center;gap:10px;}
.header .title{font-size:14px;font-weight:600;background:linear-gradient(135deg,#6366f1,#a855f7);-webkit-background-clip:text;-webkit-text-fill-color:transparent;}
.header .status{font-size:11px;color:#4ade80;display:flex;align-items:center;gap:4px;}
.header .status::before{content:'';width:6px;height:6px;border-radius:50%;background:#4ade80;}
.context-bar{padding:8px 16px;background:#111;border-bottom:1px solid #1a1a1a;font-size:12px;color:#666;display:flex;align-items:center;gap:6px;}
.context-bar .page-title{color:#999;max-width:200px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;}
.messages{flex:1;overflow-y:auto;padding:16px;}
.message{margin-bottom:16px;font-size:13px;line-height:1.5;}
.message .sender{font-size:11px;font-weight:600;text-transform:uppercase;letter-spacing:0.5px;margin-bottom:4px;color:#6366f1;}
.message.user .sender{color:#8b5cf6;}
.message .content{padding:10px 14px;border-radius:10px;background:#111;border:1px solid #1a1a1a;}
.message.user .content{background:#1a1a2e;border-color:#2a2a4a;}
.actions{padding:8px 16px;display:flex;gap:6px;flex-wrap:wrap;}
.actions button{padding:6px 12px;border-radius:8px;border:1px solid #333;background:#111;color:#aaa;font-size:12px;cursor:pointer;transition:all 0.2s;}
.actions button:hover{border-color:#6366f1;color:#e0e0e0;}
.input-area{padding:12px 16px;border-top:1px solid #222;display:flex;gap:8px;}
.input-area input{flex:1;padding:10px 14px;border-radius:10px;border:1px solid #333;background:#111;color:#e0e0e0;font-size:13px;outline:none;}
.input-area input:focus{border-color:#6366f1;}
.input-area button{padding:10px 16px;border-radius:10px;border:none;background:#6366f1;color:white;font-size:13px;cursor:pointer;}
</style>
</head>
<body>
<div class="header">
<div class="title">MoltBrowser AI</div>
<div class="status">Local Mode</div>
</div>
<div class="context-bar">
<span>Page:</span>
<span class="page-title" id="pageTitle">Loading...</span>
</div>
<div class="messages" id="messages">
<div class="message ai">
<div class="sender">AI Assistant</div>
<div class="content">I can help you with this page. Try asking me to summarize it, extract data, or answer questions about it.</div>
</div>
</div>
<div class="actions">
<button onclick="quickAction('Summarize this page')">Summarize</button>
<button onclick="quickAction('Extract key data')">Extract Data</button>
<button onclick="quickAction('Find contact info')">Find Contacts</button>
<button onclick="quickAction('Translate to English')">Translate</button>
</div>
<div class="input-area">
<input type="text" id="prompt" placeholder="Ask about this page..." autofocus>
<button onclick="sendMessage()">Send</button>
</div>
<script>
document.getElementById('prompt').addEventListener('keydown',function(e){if(e.key==='Enter')sendMessage();});
function quickAction(text){document.getElementById('prompt').value=text;sendMessage();}
function sendMessage(){var i=document.getElementById('prompt');var t=i.value.trim();if(!t)return;addMsg('You',t,'user');i.value='';setTimeout(function(){addMsg('AI Assistant','AI inference pipeline is being connected. You asked: \"'+t+'\"','ai');},300);}
function addMsg(s,t,type){var m=document.getElementById('messages');var d=document.createElement('div');d.className='message '+type;d.innerHTML='<div class="sender">'+s+'</div><div class="content">'+esc(t)+'</div>';m.appendChild(d);m.scrollTop=m.scrollHeight;}
function esc(t){var d=document.createElement('div');d.textContent=t;return d.innerHTML;}
try{var title=window.parent&&window.parent.document?window.parent.document.title:'Unknown';document.getElementById('pageTitle').textContent=title;}catch(e){document.getElementById('pageTitle').textContent='Current Page';}
</script>
</body>
</html>)HTML";

    std::move(callback).Run(
        base::MakeRefCounted<base::RefCountedString>(std::move(html)));
  }

  bool ShouldReplaceExistingSource() override { return true; }
};

}  // namespace

MoltAIChatUI::MoltAIChatUI(content::WebUI* web_ui)
    : content::WebUIController(web_ui) {
  content::URLDataSource::Add(
      web_ui->GetWebContents()->GetBrowserContext(),
      std::make_unique<MoltAIChatDataSource>());
}

MoltAIChatUI::~MoltAIChatUI() = default;
