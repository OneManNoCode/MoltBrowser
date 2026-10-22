// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/webui/molt_ai/molt_ai_memory_ui.h"

#include <algorithm>
#include <cmath>
#include <cstdint>  // for int64_t — required on Linux under -fmodules
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/ref_counted_memory.h"
#include "base/memory/scoped_refptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/memory/embedding_model_manager.h"
#include "chrome/browser/molt_ai/memory/memory_service.h"
#include "chrome/browser/molt_ai/memory/memory_service_factory.h"
#include "chrome/browser/molt_ai/memory/memory_types.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/webui/molt_ai/molt_ai_chat_handler_helpers.h"
#include "chrome/browser/ui/webui/molt_ai/molt_webui_csp.h"
#include "components/omnibox/browser/molt_memory_query.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_message_handler.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "url/gurl.h"

namespace {

using molt_ai::chat_handler_helpers::HasCallbackArgs;
using molt_ai::chat_handler_helpers::StringArg;
using molt_ai::memory::Document;
using molt_ai::memory::EmbeddingModelManager;
using molt_ai::memory::MemoryService;
using molt_ai::memory::MemoryServiceFactory;
using molt_ai::memory::QueryHit;
using molt_ai::memory::SearchResult;
using molt_ai::memory::SemanticIndexStatus;

// The search box's maxlength is 500 characters; anything much longer did
// not come from it.
constexpr size_t kMaxQueryBytes = 2000;
constexpr int kMaxSearchResults = 50;
constexpr int kMaxRecent = 200;

// args[index] as an int in [min_value, max_value], else |fallback|.
int IntArg(const base::ListValue& args,
           size_t index,
           int fallback,
           int min_value,
           int max_value) {
  if (index >= args.size()) return fallback;
  const base::Value& v = args[index];
  double d = 0;
  if (v.is_int()) {
    d = v.GetInt();
  } else if (v.is_double() && std::isfinite(v.GetDouble())) {
    d = v.GetDouble();
  } else {
    return fallback;
  }
  return static_cast<int>(std::clamp(d, static_cast<double>(min_value),
                                     static_cast<double>(max_value)));
}

const char* ModelStateName(EmbeddingModelManager::State state) {
  using State = EmbeddingModelManager::State;
  switch (state) {
    case State::kUnknown:
      return "unknown";
    case State::kNotOffered:
      return "not_offered";
    case State::kNotInstalled:
      return "not_installed";
    case State::kDownloading:
      return "downloading";
    case State::kInstalled:
      return "installed";
    case State::kRemoving:
      return "removing";
  }
  return "unknown";
}

// The on-device search model, for the page's model card.
base::DictValue ModelStatusDict() {
  const EmbeddingModelManager* models = EmbeddingModelManager::GetInstance();
  const molt_ai::memory::EmbeddingModelSpec& spec = models->spec();
  const EmbeddingModelManager::Status& status = models->status();
  base::DictValue d;
  d.Set("state", ModelStateName(status.state));
  d.Set("reason", status.reason);
  d.Set("error", status.error);
  d.Set("received", static_cast<double>(status.received_bytes));
  d.Set("total", static_cast<double>(status.total_bytes));
  d.Set("name", std::string_view(spec.display_name));
  d.Set("company", std::string_view(spec.company));
  d.Set("license_name", std::string_view(spec.license_name));
  d.Set("license_url", std::string_view(spec.license_url));
  d.Set("size_bytes", static_cast<double>(spec.file_size_bytes
                                              ? spec.file_size_bytes
                                              : spec.approx_size_bytes));
  return d;
}

base::DictValue IndexStatusDict(const SemanticIndexStatus& s) {
  base::DictValue d;
  d.Set("model_active", s.model_active);
  d.Set("migrating", s.migrating);
  d.Set("done", s.migration_done);
  d.Set("total", s.migration_total);
  d.Set("pending", s.pending);
  d.Set("background_allowed", s.background_allowed);
  d.Set("model_failed", s.model_failed);
  return d;
}

base::ListValue HitsList(const std::vector<QueryHit>& hits) {
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
  return arr;
}

class MoltAIMemoryHandler : public content::WebUIMessageHandler {
 public:
  MoltAIMemoryHandler() = default;
  ~MoltAIMemoryHandler() override = default;

