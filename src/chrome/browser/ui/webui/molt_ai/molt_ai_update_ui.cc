// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/webui/molt_ai/molt_ai_update_ui.h"

#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/ref_counted_memory.h"
#include "base/values.h"
#include "chrome/browser/lifetime/application_lifetime.h"
#include "chrome/browser/molt_ai/update/update_manager.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_message_handler.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"

namespace {

std::string StateToString(molt_ai::UpdateState s) {
  switch (s) {
    case molt_ai::UpdateState::kIdle:
      return "idle";
    case molt_ai::UpdateState::kChecking:
      return "checking";
    case molt_ai::UpdateState::kUpToDate:
      return "uptodate";
    case molt_ai::UpdateState::kAvailable:
      return "available";
    case molt_ai::UpdateState::kDownloading:
      return "downloading";
    case molt_ai::UpdateState::kDownloaded:
      return "downloaded";
    case molt_ai::UpdateState::kInstalling:
      return "installing";
    case molt_ai::UpdateState::kError:
      return "error";
  }
  return "idle";
}

base::DictValue BuildStateDict() {
  molt_ai::UpdateManager* mgr = molt_ai::UpdateManager::Get();
  base::DictValue d;
  d.Set("state", StateToString(mgr->state()));
  d.Set("current_version", mgr->current_version());
  d.Set("latest_version", mgr->latest_version());
  d.Set("release_notes", mgr->release_notes());
  d.Set("release_url", mgr->release_url());
  d.Set("error", mgr->error());
  d.Set("percent", mgr->download_percent());
  d.Set("auto_update", mgr->auto_update());
  d.Set("update_available", mgr->update_available());
  return d;
}

// ---- Handler ----
// Bridges the WebUI page and the process-global UpdateManager. Observes the
// manager so background state changes (auto check/download progress) push to
// the page without polling.
class MoltAIUpdateHandler : public content::WebUIMessageHandler,
                            public molt_ai::UpdateManager::Observer {
 public:
  MoltAIUpdateHandler() {
    molt_ai::UpdateManager::Get()->AddObserver(this);
  }
  ~MoltAIUpdateHandler() override {
    molt_ai::UpdateManager::Get()->RemoveObserver(this);
  }

  MoltAIUpdateHandler(const MoltAIUpdateHandler&) = delete;
  MoltAIUpdateHandler& operator=(const MoltAIUpdateHandler&) = delete;

  void RegisterMessages() override {
    web_ui()->RegisterMessageCallback(
        "updateGetState",
        base::BindRepeating(&MoltAIUpdateHandler::HandleGetState,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "updateCheck",
        base::BindRepeating(&MoltAIUpdateHandler::HandleCheck,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "updateDownload",
        base::BindRepeating(&MoltAIUpdateHandler::HandleDownload,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "updateInstall",
        base::BindRepeating(&MoltAIUpdateHandler::HandleInstall,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "updateSetAuto",
        base::BindRepeating(&MoltAIUpdateHandler::HandleSetAuto,
                            base::Unretained(this)));
  }

  // molt_ai::UpdateManager::Observer
  void OnUpdateChanged() override {
    if (IsJavascriptAllowed()) {
      FireWebUIListener("update-state", base::Value(BuildStateDict()));
    }
  }

 private:
  void HandleGetState(const base::ListValue& args) {
    AllowJavascript();
    CHECK_GE(args.size(), 1u);
    const std::string callback_id = args[0].GetString();
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(BuildStateDict()));
  }

  void HandleCheck(const base::ListValue& args) {
    AllowJavascript();
    molt_ai::UpdateManager::Get()->CheckForUpdates(/*user_initiated=*/true);
  }

  void HandleDownload(const base::ListValue& args) {
    AllowJavascript();
    molt_ai::UpdateManager::Get()->DownloadUpdate();
  }

  void HandleInstall(const base::ListValue& args) {
    AllowJavascript();
    // InstallUpdate launches the detached installer; if that succeeds we must
    // exit so it can replace the running app and relaunch it.
    if (molt_ai::UpdateManager::Get()->InstallUpdate()) {
      chrome::AttemptExit();
    }
  }

  void HandleSetAuto(const base::ListValue& args) {
    AllowJavascript();
    bool enabled = true;
    if (args.size() > 0 && args[0].is_bool()) {
      enabled = args[0].GetBool();
    }
    molt_ai::UpdateManager::Get()->SetAutoUpdate(enabled);
  }
};

// ---- Data source ----
class MoltAIUpdateDataSource : public content::URLDataSource {
 public:
  MoltAIUpdateDataSource() = default;
  ~MoltAIUpdateDataSource() override = default;

  std::string GetSource() override {
    return chrome::kChromeUIMoltAIUpdateHost;
  }

  std::string GetMimeType(const GURL& url) override { return "text/html"; }

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
<title>MoltBrowser Updates</title>
<style>
  :root { color-scheme: dark; }
  body { font-family: -apple-system, system-ui, sans-serif; background:#0d0d0f;
         color:#e8e8ea; margin:0; padding:40px; display:flex;
         justify-content:center; }
  .card { width:100%; max-width:620px; }
  h1 { font-size:22px; margin:0 0 4px; }
  .sub { color:#8a8a92; font-size:13px; margin-bottom:28px; }
  .panel { background:#16161a; border:1px solid #26262c; border-radius:14px;
           padding:24px; margin-bottom:18px; }
  .row { display:flex; align-items:center; justify-content:space-between;
         gap:16px; }
  .vlabel { color:#8a8a92; font-size:12px; text-transform:uppercase;
            letter-spacing:.05em; }
  .vnum { font-size:18px; font-weight:600; }
  #badge { display:inline-block; padding:6px 12px; border-radius:999px;
           font-size:13px; font-weight:600; background:#1f2430; color:#9aa4b2; }
  #status { font-size:15px; margin:18px 0 6px; }
  #notes { white-space:pre-wrap; background:#0f0f12; border:1px solid #26262c;
           border-radius:10px; padding:14px; font-size:13px; color:#c2c2c8;
           max-height:220px; overflow:auto; margin-top:14px; display:none; }
  .bar { height:8px; background:#26262c; border-radius:999px; overflow:hidden;
         margin:14px 0 4px; display:none; }
  .bar > div { height:100%; width:0%; background:#6d5cff; transition:width .2s; }
  .btns { display:flex; gap:12px; margin-top:22px; }
  button { font:inherit; font-weight:600; border:0; border-radius:10px;
           padding:11px 18px; cursor:pointer; }
  .primary { background:#6d5cff; color:#fff; }
  .primary:disabled { background:#34343c; color:#7a7a82; cursor:default; }
  .ghost { background:#26262c; color:#e8e8ea; }
  .toggle { display:flex; align-items:center; gap:10px; font-size:14px; }
  .toggle input { width:18px; height:18px; accent-color:#6d5cff; }
  a { color:#9a8cff; }
  .muted { color:#8a8a92; font-size:12px; margin-top:10px; }
</style>
</head>
<body>
<div class="card">
  <h1>Software Update</h1>
  <div class="sub">MoltBrowser keeps itself up to date automatically.</div>

  <div class="panel">
    <div class="row">
      <div>
        <div class="vlabel">Installed</div>
        <div class="vnum" id="current">&mdash;</div>
      </div>
      <div id="badge">Idle</div>
      <div style="text-align:right">
        <div class="vlabel">Latest</div>
        <div class="vnum" id="latest">&mdash;</div>
      </div>
    </div>

    <div id="status">Checking for updates&hellip;</div>
    <div class="bar" id="bar"><div id="barfill"></div></div>
    <div id="notes"></div>

    <div class="btns">
      <button class="ghost" id="checkBtn" onclick="doCheck()">Check for updates</button>
      <button class="primary" id="actionBtn" onclick="doAction()" disabled>Update now</button>
    </div>
    <div class="muted" id="rellink"></div>
  </div>

  <div class="panel">
    <label class="toggle">
      <input type="checkbox" id="autoUpdate" checked onchange="setAuto()">
      <span>Download &amp; install updates automatically</span>
    </label>
    <div class="muted">When on, MoltBrowser downloads new versions in the
      background and installs them on restart.</div>
  </div>
</div>

<script>
var idCounter = 0, pendingCbs = {};
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
// C++ FireWebUIListener(event, v) invokes cr.webUIListenerCallback — register
// handlers into a map it reads. (The previous document-event polyfill was
// wrong: FireWebUIListener does NOT dispatch DOM events, so 'update-state'
// pushes never arrived and "Check for updates" appeared to do nothing — the
// page only ever populated from the initial getUpdateState promise.)
var webUIListeners = {};
cr.addWebUIListener = function(event, fn) {
  (webUIListeners[event] = webUIListeners[event] || []).push(fn);
};
cr.webUIListenerCallback = function(event) {
  var args = Array.prototype.slice.call(arguments, 1);
  var ls = webUIListeners[event] || [];
  for (var i = 0; i < ls.length; i++) ls[i].apply(null, args);
};

var lastState = 'idle';

function doCheck() { chrome.send('updateCheck'); }
function setAuto() {
  chrome.send('updateSetAuto', [document.getElementById('autoUpdate').checked]);
}
function doAction() {
  if (lastState === 'downloaded') { chrome.send('updateInstall'); }
  else { chrome.send('updateDownload'); }
}

function render(s) {
  lastState = s.state;
  document.getElementById('current').textContent = s.current_version || '—';
  document.getElementById('latest').textContent = s.latest_version || '—';
  document.getElementById('autoUpdate').checked = !!s.auto_update;

  var badge = document.getElementById('badge');
  var status = document.getElementById('status');
  var action = document.getElementById('actionBtn');
  var checkBtn = document.getElementById('checkBtn');
  var bar = document.getElementById('bar');
  var fill = document.getElementById('barfill');
  var notes = document.getElementById('notes');
  var rel = document.getElementById('rellink');

  bar.style.display = 'none';
  action.disabled = true;
  checkBtn.disabled = false;
  var bg = '#1f2430', fg = '#9aa4b2', label = 'Idle';

  if (s.state === 'checking') {
    label = 'Checking'; status.textContent = 'Checking for updates…';
    checkBtn.disabled = true;
  } else if (s.state === 'uptodate') {
    bg = '#15301f'; fg = '#4ade80'; label = 'Up to date';
    status.textContent = 'You’re running the latest version.';
  } else if (s.state === 'available') {
    bg = '#2a2410'; fg = '#fbbf24'; label = 'Update available';
    status.textContent = 'Version ' + s.latest_version + ' is available.';
    action.textContent = 'Download & install'; action.disabled = false;
  } else if (s.state === 'downloading') {
    bg = '#1a2233'; fg = '#7aa2ff'; label = 'Downloading';
    status.textContent = 'Downloading version ' + s.latest_version + '…';
    bar.style.display = 'block';
    fill.style.width = (s.percent >= 0 ? s.percent : 30) + '%';
  } else if (s.state === 'downloaded') {
    bg = '#15301f'; fg = '#4ade80'; label = 'Ready to install';
    status.textContent = 'Version ' + s.latest_version +
      ' is ready. Restart to finish updating.';
    action.textContent = 'Restart to update'; action.disabled = false;
  } else if (s.state === 'installing') {
    label = 'Installing'; status.textContent = 'Restarting to apply update…';
    checkBtn.disabled = true;
  } else if (s.state === 'error') {
    bg = '#311616'; fg = '#f87171'; label = 'Error';
    status.textContent = s.error || 'Update failed.';
    if (s.update_available) { action.textContent = 'Retry'; action.disabled = false; }
  }
  badge.style.background = bg; badge.style.color = fg; badge.textContent = label;

  if (s.release_notes) {
    notes.style.display = 'block'; notes.textContent = s.release_notes;
  } else { notes.style.display = 'none'; }

  rel.innerHTML = s.release_url ?
    ('<a href="' + s.release_url + '" target="_blank">View release on GitHub</a>') : '';
}

cr.addWebUIListener('update-state', render);
sendWithPromise('updateGetState').then(render);
// Kick a fresh check whenever the page is opened.
chrome.send('updateCheck');
</script>
</body>
</html>)HTML";

    std::move(callback).Run(
        base::MakeRefCounted<base::RefCountedString>(std::move(html)));
  }

  bool ShouldReplaceExistingSource() override { return true; }
};

}  // namespace

MoltAIUpdateUI::MoltAIUpdateUI(content::WebUI* web_ui)
    : content::WebUIController(web_ui) {
  content::URLDataSource::Add(
      web_ui->GetWebContents()->GetBrowserContext(),
      std::make_unique<MoltAIUpdateDataSource>());
  web_ui->AddMessageHandler(std::make_unique<MoltAIUpdateHandler>());
}

MoltAIUpdateUI::~MoltAIUpdateUI() = default;
