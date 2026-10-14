// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/webui/molt_ai/molt_ai_memory_ui.h"

#include <cstdint>  // for int64_t — required on Linux under -fmodules
#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/ref_counted_memory.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/memory/memory_service.h"
#include "chrome/browser/molt_ai/memory/memory_service_factory.h"
#include "chrome/browser/molt_ai/memory/memory_types.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/webui/molt_ai/molt_webui_csp.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_message_handler.h"

namespace {

using molt_ai::memory::Document;
using molt_ai::memory::MemoryService;
using molt_ai::memory::MemoryServiceFactory;
using molt_ai::memory::QueryHit;

class MoltAIMemoryHandler : public content::WebUIMessageHandler {
 public:
  MoltAIMemoryHandler() = default;
  ~MoltAIMemoryHandler() override = default;

  void RegisterMessages() override {
    web_ui()->RegisterMessageCallback(
        "memQuery",
        base::BindRepeating(&MoltAIMemoryHandler::HandleQuery,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "memStats",
        base::BindRepeating(&MoltAIMemoryHandler::HandleStats,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "memListRecent",
        base::BindRepeating(&MoltAIMemoryHandler::HandleListRecent,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "memDeleteDoc",
        base::BindRepeating(&MoltAIMemoryHandler::HandleDeleteDoc,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "memDeleteDomain",
        base::BindRepeating(&MoltAIMemoryHandler::HandleDeleteDomain,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "memClearAll",
        base::BindRepeating(&MoltAIMemoryHandler::HandleClearAll,
                            base::Unretained(this)));
    // Opt-in / pause / retention controls (P17).
    web_ui()->RegisterMessageCallback(
        "memGetSettings",
        base::BindRepeating(&MoltAIMemoryHandler::HandleGetSettings,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "memSetSettings",
        base::BindRepeating(&MoltAIMemoryHandler::HandleSetSettings,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "memDismissNotice",
        base::BindRepeating(&MoltAIMemoryHandler::HandleDismissNotice,
                            base::Unretained(this)));
  }

 private:
  MemoryService* GetService() {
    Profile* p = Profile::FromBrowserContext(
        web_ui()->GetWebContents()->GetBrowserContext());
    return MemoryServiceFactory::GetForProfile(p);
  }

  void Reply(const std::string& callback_id, base::Value v) {
    ResolveJavascriptCallback(base::Value(callback_id), std::move(v));
  }

  void HandleQuery(const base::ListValue& args) {
    AllowJavascript();
    CHECK_GE(args.size(), 3u);
    const std::string callback_id = args[0].GetString();
    const std::string query =
        args[1].is_string() ? args[1].GetString() : "";
    int top_k = args[2].is_int() ? args[2].GetInt() : 10;

    MemoryService* svc = GetService();
    if (!svc || query.empty()) {
      base::DictValue r;
      r.Set("hits", base::ListValue());
      Reply(callback_id, base::Value(std::move(r)));
      return;
    }
    auto weak_this = weak_factory_.GetWeakPtr();
    svc->Query(query, top_k, base::BindOnce(
        [](base::WeakPtr<MoltAIMemoryHandler> self, std::string cb_id,
           std::vector<QueryHit> hits) {
          if (!self) return;
          base::ListValue arr;
          for (const auto& h : hits) {
            base::DictValue d;
            d.Set("url", h.url);
            d.Set("title", h.title);
            d.Set("snippet", h.snippet);
            d.Set("score", h.score);
            d.Set("visited_at", static_cast<double>(h.visited_at_unix));
            arr.Append(std::move(d));
          }
          base::DictValue r;
          r.Set("hits", std::move(arr));
          self->Reply(cb_id, base::Value(std::move(r)));
        },
        weak_this, callback_id));
  }

  void HandleStats(const base::ListValue& args) {
    AllowJavascript();
    CHECK_GE(args.size(), 1u);
    const std::string callback_id = args[0].GetString();
    MemoryService* svc = GetService();
    if (!svc) {
      base::DictValue r;
      r.Set("docs", 0);
      r.Set("chunks", 0);
      Reply(callback_id, base::Value(std::move(r)));
      return;
    }
    auto weak_this = weak_factory_.GetWeakPtr();
    svc->GetStats(base::BindOnce(
        [](base::WeakPtr<MoltAIMemoryHandler> self, std::string cb_id,
           MemoryService::Stats stats) {
          if (!self) return;
          base::DictValue r;
          r.Set("docs", stats.doc_count);
          r.Set("chunks", stats.chunk_count);
          // Only the newest kHotIndexCap chunks are semantically searchable;
          // the page says so instead of implying everything is.
          r.Set("searchable", stats.searchable_chunk_count);
          self->Reply(cb_id, base::Value(std::move(r)));
        },
        weak_this, callback_id));
  }

  // Settings snapshot. |show_opt_in_notice| is true for profiles that have
  // pages from before memory became opt-in and haven't decided yet.
  void ReplySettings(const std::string& callback_id) {
    MemoryService* svc = GetService();
    if (!svc) {
      base::DictValue r;
      r.Set("available", false);
      Reply(callback_id, base::Value(std::move(r)));
      return;
    }
    auto weak_this = weak_factory_.GetWeakPtr();
    svc->GetStats(base::BindOnce(
        [](base::WeakPtr<MoltAIMemoryHandler> self, std::string cb_id,
           MemoryService::Stats stats) {
          if (!self) return;
          MemoryService* s = self->GetService();
          base::DictValue r;
          r.Set("available", s != nullptr);
          if (!s) {
            self->Reply(cb_id, base::Value(std::move(r)));
            return;
          }
          r.Set("enabled", s->IsEnabled());
          r.Set("paused", s->IsPaused());
          r.Set("retention_days", s->GetRetentionDays());
          r.Set("show_opt_in_notice", !s->IsOptInNoticeDismissed() &&
                                          !s->IsEnabled() &&
                                          stats.doc_count > 0);
          self->Reply(cb_id, base::Value(std::move(r)));
        },
        weak_this, callback_id));
  }

  void HandleGetSettings(const base::ListValue& args) {
    AllowJavascript();
    if (args.empty() || !args[0].is_string()) return;
    ReplySettings(args[0].GetString());
  }

  // Args: [callback_id, {enabled?: bool, paused?: bool,
  //                      retention_days?: int}]
  void HandleSetSettings(const base::ListValue& args) {
    AllowJavascript();
    if (args.empty() || !args[0].is_string()) return;
    const std::string callback_id = args[0].GetString();
    MemoryService* svc = GetService();
    if (svc && args.size() >= 2 && args[1].is_dict()) {
      const base::DictValue& d = args[1].GetDict();
      if (auto v = d.FindBool("enabled")) svc->SetEnabled(*v);
      if (auto v = d.FindBool("paused")) svc->SetPaused(*v);
      if (auto v = d.FindInt("retention_days")) svc->SetRetentionDays(*v);
    }
    ReplySettings(callback_id);
  }

  void HandleDismissNotice(const base::ListValue& args) {
    AllowJavascript();
    if (args.empty() || !args[0].is_string()) return;
    if (MemoryService* svc = GetService()) svc->DismissOptInNotice();
    ReplySettings(args[0].GetString());
  }

  void HandleListRecent(const base::ListValue& args) {
    AllowJavascript();
    CHECK_GE(args.size(), 2u);
    const std::string callback_id = args[0].GetString();
    int limit = args[1].is_int() ? args[1].GetInt() : 50;
    MemoryService* svc = GetService();
    if (!svc) {
      base::DictValue r;
      r.Set("docs", base::ListValue());
      Reply(callback_id, base::Value(std::move(r)));
      return;
    }
    auto weak_this = weak_factory_.GetWeakPtr();
    svc->ListRecent(limit, base::BindOnce(
        [](base::WeakPtr<MoltAIMemoryHandler> self, std::string cb_id,
           std::vector<Document> docs) {
          if (!self) return;
          base::ListValue arr;
          for (const auto& d : docs) {
            base::DictValue x;
            x.Set("doc_id", static_cast<double>(d.doc_id));
            x.Set("url", d.url);
            x.Set("title", d.title);
            x.Set("visited_at", static_cast<double>(d.visited_at_unix));
            x.Set("word_count", d.word_count);
            arr.Append(std::move(x));
          }
          base::DictValue r;
          r.Set("docs", std::move(arr));
          self->Reply(cb_id, base::Value(std::move(r)));
        },
        weak_this, callback_id));
  }

  void HandleDeleteDoc(const base::ListValue& args) {
    AllowJavascript();
    CHECK_GE(args.size(), 2u);
    const std::string callback_id = args[0].GetString();
    int64_t doc_id = 0;
    if (args[1].is_double()) doc_id = static_cast<int64_t>(args[1].GetDouble());
    else if (args[1].is_int()) doc_id = args[1].GetInt();
    MemoryService* svc = GetService();
    if (!svc || doc_id == 0) {
      base::DictValue r;
      r.Set("success", false);
      Reply(callback_id, base::Value(std::move(r)));
      return;
    }
    auto weak_this = weak_factory_.GetWeakPtr();
    svc->DeleteDocument(doc_id, base::BindOnce(
        [](base::WeakPtr<MoltAIMemoryHandler> self, std::string cb_id,
           bool ok) {
          if (!self) return;
          base::DictValue r;
          r.Set("success", ok);
          self->Reply(cb_id, base::Value(std::move(r)));
        },
        weak_this, callback_id));
  }

  void HandleDeleteDomain(const base::ListValue& args) {
    AllowJavascript();
    CHECK_GE(args.size(), 2u);
    const std::string callback_id = args[0].GetString();
    const std::string domain =
        args[1].is_string() ? args[1].GetString() : "";
    MemoryService* svc = GetService();
    if (!svc || domain.empty()) {
      base::DictValue r;
      r.Set("deleted", 0);
      Reply(callback_id, base::Value(std::move(r)));
      return;
    }
    auto weak_this = weak_factory_.GetWeakPtr();
    svc->DeleteByDomain(domain, base::BindOnce(
        [](base::WeakPtr<MoltAIMemoryHandler> self, std::string cb_id,
           int n) {
          if (!self) return;
          base::DictValue r;
          r.Set("deleted", n);
          self->Reply(cb_id, base::Value(std::move(r)));
        },
        weak_this, callback_id));
  }

  void HandleClearAll(const base::ListValue& args) {
    AllowJavascript();
    CHECK_GE(args.size(), 1u);
    const std::string callback_id = args[0].GetString();
    MemoryService* svc = GetService();
    if (!svc) {
      base::DictValue r;
      r.Set("success", false);
      Reply(callback_id, base::Value(std::move(r)));
      return;
    }
    auto weak_this = weak_factory_.GetWeakPtr();
    svc->ClearAll(base::BindOnce(
        [](base::WeakPtr<MoltAIMemoryHandler> self, std::string cb_id,
           bool ok) {
          if (!self) return;
          base::DictValue r;
          r.Set("success", ok);
          self->Reply(cb_id, base::Value(std::move(r)));
        },
        weak_this, callback_id));
  }

  base::WeakPtrFactory<MoltAIMemoryHandler> weak_factory_{this};
};

constexpr char kMemoryPageJs[] = R"JS(// molt://memory page script, served as /app.js under the strict molt
// WebUI CSP (molt_webui_csp.h). The page never writes HTML strings, so
// Trusted Types allow no policies at all.
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
  var cb = pendingCbs[id];
  if (cb) { delete pendingCbs[id]; ok ? cb.resolve(resp) : cb.reject(resp); }
};

function showToast(m, isErr) {
  var t = document.getElementById('toast');
  t.textContent = m;
  t.className = 'toast show' + (isErr ? ' error' : '');
  setTimeout(function(){ t.className = 'toast' + (isErr ? ' error' : ''); }, 2200);
}

// Results are built with DOM APIs, never HTML strings: page titles,
// URLs and snippets all come from pages the user visited. (The old
// esc() escaped < > & but not quotes and was spliced into href="...".)
function el(tag, cls, text) {
  var e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text != null) e.textContent = String(text);
  return e;
}

// Only http(s) URLs become links; anything else (javascript:, data:,
// file:, chrome:) is shown as plain text.
function safeHttpHref(u) {
  try {
    var p = new URL(String(u));
    return (p.protocol === 'http:' || p.protocol === 'https:') ? p.href : null;
  } catch (e) {
    return null;
  }
}

function titleRow(url, title) {
  var row = el('div', 'hit-title');
  var href = safeHttpHref(url);
  if (href) {
    var a = el('a', null, title || url);
    a.href = href;
    a.target = '_blank';
    a.rel = 'noopener noreferrer';
    row.appendChild(a);
  } else {
    row.textContent = title || url || '';
  }
  return row;
}

function showEmpty(container, text) {
  container.replaceChildren(el('div', 'empty', text));
}

function fmtDate(unix) {
  if (!unix) return '';
  return new Date(unix * 1000).toLocaleString();
}

function fmtHost(url) {
  try { return new URL(url).host; } catch(e) { return url; }
}

function refreshStats() {
  sendWithPromise('memStats').then(function(r){
    document.getElementById('doc-count').textContent = r.docs;
    document.getElementById('chunk-count').textContent = r.chunks;
    // Be explicit when older chunks fall outside the searchable window.
    document.getElementById('searchable-note').textContent =
        (r.searchable < r.chunks) ? '(newest ' + r.searchable + ' searchable)'
                                  : '';
  });
}

// ---- Opt-in / pause / retention (P17) ----
function renderMemSettings(s) {
  if (!s || !s.available) {
    document.getElementById('mem-settings').hidden = true;
    return;
  }
  document.getElementById('mem-enabled').checked = !!s.enabled;
  var paused = document.getElementById('mem-paused');
  paused.checked = !!s.paused;
  paused.disabled = !s.enabled;
  document.getElementById('mem-retention').value = String(s.retention_days);
  var st = document.getElementById('mem-state');
  st.textContent = !s.enabled ? 'Off \u2014 nothing is being recorded'
                 : (s.paused ? 'Paused' : 'Recording');
  st.className = 'state' + (s.enabled && !s.paused ? ' on' : '');
  document.getElementById('optin-notice').hidden = !s.show_opt_in_notice;
}

function loadMemSettings() {
  sendWithPromise('memGetSettings').then(renderMemSettings);
}

function setMemSettings(change) {
  sendWithPromise('memSetSettings', change).then(function(s){
    renderMemSettings(s);
    refreshStats();
  });
}

function renderRecents(docs) {
  document.getElementById('results-title').textContent = 'Recent pages';
  document.getElementById('timing').textContent = '';
  var results = document.getElementById('results');
  if (!docs.length) {
    showEmpty(results, 'No pages captured yet. Browse a few sites ' +
        '(not bank/healthcare/login — those are skipped by default).');
    return;
  }
  results.replaceChildren.apply(results, docs.map(function(d){
    var hit = el('div', 'hit');
    hit.appendChild(titleRow(d.url, d.title));
    var meta = el('div', 'hit-meta',
        fmtHost(d.url) + ' \u00b7 ' + fmtDate(d.visited_at) + ' \u00b7 ' +
        (Number(d.word_count) || 0) + ' words ');
    var del = el('a', 'hit-delete', 'delete');
    del.href = '#';
    del.style.color = '#666';
    del.style.marginLeft = '8px';
    del.addEventListener('click', function(e) {
      e.preventDefault();
      delDoc(d.doc_id);
    });
    meta.appendChild(del);
    hit.appendChild(meta);
    return hit;
  }));
}

function renderHits(hits, ms) {
  document.getElementById('results-title').textContent = 'Search results';
  var timing = document.getElementById('timing');
  timing.replaceChildren('query in ', el('b', null, ms.toFixed(1) + 'ms'));
  var results = document.getElementById('results');
  if (!hits.length) {
    showEmpty(results, 'No matches yet.');
    return;
  }
  results.replaceChildren.apply(results, hits.map(function(h){
    var hit = el('div', 'hit');
    hit.appendChild(titleRow(h.url, h.title));
    var meta = el('div', 'hit-meta',
        fmtHost(h.url) + ' \u00b7 ' + fmtDate(h.visited_at) + ' \u00b7 ');
    meta.appendChild(el('span', 'score',
        'cosine ' + (Number(h.score) || 0).toFixed(3)));
    hit.appendChild(meta);
    hit.appendChild(el('div', 'hit-snippet', h.snippet));
    return hit;
  }));
}

function runSearch() {
  var q = document.getElementById('q').value.trim();
  if (!q) { loadRecents(); return; }
  var t0 = performance.now();
  sendWithPromise('memQuery', q, 15).then(function(r){
    var ms = performance.now() - t0;
    renderHits(r.hits || [], ms);
  });
}

function loadRecents() {
  sendWithPromise('memListRecent', 30).then(function(r){
    renderRecents(r.docs || []);
  });
}

function delDoc(docId) {
  if (!confirm('Forget this page?')) return;
  sendWithPromise('memDeleteDoc', docId).then(function(r){
    if (r.success) { showToast('Forgotten'); refreshStats(); loadRecents(); }
    else { showToast('Delete failed', true); }
  });
}

function deleteDomain() {
  var d = document.getElementById('dom-del').value.trim();
  if (!d) { showToast('Type a domain first', true); return; }
  if (!confirm('Forget every page from ' + d + '?')) return;
  sendWithPromise('memDeleteDomain', d).then(function(r){
    showToast('Forgot ' + (r.deleted || 0) + ' pages');
    document.getElementById('dom-del').value = '';
    refreshStats();
    loadRecents();
  });
}

function clearAll() {
  if (!confirm('Wipe the entire memory? This cannot be undone.')) return;
  sendWithPromise('memClearAll').then(function(r){
    showToast(r.success ? 'Memory wiped' : 'Wipe failed', !r.success);
    refreshStats();
    loadRecents();
  });
}

document.getElementById('q').addEventListener('keydown', function(e){
  if (e.key === 'Enter') runSearch();
});
document.getElementById('search-btn').addEventListener('click', runSearch);
document.getElementById('forget-domain-btn').addEventListener('click',
                                                              deleteDomain);
document.getElementById('wipe-btn').addEventListener('click', clearAll);
document.getElementById('mem-enabled').addEventListener('change', function(e) {
  setMemSettings({enabled: e.target.checked});
});
document.getElementById('mem-paused').addEventListener('change', function(e) {
  setMemSettings({paused: e.target.checked});
});
document.getElementById('mem-retention').addEventListener('change',
    function(e) {
      setMemSettings({retention_days: parseInt(e.target.value, 10)});
    });
document.getElementById('notice-enable-btn').addEventListener('click',
    function() { setMemSettings({enabled: true}); });
document.getElementById('notice-dismiss-btn').addEventListener('click',
    function() {
      sendWithPromise('memDismissNotice').then(renderMemSettings);
    });

loadMemSettings();
refreshStats();
loadRecents();
// Poll stats every 5s so counts update as the user browses in
// background tabs.
setInterval(refreshStats, 5000);
)JS";

class MoltAIMemoryDataSource : public content::URLDataSource {
 public:
  MoltAIMemoryDataSource() = default;
  ~MoltAIMemoryDataSource() override = default;

  std::string GetSource() override { return "molt-memory"; }
  std::string GetMimeType(const GURL& url) override {
    return molt_webui::MimeTypeFor(url);
  }
  bool ShouldServeMimeTypeAsContentTypeHeader() override { return true; }

  // Strict molt WebUI CSP; this page never writes HTML.
  std::string GetContentSecurityPolicy(
      const network::mojom::CSPDirectiveName directive) override {
    if (std::optional<std::string> csp =
            molt_webui::PageContentSecurityPolicy(
                directive, molt_webui::TrustedTypesMode::kNoPolicies)) {
      return *csp;
    }
    return content::URLDataSource::GetContentSecurityPolicy(directive);
  }

  void StartDataRequest(
      const GURL& url,
      const content::WebContents::Getter& wc_getter,
      content::URLDataSource::GotDataCallback callback) override {
    if (molt_webui::IsAppJsRequest(url)) {
      std::move(callback).Run(base::MakeRefCounted<base::RefCountedString>(
          std::string(kMemoryPageJs)));
      return;
    }
    std::string html = R"HTML(
<!DOCTYPE html>
<html><head>
<meta charset="utf-8">
<title>MoltBrowser Memory</title>
<style>
*{margin:0;padding:0;box-sizing:border-box;}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;
  background:#0a0a0f;color:#e0e0e8;min-height:100vh;}
.header{background:linear-gradient(135deg,#1a1a2e,#16213e);
  padding:18px 28px;display:flex;align-items:center;gap:18px;
  border-bottom:1px solid rgba(255,255,255,0.08);}
.header h1{font-size:22px;font-weight:600;}
.header h1 span{color:#a78bfa;font-weight:700;}
.badge{background:#a78bfa;color:#0a0a0f;font-size:10px;padding:2px 8px;
  border-radius:10px;font-weight:700;text-transform:uppercase;}
.bar{flex:1;}
.container{max-width:1080px;margin:0 auto;padding:32px 28px 64px;}
.intro{color:#888;margin-bottom:24px;line-height:1.5;}
.intro b{color:#ddd;}
.intro code{background:#1a1a2a;padding:2px 6px;border-radius:4px;
  font-family:monospace;color:#a78bfa;}
.searchbar{display:flex;gap:8px;margin-bottom:18px;}
.searchbar input{flex:1;background:#0d0d14;border:1px solid #2a2a3a;
  color:#fff;padding:14px 18px;border-radius:10px;font-size:15px;}
.searchbar input:focus{outline:none;border-color:#a78bfa;}
.btn{background:#a78bfa;border:none;color:#0a0a0f;padding:10px 20px;
  border-radius:8px;font-weight:700;cursor:pointer;font-size:14px;}
.btn:hover{background:#c4b5fd;}
.btn.ghost{background:transparent;border:1px solid #2a2a3a;color:#ccc;
  font-weight:500;}
.btn.ghost:hover{border-color:#a78bfa;color:#fff;}
.btn.red{background:#3a1a1a;color:#f87171;border:1px solid #4a2a2a;}
.btn.red:hover{background:#4a2a2a;}
.stats{display:flex;gap:24px;margin-bottom:24px;color:#888;font-size:13px;}
.stats b{color:#ddd;}
.hit{background:#0d0d14;border:1px solid #1a1a2a;border-radius:10px;
  padding:14px 18px;margin-bottom:10px;}
.hit:hover{border-color:#a78bfa;}
.hit-title{font-size:15px;font-weight:600;margin-bottom:4px;}
.hit-title a{color:#e0e0e8;text-decoration:none;}
.hit-title a:hover{color:#a78bfa;}
.hit-meta{font-size:11px;color:#666;margin-bottom:6px;
  font-family:monospace;}
.hit-meta .score{color:#a78bfa;font-weight:600;}
.hit-snippet{color:#aaa;font-size:13px;line-height:1.5;
  display:-webkit-box;-webkit-line-clamp:3;-webkit-box-orient:vertical;
  overflow:hidden;}
.hit-snippet mark{background:rgba(167,139,250,0.25);color:#fff;
  padding:0 2px;border-radius:2px;}
.section-title{font-size:13px;color:#888;text-transform:uppercase;
  letter-spacing:1px;margin:24px 0 10px;}
.empty{color:#555;padding:32px;text-align:center;font-style:italic;}
.timing{font-size:11px;color:#666;margin-top:6px;font-family:monospace;}
.timing b{color:#a78bfa;}
.controls{margin-top:30px;padding-top:20px;
  border-top:1px solid #1a1a2a;}
.danger{display:flex;gap:8px;flex-wrap:wrap;}
.danger input{background:#0d0d14;border:1px solid #2a2a3a;color:#fff;
  padding:8px 12px;border-radius:6px;font-size:12px;}
.toast{position:fixed;bottom:24px;right:24px;background:#1a3a1a;
  color:#4ade80;padding:12px 18px;border-radius:10px;
  border:1px solid #2a4a2a;font-size:13px;font-weight:600;
  transform:translateY(120px);opacity:0;transition:all 0.25s;
  z-index:1000;}
.toast.show{transform:translateY(0);opacity:1;}
.toast.error{background:#3a1a1a;color:#f87171;border-color:#4a2a2a;}
.notice{background:#1a1730;border:1px solid #4c3d8a;border-radius:10px;
  padding:14px 18px;margin-bottom:20px;color:#ddd;line-height:1.5;
  font-size:13px;}
.notice b{color:#fff;}
.notice .row{margin-top:10px;display:flex;gap:8px;}
.settings{background:#0d0d14;border:1px solid #1a1a2a;border-radius:10px;
  padding:14px 18px;margin-bottom:24px;display:flex;flex-wrap:wrap;
  gap:18px;align-items:center;font-size:13px;color:#ccc;}
.settings label{display:flex;align-items:center;gap:8px;cursor:pointer;}
.settings select{background:#0a0a0f;border:1px solid #2a2a3a;color:#fff;
  padding:6px 8px;border-radius:6px;font-size:12px;}
.settings .state{margin-left:auto;font-size:12px;color:#888;}
.settings .state.on{color:#4ade80;}
[hidden]{display:none !important;}
</style>
</head>
<body>

<div class="header">
  <h1><span>Molt</span>Memory</h1>
  <span class="badge">Local</span>
  <div class="bar"></div>
  <a href="molt://ai/" style="color:#888;text-decoration:none;font-size:13px;">AI Chat</a>
  <a href="molt://ai-automation/" style="color:#888;text-decoration:none;font-size:13px;">Automation</a>
  <a href="molt://ai-settings/" style="color:#888;text-decoration:none;font-size:13px;">Settings</a>
</div>

<div class="container">
  <div class="notice" id="optin-notice" hidden>
    <b>Memory is now opt-in.</b> MoltBrowser no longer records the pages
    you read unless you turn Memory on below. Pages captured earlier are
    still stored on this device (encrypted) until you clear them &mdash;
    use <i>Wipe all memory</i> to remove them.
    <div class="row">
      <button class="btn" id="notice-enable-btn">Turn on Memory</button>
      <button class="btn ghost" id="notice-dismiss-btn">Keep it off</button>
    </div>
  </div>

  <div class="settings" id="mem-settings">
    <label><input type="checkbox" id="mem-enabled"> Record pages I read</label>
    <label><input type="checkbox" id="mem-paused"> Pause recording</label>
    <label>Keep pages for
      <select id="mem-retention">
        <option value="30">30 days</option>
        <option value="90">90 days</option>
        <option value="365">1 year</option>
        <option value="0">Forever</option>
      </select></label>
    <span class="state" id="mem-state"></span>
  </div>

  <p class="intro">
    When Memory is on, pages you read are chunked, embedded, and stored
    <b>on this device</b> (banking, health, login and checkout pages are
    always skipped). Ask in plain English &mdash; we look up the relevant
    chunks with a cosine scan over the newest 50,000 chunks, fetch the
    snippet, and rank. URLs, titles, text and vectors are encrypted at
    rest via the OS keychain. No cloud, no telemetry.
    <br><br>
    Try: <code>that recipe with miso</code>,
    <code>kubernetes operator bug</code>,
    <code>flight to bali deal</code>.
  </p>

  <div class="searchbar">
    <input type="text" id="q" placeholder="Search your reading history..."
           autocomplete="off">
    <button class="btn" id="search-btn">Search</button>
  </div>

  <div class="stats" id="stats">
    <span><b id="doc-count">&middot;</b> documents</span>
    <span><b id="chunk-count">&middot;</b> chunks</span>
    <span id="searchable-note"></span>
    <span id="timing"></span>
  </div>

  <div class="section-title" id="results-title">Recent pages</div>
  <div id="results"></div>

  <div class="controls">
    <div class="section-title">Privacy controls</div>
    <div class="danger">
      <input type="text" id="dom-del" placeholder="domain to forget (e.g. example.com)">
      <button class="btn red" id="forget-domain-btn">Forget domain</button>
      <button class="btn red" id="wipe-btn">Wipe all memory</button>
    </div>
  </div>
</div>

<div class="toast" id="toast"></div>

<script src="app.js"></script>
</body></html>
)HTML";
    std::move(callback).Run(
        base::MakeRefCounted<base::RefCountedString>(std::move(html)));
  }
};

}  // namespace

MoltAIMemoryUI::MoltAIMemoryUI(content::WebUI* web_ui)
    : content::WebUIController(web_ui) {
  content::URLDataSource::Add(
      web_ui->GetWebContents()->GetBrowserContext(),
      std::make_unique<MoltAIMemoryDataSource>());
  web_ui->AddMessageHandler(std::make_unique<MoltAIMemoryHandler>());
}

MoltAIMemoryUI::~MoltAIMemoryUI() = default;