  void RegisterMessages() override {
    web_ui()->RegisterMessageCallback(
        "memSearch",
        base::BindRepeating(&MoltAIMemoryHandler::HandleSearch,
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
    // Search by meaning: the on-device search model and the index built
    // with it (docs/design/SEMANTIC_SEARCH.md).
    web_ui()->RegisterMessageCallback(
        "memSemanticStatus",
        base::BindRepeating(&MoltAIMemoryHandler::HandleSemanticStatus,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "memModelDownload",
        base::BindRepeating(&MoltAIMemoryHandler::HandleModelDownload,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "memModelCancel",
        base::BindRepeating(&MoltAIMemoryHandler::HandleModelCancel,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "memModelRemove",
        base::BindRepeating(&MoltAIMemoryHandler::HandleModelRemove,
                            base::Unretained(this)));
    // molt://memory/?q=, which the omnibox's @memory shortcut opens.
    web_ui()->RegisterMessageCallback(
        "memInitialQuery",
        base::BindRepeating(&MoltAIMemoryHandler::HandleInitialQuery,
                            base::Unretained(this)));
  }

 private:
  Profile* GetProfile() {
    return Profile::FromBrowserContext(
        web_ui()->GetWebContents()->GetBrowserContext());
  }

  // Null for off-the-record (incl. MoltNet) profiles: memory exists only
  // for regular profiles (MemoryServiceFactory).
  MemoryService* GetService() {
    return MemoryServiceFactory::GetForProfile(GetProfile());
  }

  void Reply(const std::string& callback_id, base::Value v) {
    ResolveJavascriptCallback(base::Value(callback_id), std::move(v));
  }

  void ReplyResult(const std::string& callback_id,
                   bool success,
                   const std::string& error) {
    base::DictValue r;
    r.Set("success", success);
    r.Set("error", error);
    Reply(callback_id, base::Value(std::move(r)));
  }

  // Args: [callback_id, query, top_k]. Replies {hits, model, ok}: one hit
  // per page (its best passage, with a snippet), ranked by the on-device
  // search model when it is in use, else by keyword hashing.
  void HandleSearch(const base::ListValue& args) {
    AllowJavascript();
    if (!HasCallbackArgs(args, 2u, __func__)) return;
    const std::string callback_id = args[0].GetString();
    const std::string query = StringArg(args, 1);
    const int top_k = IntArg(args, 2, 20, 1, kMaxSearchResults);

    MemoryService* svc = GetService();
    if (!svc || query.empty() || query.size() > kMaxQueryBytes) {
      base::DictValue r;
      r.Set("hits", base::ListValue());
      r.Set("model", false);
      r.Set("ok", svc != nullptr && query.size() <= kMaxQueryBytes);
      Reply(callback_id, base::Value(std::move(r)));
      return;
    }
    svc->SearchByMeaning(
        query, top_k,
        base::BindOnce(
            [](base::WeakPtr<MoltAIMemoryHandler> self, std::string cb_id,
               SearchResult result) {
              if (!self) return;
              base::DictValue r;
              r.Set("hits", HitsList(result.hits));
              r.Set("model", result.model);
              r.Set("ok", result.ok);
              self->Reply(cb_id, base::Value(std::move(r)));
            },
            weak_factory_.GetWeakPtr(), callback_id));
  }

  // Args: [callback_id]. Replies {query}: the q parameter of the page's URL
  // (molt://memory/?q=..., which the omnibox's @memory shortcut opens),
  // decoded and made safe by molt_memory::QueryFromUrlQuery() (no control
  // or bidi characters, at most 500 characters), or "". Read from the
  // committed URL here, never from the page; the page only puts it in its
  // search box as a value.
  void HandleInitialQuery(const base::ListValue& args) {
    AllowJavascript();
    if (!HasCallbackArgs(args, 1u, __func__)) return;
    const std::string callback_id = args[0].GetString();
    std::string query;
    if (GetService()) {
      const GURL& url = web_ui()->GetWebContents()->GetLastCommittedURL();
      if (url.has_query()) {
        query = molt_memory::QueryFromUrlQuery(url.query())
                    .value_or(std::string());
      }
    }
    base::DictValue r;
    r.Set("query", query);
    Reply(callback_id, base::Value(std::move(r)));
  }

  // Args: [callback_id, refresh?]. Replies {available, model, index}.
  // |refresh| (on page load) looks at disk again, in case the model file
  // was removed by hand.
  void HandleSemanticStatus(const base::ListValue& args) {
    AllowJavascript();
    if (!HasCallbackArgs(args, 1u, __func__)) return;
    const std::string callback_id = args[0].GetString();
    MemoryService* svc = GetService();
    if (!svc) {
      base::DictValue r;
      r.Set("available", false);
      Reply(callback_id, base::Value(std::move(r)));
      return;
    }
    if (args.size() >= 2 && args[1].is_bool() && args[1].GetBool()) {
      EmbeddingModelManager::GetInstance()->Refresh();
    }
    svc->GetSemanticStatus(base::BindOnce(
        [](base::WeakPtr<MoltAIMemoryHandler> self, std::string cb_id,
           SemanticIndexStatus index) {
          if (!self) return;
          base::DictValue r;
          r.Set("available", true);
          r.Set("model", ModelStatusDict());
          r.Set("index", IndexStatusDict(index));
          self->Reply(cb_id, base::Value(std::move(r)));
        },
        weak_factory_.GetWeakPtr(), callback_id));
  }

  // Args: [callback_id]. Resolves {success, error} when the download ends;
  // the page follows progress with memSemanticStatus.
  void HandleModelDownload(const base::ListValue& args) {
    AllowJavascript();
    if (!HasCallbackArgs(args, 1u, __func__)) return;
    const std::string callback_id = args[0].GetString();
    Profile* profile = GetProfile();
    // Only from a regular profile, through its own network context: never
    // an off-the-record or MoltNet one (which have no memory either).
    if (!profile || profile->IsOffTheRecord() || !GetService()) {
      ReplyResult(callback_id, false,
                  "The search model can only be downloaded from a regular "
                  "window.");
      return;
    }
    scoped_refptr<network::SharedURLLoaderFactory> factory =
        profile->GetDefaultStoragePartition()
            ->GetURLLoaderFactoryForBrowserProcess();
    EmbeddingModelManager::GetInstance()->StartDownload(
        std::move(factory),
        base::BindOnce(
            [](base::WeakPtr<MoltAIMemoryHandler> self, std::string cb_id,
               std::string error) {
              if (!self) return;
              self->ReplyResult(cb_id, error.empty(), error);
            },
            weak_factory_.GetWeakPtr(), callback_id));
  }

  void HandleModelCancel(const base::ListValue& args) {
    AllowJavascript();
    if (!HasCallbackArgs(args, 1u, __func__)) return;
    const std::string callback_id = args[0].GetString();
    if (GetService()) EmbeddingModelManager::GetInstance()->CancelDownload();
    ReplyResult(callback_id, true, std::string());
  }

  void HandleModelRemove(const base::ListValue& args) {
    AllowJavascript();
    if (!HasCallbackArgs(args, 1u, __func__)) return;
    const std::string callback_id = args[0].GetString();
    if (!GetService()) {
      ReplyResult(callback_id, false, std::string());
      return;
    }
    EmbeddingModelManager::GetInstance()->Remove(base::BindOnce(
        [](base::WeakPtr<MoltAIMemoryHandler> self, std::string cb_id,
           bool ok) {
          if (!self) return;
          self->ReplyResult(
              cb_id, ok, ok ? std::string() : "Couldn't remove the model.");
        },
        weak_factory_.GetWeakPtr(), callback_id));
  }

  void HandleStats(const base::ListValue& args) {
    AllowJavascript();
    if (!HasCallbackArgs(args, 1u, __func__)) return;
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
    if (!HasCallbackArgs(args, 1u, __func__)) return;
    ReplySettings(args[0].GetString());
  }

  // Args: [callback_id, {enabled?: bool, paused?: bool,
  //                      retention_days?: int}]
  void HandleSetSettings(const base::ListValue& args) {
    AllowJavascript();
    if (!HasCallbackArgs(args, 1u, __func__)) return;
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
    if (!HasCallbackArgs(args, 1u, __func__)) return;
    if (MemoryService* svc = GetService()) svc->DismissOptInNotice();
    ReplySettings(args[0].GetString());
  }

  void HandleListRecent(const base::ListValue& args) {
    AllowJavascript();
    if (!HasCallbackArgs(args, 1u, __func__)) return;
    const std::string callback_id = args[0].GetString();
    const int limit = IntArg(args, 1, 50, 1, kMaxRecent);
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
    if (!HasCallbackArgs(args, 2u, __func__)) return;
    const std::string callback_id = args[0].GetString();
    // Document ids come back from memListRecent as JS numbers.
    int64_t doc_id = 0;
    if (args[1].is_int()) {
      doc_id = args[1].GetInt();
    } else if (args[1].is_double()) {
      const double d = args[1].GetDouble();
      if (d >= 1 && d <= 9007199254740991.0 && d == std::floor(d)) {
        doc_id = static_cast<int64_t>(d);
      }
    }
    MemoryService* svc = GetService();
    if (!svc || doc_id <= 0) {
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
    if (!HasCallbackArgs(args, 2u, __func__)) return;
    const std::string callback_id = args[0].GetString();
    const std::string domain = StringArg(args, 1);
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
    if (!HasCallbackArgs(args, 1u, __func__)) return;
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

constexpr char kMemoryPageJs[] = R"JS(// molt://memory page script, served as /app.js after the shared
// moltActions prologue under the strict molt WebUI CSP (molt_webui_csp.h).
// The page never writes HTML strings, so Trusted Types allow no policies
// at all: titles, URLs and snippets come from pages the user visited and
// only ever become text.
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

// Results are built with DOM APIs, never HTML strings.
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

function link(href, text) {
  var a = el('a', null, text);
  a.href = href;
  a.target = '_blank';
  a.rel = 'noopener noreferrer';
  return a;
}

function titleRow(url, title) {
  var row = el('div', 'hit-title');
  var href = safeHttpHref(url);
  if (href) {
    row.appendChild(link(href, title || url));
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

function fmtBytes(n) {
  n = Number(n) || 0;
  if (n >= 1e9) return (n / 1e9).toFixed(1) + ' GB';
  return Math.max(1, Math.round(n / 1e6)) + ' MB';
}

function fmtCount(n) {
  return (Number(n) || 0).toLocaleString();
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
  // The @memory tip only where the shortcut exists (a profile with memory).
  document.getElementById('omnibox-tip').hidden = !(s && s.available);
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
  st.textContent = !s.enabled ? 'Off — nothing is being recorded'
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
    loadSemantic(false);
  });
}

// ---- Search by meaning: the on-device search model ----
var semantic = null;  // the last memSemanticStatus reply

function setButton(id, show, text) {
  var b = document.getElementById(id);
  b.hidden = !show;
  if (text) b.textContent = text;
}

function engineText(m, ix) {
  var state = String(m.state || '');
  if (ix.model_active) {
    var t = 'Ranked by meaning, with the on-device search model.';
    if (ix.pending > 0) {
      t += ' ' + fmtCount(ix.pending) + ' new passages are waiting for it.';
    }
    return t;
  }
  if (state === 'installed' && ix.model_failed) {
    return 'The search model couldn’t run, so results are ranked by ' +
           'matching words for now.';
  }
  if (state === 'installed' && ix.migrating) {
    return 'Building the search-by-meaning index: ' + fmtCount(ix.done) +
           ' of ' + fmtCount(ix.total) + ' passages' +
           (ix.background_allowed ? '' : ', paused while Memory is off or paused') +
           '. Until it is done, results are ranked by matching words.';
  }
  if (state === 'installed') return 'Getting the search model ready…';
  if (state === 'not_offered') return 'Results are ranked by matching words.';
  return 'Results are ranked by matching words. Install the on-device ' +
         'search model below to search by meaning.';
}

function modelStateText(m) {
  switch (String(m.state || '')) {
    case 'not_offered':
      return String(m.reason || 'Not available on this device.');
    case 'not_installed':
      return 'Not installed. One download of about ' + fmtBytes(m.size_bytes) +
             (m.error ? '. Last attempt: ' + String(m.error) : '.');
    case 'downloading':
      return 'Downloading… ' + fmtBytes(m.received) + ' of ' +
             fmtBytes(m.total || m.size_bytes);
    case 'installed':
      return 'Installed.';
    case 'removing':
      return 'Removing…';
    default:
      return 'Checking…';
  }
}

function renderSemantic(s) {
  semantic = s || null;
  var box = document.getElementById('search-model');
  var engine = document.getElementById('engine');
  if (!s || !s.available) {
    box.hidden = true;
    engine.textContent = '';
    return;
  }
  var m = s.model || {};
  var ix = s.index || {};
  var state = String(m.state || '');
  box.hidden = false;
  document.getElementById('model-name').textContent =
      String(m.name || 'On-device search model');
  var lic = document.getElementById('model-license');
  var href = safeHttpHref(m.license_url);
  if (m.license_name && href) {
    lic.replaceChildren('License: ', link(href, String(m.license_name)));
  } else {
    lic.textContent = m.license_name ? 'License: ' + String(m.license_name) : '';
  }
  document.getElementById('model-state').textContent = modelStateText(m);
  var progress = document.getElementById('model-progress');
  progress.hidden = state !== 'downloading';
  var total = Number(m.total || m.size_bytes) || 0;
  var pct = total > 0 ? Math.min(100, 100 * (Number(m.received) || 0) / total) : 0;
  document.getElementById('model-progress-bar').style.width = pct.toFixed(1) + '%';
  setButton('model-download-btn', state === 'not_installed',
            'Download (' + fmtBytes(m.size_bytes) + ')');
  setButton('model-cancel-btn', state === 'downloading');
  setButton('model-remove-btn', state === 'installed');
  engine.textContent = engineText(m, ix);
}

function semanticBusy() {
  if (!semantic || !semantic.available) return false;
  var state = String((semantic.model || {}).state || '');
  var ix = semantic.index || {};
  return state === 'downloading' || state === 'removing' ||
         state === 'unknown' || (state === 'installed' && !ix.model_active);
}

function loadSemantic(refresh) {
  return sendWithPromise('memSemanticStatus', !!refresh).then(renderSemantic);
}

function downloadModel() {
  sendWithPromise('memModelDownload').then(function(r){
    if (r && r.success) showToast('Search model installed');
    else if (r && r.error) showToast(String(r.error), true);
    loadSemantic(false);
  });
  // Show "Downloading" at once; the answer above comes when it ends.
  setTimeout(function() { loadSemantic(false); }, 150);
}

function cancelModel() {
  sendWithPromise('memModelCancel').then(function() { loadSemantic(false); });
}

function removeModel() {
  if (!confirm('Remove the on-device search model? Memory goes back to ' +
               'matching words; your pages stay.')) {
    return;
  }
  sendWithPromise('memModelRemove').then(function(r){
    showToast(r && r.success ? 'Search model removed'
                             : String((r && r.error) || 'Remove failed'),
              !(r && r.success));
    loadSemantic(false);
  });
}

// Marks the query's words where they appear in |text|: text nodes and
// <mark> elements, never HTML. One pass over the snippet's words.
function appendHighlighted(parent, text, query) {
  text = String(text == null ? '' : text);
  var words = String(query || '').toLowerCase().split(/[^\p{L}\p{N}]+/u)
      .filter(function(w) { return w.length >= 2; }).slice(0, 16);
  if (!words.length) { parent.textContent = text; return; }
  var re = /[\p{L}\p{N}]+/gu;
  var last = 0;
  var m;
  while ((m = re.exec(text)) !== null) {
    var w = m[0].toLowerCase();
    if (!words.some(function(q) { return w.startsWith(q); })) continue;
    if (m.index > last) {
      parent.appendChild(document.createTextNode(text.slice(last, m.index)));
    }
    parent.appendChild(el('mark', null, m[0]));
    last = m.index + m[0].length;
  }
  if (last < text.length) {
    parent.appendChild(document.createTextNode(text.slice(last)));
  }
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
        fmtHost(d.url) + ' · ' + fmtDate(d.visited_at) + ' · ' +
        (Number(d.word_count) || 0) + ' words ');
    var del = el('a', 'hit-delete', 'delete');
    del.href = '#';
    del.setAttribute('data-on-click', 'deleteDoc');
    del.setAttribute('data-doc-id', String(Number(d.doc_id) || 0));
    meta.appendChild(del);
    hit.appendChild(meta);
    return hit;
  }));
}

function renderHits(r, query, ms) {
  var hits = Array.isArray(r.hits) ? r.hits : [];
  document.getElementById('results-title').textContent = 'Search results';
  var timing = document.getElementById('timing');
  timing.replaceChildren((r.model ? 'by meaning' : 'by matching words') +
                         ' in ', el('b', null, ms.toFixed(0) + ' ms'));
  var results = document.getElementById('results');
  if (r.ok === false) {
    showEmpty(results, 'The search model is busy or couldn’t start. ' +
                       'Try again in a moment.');
    return;
  }
  if (!hits.length) {
    showEmpty(results, 'No matches yet.');
    return;
  }
  results.replaceChildren.apply(results, hits.map(function(h){
    var hit = el('div', 'hit');
    hit.appendChild(titleRow(h.url, h.title));
    hit.appendChild(el('div', 'hit-url', h.url));
    var meta = el('div', 'hit-meta', fmtDate(h.visited_at) + ' · ');
    meta.appendChild(el('span', 'score',
        (r.model ? 'similarity ' : 'cosine ') + (Number(h.score) || 0).toFixed(3)));
    hit.appendChild(meta);
    var snippet = el('div', 'hit-snippet');
    appendHighlighted(snippet, h.snippet, query);
    hit.appendChild(snippet);
    return hit;
  }));
}

var searchSeq = 0;
function runSearch() {
  var q = document.getElementById('q').value.trim();
  if (!q) { loadRecents(); return; }
  var seq = ++searchSeq;
  var t0 = performance.now();
  document.getElementById('timing').textContent = 'searching…';
  sendWithPromise('memSearch', q, 20).then(function(r){
    if (seq !== searchSeq) return;  // a newer search is on its way
    renderHits(r || {}, q, performance.now() - t0);
  });
}

function loadRecents() {
  var seq = ++searchSeq;
  sendWithPromise('memListRecent', 30).then(function(r){
    if (seq !== searchSeq) return;  // a search started meanwhile
    renderRecents((r && r.docs) || []);
  });
}

// molt://memory/?q=...: the omnibox's @memory shortcut opens the page with
// a search. The browser reads and cleans the query from the page's URL; it
// only ever becomes the search box's value.
function loadInitial() {
  sendWithPromise('memInitialQuery').then(function(r){
    var q = (r && typeof r.query === 'string') ? r.query : '';
    if (q) {
      document.getElementById('q').value = q;
      runSearch();
    } else {
      loadRecents();
    }
  }, function() { loadRecents(); });
}

function delDoc(docId) {
  if (!docId) return;
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

moltActions({
  search: function() { runSearch(); },
  searchKey: function(e) { if (e.key === 'Enter') runSearch(); },
  forgetDomain: function() { deleteDomain(); },
  wipe: function() { clearAll(); },
  deleteDoc: function() {
    delDoc(Number(this.getAttribute('data-doc-id')) || 0);
    return false;
  },
  setEnabled: function() { setMemSettings({enabled: this.checked}); },
  setPaused: function() { setMemSettings({paused: this.checked}); },
  setRetention: function() {
    setMemSettings({retention_days: parseInt(this.value, 10)});
  },
  noticeEnable: function() { setMemSettings({enabled: true}); },
  noticeDismiss: function() {
    sendWithPromise('memDismissNotice').then(renderMemSettings);
  },
  downloadModel: function() { downloadModel(); },
  cancelModel: function() { cancelModel(); },
  removeModel: function() { removeModel(); },
});

loadMemSettings();
refreshStats();
loadInitial();
loadSemantic(true);
// Counts update as the user browses in background tabs (every 5 s); the
// model card follows a download or the index build every second.
var ticks = 0;
setInterval(function() {
  ticks++;
  if (ticks % 5 === 0) refreshStats();
  if (semanticBusy() || ticks % 5 === 0) loadSemantic(false);
}, 1000);
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
          std::string(molt_webui::kMoltActionsJs) + kMemoryPageJs));
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
.searchbar{display:flex;gap:8px;margin-bottom:8px;}
.searchbar input{flex:1;background:#0d0d14;border:1px solid #2a2a3a;
  color:#fff;padding:14px 18px;border-radius:10px;font-size:15px;}
.searchbar input:focus{outline:none;border-color:#a78bfa;}
.engine{font-size:12px;color:#888;margin-bottom:14px;line-height:1.5;}
.btn{background:#a78bfa;border:none;color:#0a0a0f;padding:10px 20px;
  border-radius:8px;font-weight:700;cursor:pointer;font-size:14px;}
.btn:hover{background:#c4b5fd;}
.btn.ghost{background:transparent;border:1px solid #2a2a3a;color:#ccc;
  font-weight:500;}
.btn.ghost:hover{border-color:#a78bfa;color:#fff;}
.btn.red{background:#3a1a1a;color:#f87171;border:1px solid #4a2a2a;}
.btn.red:hover{background:#4a2a2a;}
.model{background:#0d0d14;border:1px solid #1a1a2a;border-radius:10px;
  padding:14px 18px;margin-bottom:18px;font-size:13px;color:#ccc;
  line-height:1.5;}
.model-head{display:flex;flex-wrap:wrap;gap:12px;align-items:baseline;}
.model-head b{color:#fff;}
.model-license{font-size:12px;color:#888;}
.model-license a{color:#a78bfa;}
.model-note{font-size:12px;color:#888;margin-top:4px;}
.model .row{margin-top:10px;display:flex;gap:8px;}
.model .btn{padding:7px 14px;font-size:13px;}
.progress{height:6px;background:#1a1a2a;border-radius:3px;margin-top:8px;
  overflow:hidden;}
.progress-bar{height:100%;width:0;background:#a78bfa;transition:width 0.3s;}
.stats{display:flex;gap:24px;margin-bottom:24px;color:#888;font-size:13px;}
.stats b{color:#ddd;}
.hit{background:#0d0d14;border:1px solid #1a1a2a;border-radius:10px;
  padding:14px 18px;margin-bottom:10px;}
.hit:hover{border-color:#a78bfa;}
.hit-title{font-size:15px;font-weight:600;margin-bottom:2px;}
.hit-title a{color:#e0e0e8;text-decoration:none;}
.hit-title a:hover{color:#a78bfa;}
.hit-url{font-size:12px;color:#6b8f71;margin-bottom:4px;white-space:nowrap;
  overflow:hidden;text-overflow:ellipsis;}
.hit-meta{font-size:11px;color:#666;margin-bottom:6px;
  font-family:monospace;}
.hit-meta .score{color:#a78bfa;font-weight:600;}
.hit-delete{color:#666;margin-left:8px;}
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
      <button class="btn" id="notice-enable-btn" data-on-click="noticeEnable">Turn on Memory</button>
      <button class="btn ghost" id="notice-dismiss-btn" data-on-click="noticeDismiss">Keep it off</button>
    </div>
  </div>

  <div class="settings" id="mem-settings">
    <label><input type="checkbox" id="mem-enabled" data-on-change="setEnabled"> Record pages I read</label>
    <label><input type="checkbox" id="mem-paused" data-on-change="setPaused"> Pause recording</label>
    <label>Keep pages for
      <select id="mem-retention" data-on-change="setRetention">
        <option value="30">30 days</option>
        <option value="90">90 days</option>
        <option value="365">1 year</option>
        <option value="0">Forever</option>
      </select></label>
    <span class="state" id="mem-state"></span>
  </div>

  <p class="intro">
    When Memory is on, pages you read are split into passages and stored
    <b>on this device</b>, encrypted (banking, health, login and checkout
    pages are always skipped). <b>Search by meaning</b> finds a page from
    what it was about, in your own words and in any language, using a small
    search model that runs on this device; without it, results are ranked
    by matching words. Only the newest 50,000 passages are searched.
    Nothing is sent to the cloud, and MoltNet windows are never recorded.
    <span id="omnibox-tip" hidden>From the address bar, type
    <code>@memory</code> and what you remember.</span>
    <br><br>
    Try: <code>that recipe with miso</code>,
    <code>kubernetes operator bug</code>,
    <code>flight to bali deal</code>.
  </p>

  <div class="section-title">Search by meaning</div>
  <div class="searchbar">
    <input type="text" id="q" maxlength="500" autocomplete="off"
           placeholder="Describe what you remember reading..."
           data-on-keydown="searchKey">
    <button class="btn" id="search-btn" data-on-click="search">Search</button>
  </div>
  <div class="engine" id="engine"></div>

  <div class="model" id="search-model" hidden>
    <div class="model-head">
      <b id="model-name">On-device search model</b>
      <span class="model-license" id="model-license"></span>
    </div>
    <div id="model-state"></div>
    <div class="model-note">
      Downloaded once from a pinned Hugging Face revision and checked
      (size and SHA-256) before it is used. It runs in a sandbox on this
      device and only reads what Memory already stores.
    </div>
    <div class="progress" id="model-progress" hidden>
      <div class="progress-bar" id="model-progress-bar"></div>
    </div>
    <div class="row">
      <button class="btn" id="model-download-btn" data-on-click="downloadModel" hidden>Download</button>
      <button class="btn ghost" id="model-cancel-btn" data-on-click="cancelModel" hidden>Cancel</button>
      <button class="btn ghost" id="model-remove-btn" data-on-click="removeModel" hidden>Remove</button>
    </div>
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
      <button class="btn red" id="forget-domain-btn" data-on-click="forgetDomain">Forget domain</button>
      <button class="btn red" id="wipe-btn" data-on-click="wipe">Wipe all memory</button>
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
