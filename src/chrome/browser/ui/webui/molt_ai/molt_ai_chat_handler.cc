// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltAIChatHandler: WebUI message handler bridging the AI chat frontend
// to BrowserAIRuntime for local llama.cpp inference.

#include "chrome/browser/ui/webui/molt_ai/molt_ai_chat_handler.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <functional>
#include <memory>
#include <cstdint>  // for int64_t — required on Linux under -fmodules
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/system/sys_info.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "chrome/browser/bookmarks/bookmark_model_factory.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/common/content_settings.h"
#include "components/content_settings/core/common/content_settings_pattern.h"
#include "components/content_settings/core/common/content_settings_types.h"
#include "chrome/browser/molt_ai/automation/agent_inbox_registry.h"
#include "chrome/browser/molt_ai/automation/automation_runner.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/automation/automation_scheduler_factory.h"
#include "chrome/browser/molt_ai/automation/automation_scheduler_service.h"
#include "chrome/browser/molt_ai/automation/automation_storage.h"
#include "chrome/browser/molt_ai/common/molt_blocking_scope.h"
#include "chrome/browser/molt_ai/ocr/ocr_service.h"
#include "chrome/browser/molt_ai/pdf/pdf_text_scraper.h"
#include "chrome/browser/molt_ai/profile/molt_profile_store.h"
#include "chrome/browser/molt_ai/vault/vault_store.h"
#include "chrome/browser/molt_ai/voice/voice_service.h"
#include "chrome/browser/molt_ai/tor/tor_manager.h"
#include "chrome/browser/molt_ai/tor/tor_service.h"
#include "chrome/browser/ui/webui/molt_ai/molt_ai_chat_handler_helpers.h"
#include "components/proxy_config/proxy_config_dictionary.h"
#include "components/proxy_config/proxy_config_pref_names.h"
#include "components/prefs/pref_service.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "components/bookmarks/browser/bookmark_model.h"
#include "components/bookmarks/browser/bookmark_node.h"
#include "chrome/browser/molt_ai/memory/memory_service.h"
#include "chrome/browser/molt_ai/memory/memory_service_factory.h"
#include "chrome/browser/molt_ai/memory/memory_types.h"
#include "chrome/browser/molt_ai/runtime/browser_ai_runtime.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/reload_type.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/fetch_api.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"

MoltAIChatHandler::MoltAIChatHandler(Profile* profile)
    : profile_(profile) {}

MoltAIChatHandler::~MoltAIChatHandler() {
  url_loader_.reset();
  if (runtime_) {
    runtime_->CancelGeneration();
    runtime_->Shutdown();
  }
}

void MoltAIChatHandler::RegisterMessages() {
  web_ui()->RegisterMessageCallback(
      "initChat",
      base::BindRepeating(&MoltAIChatHandler::HandleInitChat,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "sendPrompt",
      base::BindRepeating(&MoltAIChatHandler::HandleSendPrompt,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "loadModel",
      base::BindRepeating(&MoltAIChatHandler::HandleLoadModel,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "cancelGeneration",
      base::BindRepeating(&MoltAIChatHandler::HandleCancelGeneration,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "getModelStatus",
      base::BindRepeating(&MoltAIChatHandler::HandleGetModelStatus,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "getPageContext",
      base::BindRepeating(&MoltAIChatHandler::HandleGetPageContext,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "downloadModel",
      base::BindRepeating(&MoltAIChatHandler::HandleDownloadModel,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "deleteModel",
      base::BindRepeating(&MoltAIChatHandler::HandleDeleteModel,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "getPageContent",
      base::BindRepeating(&MoltAIChatHandler::HandleGetPageContent,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "cancelDownload",
      base::BindRepeating(&MoltAIChatHandler::HandleCancelDownload,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "exportHistory",
      base::BindRepeating(&MoltAIChatHandler::HandleExportHistory,
                          base::Unretained(this)));
  // Side panel automation bridge — lets the chat run a one-shot
  // automation action (click / type / scroll / navigate) against
  // the user's currently active tab. Args:
  //   [0] callback_id (string)
  //   [1] action dict {type:"click"|"type"|"scroll"|"navigate",
  //                    selector:string, value:string}
  web_ui()->RegisterMessageCallback(
      "runMoltAction",
      base::BindRepeating(&MoltAIChatHandler::HandleRunMoltAction,
                          base::Unretained(this)));
  // Personal Vector Memory grounding — top-K relevant chunks from
  // the user's full browsing history. Args: [callback_id, query, top_k].
  web_ui()->RegisterMessageCallback(
      "queryMemory",
      base::BindRepeating(&MoltAIChatHandler::HandleQueryMemory,
                          base::Unretained(this)));
  // Tab triage: list every tab in the owning window with a snippet.
  web_ui()->RegisterMessageCallback(
      "listTabsInWindow",
      base::BindRepeating(&MoltAIChatHandler::HandleListTabsInWindow,
                          base::Unretained(this)));
  // Tab triage: bulk action on tabs (close / bookmark / pin).
  web_ui()->RegisterMessageCallback(
      "triageActOnTabs",
      base::BindRepeating(&MoltAIChatHandler::HandleTriageActOnTabs,
                          base::Unretained(this)));
  // Page Watchers: create a scheduled extract+notify Script.
  web_ui()->RegisterMessageCallback(
      "createWatcher",
      base::BindRepeating(&MoltAIChatHandler::HandleCreateWatcher,
                          base::Unretained(this)));
  // Agent Inbox: list currently-running background automation runs.
  web_ui()->RegisterMessageCallback(
      "listActiveAgents",
      base::BindRepeating(&MoltAIChatHandler::HandleListActiveAgents,
                          base::Unretained(this)));
  // PDF chat: fetch a PDF URL, extract text, return to chat.
  web_ui()->RegisterMessageCallback(
      "extractPdfText",
      base::BindRepeating(&MoltAIChatHandler::HandleExtractPdfText,
                          base::Unretained(this)));
  // Smart bookmarks: keyword-rank user's bookmarks by query.
  web_ui()->RegisterMessageCallback(
      "searchBookmarks",
      base::BindRepeating(&MoltAIChatHandler::HandleSearchBookmarks,
                          base::Unretained(this)));
  // Cross-tab Q&A: pull innerText from every tab in the owning Browser.
  web_ui()->RegisterMessageCallback(
      "extractAllTabsText",
      base::BindRepeating(&MoltAIChatHandler::HandleExtractAllTabsText,
                          base::Unretained(this)));
  // Sandbox tab: open a URL in an OTR session.
  web_ui()->RegisterMessageCallback(
      "openSandboxTab",
      base::BindRepeating(&MoltAIChatHandler::HandleOpenSandboxTab,
                          base::Unretained(this)));
  // Privacy heatmap: enumerate third-party resources on the active tab.
  web_ui()->RegisterMessageCallback(
      "getTrackerBreakdown",
      base::BindRepeating(&MoltAIChatHandler::HandleGetTrackerBreakdown,
                          base::Unretained(this)));
  // Domain reputation: visit history for a host.
  web_ui()->RegisterMessageCallback(
      "getDomainReputation",
      base::BindRepeating(&MoltAIChatHandler::HandleGetDomainReputation,
                          base::Unretained(this)));
  // Connection path: what we can show about how traffic flows to a URL.
  web_ui()->RegisterMessageCallback(
      "getConnectionPath",
      base::BindRepeating(&MoltAIChatHandler::HandleGetConnectionPath,
                          base::Unretained(this)));
  // Selective JS off: HostContentSettingsMap toggle for a host.
  web_ui()->RegisterMessageCallback(
      "setJsForDomain",
      base::BindRepeating(&MoltAIChatHandler::HandleSetJsForDomain,
                          base::Unretained(this)));
  // Tor (Phase B.1): probe local tor + read circuits.
  web_ui()->RegisterMessageCallback(
      "getTorStatus",
      base::BindRepeating(&MoltAIChatHandler::HandleGetTorStatus,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "getTorCircuits",
      base::BindRepeating(&MoltAIChatHandler::HandleGetTorCircuits,
                          base::Unretained(this)));
  // Phase B.2: launch / stop tor subprocess + open Tor-routed tab.
  web_ui()->RegisterMessageCallback(
      "launchTor",
      base::BindRepeating(&MoltAIChatHandler::HandleLaunchTor,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "stopTor",
      base::BindRepeating(&MoltAIChatHandler::HandleStopTor,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "openTorTab",
      base::BindRepeating(&MoltAIChatHandler::HandleOpenTorTab,
                          base::Unretained(this)));
  // Exit-country selector: read + set the Tor exit relay country.
  web_ui()->RegisterMessageCallback(
      "getTorExitCountries",
      base::BindRepeating(&MoltAIChatHandler::HandleGetTorExitCountries,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "setTorExitCountry",
      base::BindRepeating(&MoltAIChatHandler::HandleSetTorExitCountry,
                          base::Unretained(this)));
  // Tier 3: receipt extractor + plan-a-task.
  web_ui()->RegisterMessageCallback(
      "appendReceipt",
      base::BindRepeating(&MoltAIChatHandler::HandleAppendReceipt,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "savePlanScript",
      base::BindRepeating(&MoltAIChatHandler::HandleSavePlanScript,
                          base::Unretained(this)));
  // Tier 4: password vault.
  web_ui()->RegisterMessageCallback(
      "vaultList",
      base::BindRepeating(&MoltAIChatHandler::HandleVaultList,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "vaultAdd",
      base::BindRepeating(&MoltAIChatHandler::HandleVaultAdd,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "vaultDelete",
      base::BindRepeating(&MoltAIChatHandler::HandleVaultDelete,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "vaultFindForActive",
      base::BindRepeating(&MoltAIChatHandler::HandleVaultFindForActive,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "vaultAutofill",
      base::BindRepeating(&MoltAIChatHandler::HandleVaultAutofill,
                          base::Unretained(this)));
  // Tier 4: form filler v2 (LLM fallback).
  web_ui()->RegisterMessageCallback(
      "runFormFillAI",
      base::BindRepeating(&MoltAIChatHandler::HandleRunFormFillAI,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "applyFormFillMap",
      base::BindRepeating(&MoltAIChatHandler::HandleApplyFormFillMap,
                          base::Unretained(this)));
  // Tier 5: voice mode — local whisper transcription.
  web_ui()->RegisterMessageCallback(
      "transcribeAudio",
      base::BindRepeating(&MoltAIChatHandler::HandleTranscribeAudio,
                          base::Unretained(this)));
  // Form Filler: load/save the encrypted local profile.
  web_ui()->RegisterMessageCallback(
      "getMoltProfile",
      base::BindRepeating(&MoltAIChatHandler::HandleGetMoltProfile,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "saveMoltProfile",
      base::BindRepeating(&MoltAIChatHandler::HandleSaveMoltProfile,
                          base::Unretained(this)));
  // Form Filler: run the autofill on the active tab.
  web_ui()->RegisterMessageCallback(
      "runFormFill",
      base::BindRepeating(&MoltAIChatHandler::HandleRunFormFill,
                          base::Unretained(this)));
  // AI-grouped history: enumerate recent docs from Personal Vector
  // Memory; the JS side clusters them by keyword overlap.
  web_ui()->RegisterMessageCallback(
      "listMemoryDocs",
      base::BindRepeating(&MoltAIChatHandler::HandleListMemoryDocs,
                          base::Unretained(this)));

  // Autonomous web agent (ReAct loop).
  web_ui()->RegisterMessageCallback(
      "startWebAgent",
      base::BindRepeating(&MoltAIChatHandler::HandleStartWebAgent,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "cancelWebAgent",
      base::BindRepeating(&MoltAIChatHandler::HandleCancelWebAgent,
                          base::Unretained(this)));
}

void MoltAIChatHandler::OnJavascriptAllowed() {
  // Runtime will be created lazily on first use
}

void MoltAIChatHandler::OnJavascriptDisallowed() {
  weak_ptr_factory_.InvalidateWeakPtrs();
  url_loader_.reset();
  if (runtime_) {
    runtime_->CancelGeneration();
  }
}

molt_ai::BrowserAIRuntime* MoltAIChatHandler::GetOrCreateRuntime() {
  if (!runtime_) {
    LOG(INFO) << "[MoltAI] Creating BrowserAIRuntime...";
    runtime_ = std::make_unique<molt_ai::BrowserAIRuntime>();
    runtime_->Initialize();
    LOG(INFO) << "[MoltAI] BrowserAIRuntime initialized";
  }
  return runtime_.get();
}

// ------------------------------------------------------------------
// LoadUserSettings: Read settings.json for inference parameters
// ------------------------------------------------------------------
namespace {

// The JsStringLiteral / SecureClear helpers used to live here as
// anonymous-namespace free functions. They moved to
// molt_ai_chat_handler_helpers.h when the file was subdivided
// (code-review MEDIUM #10). The `using` declarations below keep the
// existing call sites unchanged.
using molt_ai::chat_handler_helpers::JsStringLiteral;
using molt_ai::chat_handler_helpers::SecureClear;

struct MoltAISettings {
  int max_tokens = 512;
  float temperature = 0.7f;
  float top_p = 0.9f;
  int top_k = 40;
  int max_history_messages = 16;
  int max_page_content_chars = 4000;
  bool auto_load_model = true;
  std::string default_model = "tinyllama-1.1b";
  std::string system_prompt =
      "You are MoltBrowser AI, a helpful local AI assistant "
      "built into MoltBrowser. You run entirely on-device for "
      "privacy.\n"
      "\n"
      "You are docked in a side panel next to the user's current web "
      "page. The page's text content is provided in the prompt under "
      "'Active page content:' when available; ground your answers in "
      "it when the user asks about the page.\n"
      "\n"
      "AGENTIC ACTIONS — when the user asks you to interact with the "
      "page (click, fill a form, scroll, navigate, choose a dropdown "
      "option, drag, etc.), emit one or more action tokens on their "
      "own line. The browser dispatches each token in sequence as "
      "soon as it's emitted.\n"
      "\n"
      "CRITICAL RULE — every ACTION token MUST contain real, concrete "
      "values. Never emit angle-bracket placeholders like <full-url>, "
      "<css-selector>, <text-to-type>, <milliseconds>, or <from-user>. "
      "Those are description syntax, NOT something to copy. If you "
      "don't know the real value, ASK the user instead of guessing.\n"
      "\n"
      "Real-world examples (always use this style):\n"
      "  User: 'open nasa.com'\n"
      "  → [[ACTION navigate:https://nasa.com]]\n"
      "    Opening NASA's website.\n"
      "\n"
      "  User: 'search for puppies on google'\n"
      "  → [[ACTION navigate:https://google.com]]\n"
      "    [[ACTION wait-for:input[name=q]|3000]]\n"
      "    [[ACTION type:input[name=q]|puppies]]\n"
      "    [[ACTION click:button[type=submit]]]\n"
      "    Searching Google for puppies.\n"
      "\n"
      "  User: 'scroll down a bit'\n"
      "  → [[ACTION scroll:600]]\n"
      "    Scrolling down 600 pixels.\n"
      "\n"
      "Token grammar (use real values, never the bracketed names):\n"
      "  click:SELECTOR              — e.g. click:#search-button\n"
      "  type:SELECTOR|TEXT          — e.g. type:input[name=q]|hello\n"
      "  select:SELECTOR|VALUE       — e.g. select:#country|US\n"
      "  hover:SELECTOR              — e.g. hover:.tooltip-trigger\n"
      "  right-click:SELECTOR        — e.g. right-click:.menu\n"
      "  drag:SRC|DST                — e.g. drag:#a|#b\n"
      "  scroll:PIXELS               — e.g. scroll:400\n"
      "  navigate:URL                — e.g. navigate:https://nasa.com\n"
      "  wait:MS                     — e.g. wait:1500\n"
      "  wait-for:SELECTOR|MS        — e.g. wait-for:#results|3000\n"
      "\n"
      "If you don't know whether a selector exists (CAPTCHA, A/B test), "
      "use wait-for with a short timeout — if it doesn't appear, the "
      "run fails cleanly and the user sees the failure in the chat.\n"
      "\n"
      "File uploads: dispatch a click on the file input "
      "([[ACTION click:input[type=file]]]). The native picker opens "
      "and the user selects the file themselves.\n"
      "\n"
      "Rules for action tokens:\n"
      "  - One token per line, exact syntax with the double brackets.\n"
      "  - Prefer specific selectors: id (#name), data-testid, "
      "aria-label, name attribute. CSS classes only if necessary.\n"
      "  - Always include a one-sentence natural-language explanation "
      "of what you're doing AFTER the token(s).\n"
      "  - If the user just asks a question, do NOT emit any action "
      "token — only emit when they ask you to DO something.\n"
      "\n"
      "MEMORY GROUNDING — the prompt may include a 'Relevant past "
      "reading:' section pulled from the user's local browsing "
      "history. Use it to answer questions like 'what was that "
      "article about X' or 'compare the restaurants I researched'. "
      "Cite the URL inline.\n"
      "\n"
      "Style:\n"
      "- Be concise, accurate, and directly helpful\n"
      "- Use markdown formatting: **bold**, `code`, ```code blocks```, "
      "bullet lists, and numbered lists\n"
      "- For code questions, provide working examples\n"
      "- For summarization, use bullet points\n"
      "- Never fabricate URLs, citations, or facts\n"
      "- If unsure, say so honestly";
};

MoltAISettings LoadUserSettings() {
  ScopedAllowBlockingForMolt allow_blocking;
  MoltAISettings settings;
  base::FilePath home_dir;
  base::PathService::Get(base::DIR_HOME, &home_dir);
  base::FilePath path =
      home_dir.Append(base::FilePath::FromUTF8Unsafe(".moltbrowser"))
          .Append(base::FilePath::FromUTF8Unsafe("settings.json"));
  std::string contents;
  if (base::ReadFileToString(path, &contents)) {
    auto parsed = base::JSONReader::Read(
        contents, base::JSON_ALLOW_TRAILING_COMMAS);
    if (parsed && parsed->is_dict()) {
      const auto& d = parsed->GetDict();
      if (auto v = d.FindInt("max_tokens"))
        settings.max_tokens = *v;
      if (auto v = d.FindDouble("temperature"))
        settings.temperature = static_cast<float>(*v);
      if (auto v = d.FindDouble("top_p"))
        settings.top_p = static_cast<float>(*v);
      if (auto v = d.FindInt("top_k"))
        settings.top_k = *v;
      if (auto v = d.FindInt("max_history_messages"))
        settings.max_history_messages = *v;
      if (auto v = d.FindInt("max_page_content_chars"))
        settings.max_page_content_chars = *v;
      if (auto v = d.FindBool("auto_load_model"))
        settings.auto_load_model = *v;
      if (auto* v = d.FindString("default_model"))
        settings.default_model = *v;
      if (auto* v = d.FindString("system_prompt"))
        settings.system_prompt = *v;
      LOG(INFO) << "[MoltAI] Loaded user settings: max_tokens="
                << settings.max_tokens
                << " temperature=" << settings.temperature
                << " model=" << settings.default_model;
    }
  }
  return settings;
}
}  // namespace

// ------------------------------------------------------------------
// HandleInitChat: Called when the chat UI loads.
// JS: chrome.send('initChat', [callback_id])
// Returns model status and hardware info.
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleInitChat(const base::ListValue& args) {
  LOG(INFO) << "[MoltAI] HandleInitChat called with " << args.size() << " args";
  AllowJavascript();

  CHECK_GE(args.size(), 1u);
  const std::string callback_id = args[0].GetString();

  auto* runtime = GetOrCreateRuntime();

  // All blocking I/O (model file stat()s + reading ~/.moltbrowser/settings.json)
  // runs on a ThreadPool worker. The reply on the UI thread receives a
  // pre-built settings dict and just assembles the JS result.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::TaskPriority::USER_BLOCKING, base::MayBlock()},
      base::BindOnce(
          [](molt_ai::BrowserAIRuntime* rt) -> base::DictValue {
            rt->RefreshModelStatus();
            MoltAISettings s = LoadUserSettings();
            base::DictValue d;
            d.Set("max_history_messages", s.max_history_messages);
            d.Set("max_page_content_chars", s.max_page_content_chars);
            d.Set("default_model", s.default_model);
            return d;
          },
          base::Unretained(runtime)),
      base::BindOnce(&MoltAIChatHandler::FinishInitChat,
                     weak_ptr_factory_.GetWeakPtr(), callback_id));
}

void MoltAIChatHandler::FinishInitChat(std::string callback_id,
                                       base::DictValue settings_dict) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!IsJavascriptAllowed()) return;

  auto* runtime = GetOrCreateRuntime();

  // Gather model info (now reflects on-disk reality via RefreshModelStatus)
  auto models = runtime->GetAvailableModels();
  auto hw = runtime->GetHardwareCapability();

  base::ListValue model_list;
  for (const auto& m : models) {
    base::DictValue model_dict;
    model_dict.Set("model_id", m.model_id);
    model_dict.Set("display_name", m.display_name);
    model_dict.Set("quantization", m.quantization);
    model_dict.Set("param_billions", m.parameter_count_billions);
    model_dict.Set("is_downloaded", m.is_downloaded);
    model_dict.Set("is_loaded", m.is_loaded);
    model_dict.Set("file_size_mb",
                   static_cast<int>(m.file_size_bytes / (1024 * 1024)));
    model_list.Append(std::move(model_dict));
  }

  base::DictValue result;
  result.Set("models", std::move(model_list));
  result.Set("model_loaded", model_loaded_);
  result.Set("total_ram_gb",
             static_cast<int>(hw.total_ram_bytes / (1024ULL * 1024 * 1024)));
  result.Set("gpu_backend", hw.gpu_backend);
  result.Set("cpu_cores", hw.cpu_cores);
  result.Set("has_gpu", hw.has_gpu_acceleration);

  // Settings were loaded off-thread; merge into result.
  if (auto v = settings_dict.FindInt("max_history_messages"))
    result.Set("max_history_messages", *v);
  if (auto v = settings_dict.FindInt("max_page_content_chars"))
    result.Set("max_page_content_chars", *v);
  if (auto* v = settings_dict.FindString("default_model"))
    result.Set("default_model", *v);

  // Check if this is a first-run (no models downloaded)
  bool any_downloaded = false;
  for (const auto& m : models) {
    if (m.is_downloaded) {
      any_downloaded = true;
      break;
    }
  }
  result.Set("is_first_run", !any_downloaded);

  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(result)));
}

// ------------------------------------------------------------------
// HandleLoadModel: Load a model into memory (runs on background thread)
// JS: chrome.send('loadModel', [callback_id, model_id])
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleLoadModel(const base::ListValue& args) {
  AllowJavascript();

  CHECK_GE(args.size(), 2u);
  const std::string callback_id = args[0].GetString();
  // Type-check second arg — a malicious renderer could send a non-
  // string here and CHECK-crash the browser. Code-review MEDIUM #8.
  const std::string model_id =
      args[1].is_string() ? args[1].GetString() : std::string();

  auto* runtime = GetOrCreateRuntime();

  // Fire a status update to the UI
  FireWebUIListener("model-status", base::Value("loading"),
                    base::Value(model_id));

  // Model loading is blocking (reads large file from disk + GPU upload).
  // Run on ThreadPool and reply on UI thread.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::TaskPriority::USER_BLOCKING, base::MayBlock(),
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(
          [](molt_ai::BrowserAIRuntime* rt, const std::string& mid) -> bool {
            return rt->LoadModel(mid);
          },
          base::Unretained(runtime), model_id),
      base::BindOnce(
          [](base::WeakPtr<MoltAIChatHandler> self, std::string cb_id,
             std::string mid, bool success) {
            if (!self) return;
            self->OnModelLoaded(std::move(cb_id), success,
                                success ? "" : "Failed to load model");
          },
          weak_ptr_factory_.GetWeakPtr(), callback_id, model_id));
}

void MoltAIChatHandler::OnModelLoaded(std::string callback_id, bool success,
                                      const std::string& error) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!IsJavascriptAllowed()) return;

  model_loaded_ = success;
  LOG(INFO) << "[MoltAI] OnModelLoaded: success=" << success
            << " error=" << error;

  base::DictValue result;
  result.Set("success", success);
  result.Set("error", error);

  FireWebUIListener("model-status",
                    base::Value(success ? "ready" : "error"),
                    base::Value(error));

  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(result)));
}

// ------------------------------------------------------------------
// HandleSendPrompt: Run inference (streaming on background thread)
// JS: chrome.send('sendPrompt', [callback_id, prompt_text, history_json])
// history_json is an optional JSON array of {role, content} objects.
// Streams tokens via FireWebUIListener('ai-token', ...)
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleSendPrompt(const base::ListValue& args) {
  LOG(INFO) << "[MoltAI] HandleSendPrompt called";
  AllowJavascript();

  CHECK_GE(args.size(), 2u);
  const std::string callback_id = args[0].GetString();
  std::string prompt_text =
      args[1].is_string() ? args[1].GetString() : std::string();
  // Cap to prevent a compromised WebUI from OOMing the browser with
  // a multi-GB string. 1 MiB is ~250k tokens — well above any
  // legitimate single-turn use. Code-review MEDIUM #8+#9.
  constexpr size_t kMaxPromptBytes = 1 << 20;
  if (prompt_text.size() > kMaxPromptBytes) {
    prompt_text.resize(kMaxPromptBytes);
  }

  // Optional: conversation history as pre-formatted string
  std::string history_text;
  if (args.size() >= 3u && args[2].is_string()) {
    history_text = args[2].GetString();
    // History grows with the conversation but stays bounded by
    // trimHistory on the JS side. Cap defensively at 2 MiB.
    constexpr size_t kMaxHistoryBytes = 2 << 20;
    if (history_text.size() > kMaxHistoryBytes) {
      history_text.resize(kMaxHistoryBytes);
    }
  }

  // Optional: page context (URL + title) for context-aware responses
  std::string page_context;
  if (args.size() >= 4u && args[3].is_string()) {
    page_context = args[3].GetString();
  }

  if (prompt_text.empty()) {
    base::DictValue err;
    err.Set("success", false);
    err.Set("error", "Empty prompt");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(err)));
    return;
  }

  auto* runtime = GetOrCreateRuntime();
  active_prompt_callback_id_ = callback_id;

  // settings.json is read inside the worker — UI thread cannot do file I/O
  // (Chromium's hang watchdog DCHECKs blocking calls).

  // Run settings load + model loading + streaming inference on a background
  // thread. All blocking ops (file read, model file load, llama_decode) must
  // run off the UI thread.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::TaskPriority::USER_BLOCKING, base::MayBlock(),
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(
          [](molt_ai::BrowserAIRuntime* rt, const std::string& prompt,
             const std::string& history, const std::string& page_ctx,
             bool needs_load,
             base::WeakPtr<MoltAIChatHandler> weak_self)
              -> molt_ai::GenerationResult {
            // Load settings on worker thread — file I/O is allowed here.
            MoltAISettings settings = LoadUserSettings();
            // Auto-load model on background thread if needed
            if (needs_load && settings.auto_load_model) {
              LOG(INFO) << "[MoltAI] Auto-loading " << settings.default_model
                        << " on background thread...";
              content::GetUIThreadTaskRunner({})->PostTask(
                  FROM_HERE,
                  base::BindOnce(
                      [](base::WeakPtr<MoltAIChatHandler> self,
                         std::string model_name) {
                        if (self && self->IsJavascriptAllowed()) {
                          self->FireWebUIListener(
                              "model-status",
                              base::Value("loading"),
                              base::Value("Loading " + model_name +
                                          " model..."));
                        }
                      },
                      weak_self, settings.default_model));

              bool loaded = rt->LoadModel(settings.default_model);
              if (!loaded) {
                LOG(ERROR) << "[MoltAI] Failed to load model: "
                           << settings.default_model;
                molt_ai::GenerationResult fail;
                fail.success = false;
                fail.error_message =
                    "Failed to load model '" + settings.default_model +
                    "'. Please download it from the Models panel or check "
                    "~/.moltbrowser/models/";
                // Notify UI of error
                content::GetUIThreadTaskRunner({})->PostTask(
                    FROM_HERE,
                    base::BindOnce(
                        [](base::WeakPtr<MoltAIChatHandler> self,
                           std::string err) {
                          if (self && self->IsJavascriptAllowed()) {
                            self->FireWebUIListener(
                                "model-status", base::Value("error"),
                                base::Value(err));
                          }
                        },
                        weak_self, fail.error_message));
                return fail;
              }
              LOG(INFO) << "[MoltAI] " << settings.default_model
                        << " loaded successfully";

              content::GetUIThreadTaskRunner({})->PostTask(
                  FROM_HERE,
                  base::BindOnce(
                      [](base::WeakPtr<MoltAIChatHandler> self,
                         std::string model_name) {
                        if (self && self->IsJavascriptAllowed()) {
                          self->FireWebUIListener(
                              "model-status", base::Value("ready"),
                              base::Value(model_name));
                        }
                      },
                      weak_self, settings.default_model));
            } else if (needs_load && !settings.auto_load_model) {
              molt_ai::GenerationResult fail;
              fail.success = false;
              fail.error_message =
                  "No model loaded. Open the Models panel to load a model.";
              return fail;
            }

            molt_ai::PromptOptions opts;
            opts.max_tokens = settings.max_tokens;
            opts.temperature = settings.temperature;
            opts.top_p = settings.top_p;
            opts.top_k = settings.top_k;
            opts.stream = true;

            // Format prompt with user's custom system prompt
            std::string system_msg = settings.system_prompt;
            if (!page_ctx.empty()) {
              system_msg += "\nThe user is currently viewing: " + page_ctx;
            }
            std::string formatted_prompt =
                "<|system|>\n" + system_msg + "</s>\n";

            // Include conversation history if provided
            if (!history.empty()) {
              formatted_prompt += history;
            }

            formatted_prompt +=
                "<|user|>\n" + prompt + "</s>\n<|assistant|>\n";

            LOG(INFO) << "[MoltAI] Starting inference...";

            // Use StreamPrompt to send tokens one at a time.
            // Each token is posted back to the UI thread.
            std::string full_text;
            rt->StreamPrompt(
                formatted_prompt,
                [&full_text, weak_self](const std::string& token,
                                        bool is_done) {
                  full_text += token;
                  // Skip sending EOS token to JS — it's not user-visible
                  std::string display_tok = token;
                  if (display_tok == "</s>" || display_tok == "</s>\n") {
                    display_tok = "";
                  }
                  // Post token back to UI thread for FireWebUIListener
                  content::GetUIThreadTaskRunner({})->PostTask(
                      FROM_HERE,
                      base::BindOnce(
                          [](base::WeakPtr<MoltAIChatHandler> self,
                             std::string tok, bool done) {
                            if (self && self->IsJavascriptAllowed()) {
                              self->FireWebUIListener(
                                  "ai-token", base::Value(tok),
                                  base::Value(done));
                            }
                          },
                          weak_self, display_tok, is_done));
                },
                opts);

            LOG(INFO) << "[MoltAI] Inference complete, generated "
                      << full_text.size() << " chars";

            molt_ai::GenerationResult result;
            result.text = full_text;
            result.success = !full_text.empty();
            result.was_cancelled = false;
            return result;
          },
          base::Unretained(runtime), prompt_text, history_text,
          page_context, !model_loaded_,
          weak_ptr_factory_.GetWeakPtr()),
      base::BindOnce(
          [](base::WeakPtr<MoltAIChatHandler> self, std::string cb_id,
             molt_ai::GenerationResult result) {
            if (!self) return;
            if (result.success) {
              self->model_loaded_ = true;
            }
            self->OnPromptComplete(std::move(cb_id), result.success,
                                   result.text, result.error_message);
          },
          weak_ptr_factory_.GetWeakPtr(), callback_id));
}

void MoltAIChatHandler::OnPromptComplete(std::string callback_id,
                                         bool success,
                                         const std::string& full_text,
                                         const std::string& error) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  LOG(INFO) << "[MoltAI] OnPromptComplete: success=" << success
            << " text_len=" << full_text.size()
            << " error=" << error;
  if (!IsJavascriptAllowed()) return;

  base::DictValue result;
  result.Set("success", success);
  result.Set("text", full_text);
  result.Set("error", error);

  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(result)));
  active_prompt_callback_id_.clear();
}

// ------------------------------------------------------------------
// HandleCancelGeneration: Cancel in-progress generation
// JS: chrome.send('cancelGeneration', [])
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleCancelGeneration(
    const base::ListValue& args) {
  AllowJavascript();
  if (runtime_) {
    runtime_->CancelGeneration();
  }
}

// ------------------------------------------------------------------
// HandleGetModelStatus: Get current model loading state
// JS: chrome.send('getModelStatus', [callback_id])
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleGetModelStatus(
    const base::ListValue& args) {
  AllowJavascript();

  CHECK_GE(args.size(), 1u);
  const std::string callback_id = args[0].GetString();

  auto* runtime = GetOrCreateRuntime();
  // Re-scan disk so previously-downloaded GGUF files are detected after
  // every browser restart. Without this, models appear as not-downloaded
  // even though the file is still in ~/.moltbrowser/models/.
  runtime->RefreshModelStatus();
  auto models = runtime->GetAvailableModels();

  base::ListValue model_list;
  std::string loaded_model;
  for (const auto& m : models) {
    base::DictValue d;
    d.Set("model_id", m.model_id);
    d.Set("display_name", m.display_name);
    d.Set("is_downloaded", m.is_downloaded);
    d.Set("is_loaded", m.is_loaded);
    d.Set("file_size_mb",
          static_cast<int>(m.file_size_bytes / (1024 * 1024)));
    model_list.Append(std::move(d));
    if (m.is_loaded) {
      loaded_model = m.display_name;
    }
  }

  base::DictValue result;
  result.Set("models", std::move(model_list));
  result.Set("model_loaded", model_loaded_);
  result.Set("loaded_model_name", loaded_model);

  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(result)));
}

void MoltAIChatHandler::HandleGetPageContext(const base::ListValue& args) {
  AllowJavascript();

  CHECK_GE(args.size(), 1u);
  const std::string callback_id = args[0].GetString();

  base::DictValue result;

  // Find the browser that owns this WebUI and get the active tab
  content::WebContents* webui_contents = web_ui()->GetWebContents();
  Browser* browser = chrome::FindBrowserWithTab(webui_contents);
  if (browser && browser->tab_strip_model()) {
    content::WebContents* active_tab =
        browser->tab_strip_model()->GetActiveWebContents();
    if (active_tab && active_tab != webui_contents) {
      result.Set("url", active_tab->GetLastCommittedURL().spec());
      result.Set("title", base::UTF16ToUTF8(active_tab->GetTitle()));
      result.Set("has_context", true);
    } else {
      result.Set("has_context", false);
    }
  } else {
    result.Set("has_context", false);
  }

  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(result)));
}

// ------------------------------------------------------------------
// HandleDownloadModel: Download a model from HuggingFace
// JS: chrome.send('downloadModel', [callback_id, model_id])
// Streams progress via FireWebUIListener('download-progress', ...)
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleDownloadModel(const base::ListValue& args) {
  AllowJavascript();

  CHECK_GE(args.size(), 2u);
  const std::string callback_id = args[0].GetString();
  const std::string model_id =
      args[1].is_string() ? args[1].GetString() : std::string();

  auto* runtime = GetOrCreateRuntime();
  auto info = runtime->GetModelInfo(model_id);

  if (info.model_id.empty()) {
    base::DictValue result;
    result.Set("success", false);
    result.Set("error", "Unknown model: " + model_id);
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
    return;
  }

  if (info.is_downloaded) {
    base::DictValue result;
    result.Set("success", true);
    result.Set("already_downloaded", true);
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
    return;
  }

  if (url_loader_) {
    base::DictValue result;
    result.Set("success", false);
    result.Set("error", "Another download is already in progress");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
    return;
  }

  // All filesystem checks (DirectoryExists/CreateDirectory/AmountOfFreeDiskSpace
  // /GetFileSize for partial-resume) run on a ThreadPool worker. The network
  // wiring continues on the UI thread once the precheck returns.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::TaskPriority::USER_BLOCKING, base::MayBlock()},
      base::BindOnce(
          [](molt_ai::ModelInfo info) -> base::DictValue {
            base::DictValue r;
            base::FilePath model_dir =
                base::FilePath::FromUTF8Unsafe(info.file_path);
            base::FilePath parent_dir = model_dir.DirName();
            if (!base::DirectoryExists(parent_dir)) {
              base::CreateDirectory(parent_dir);
            }
            auto disk_space_opt =
                base::SysInfo::AmountOfFreeDiskSpace(parent_dir);
            int64_t disk_space = disk_space_opt.value_or(-1);
            int64_t required_bytes =
                static_cast<int64_t>(info.file_size_bytes);
            if (disk_space >= 0 && disk_space < required_bytes) {
              r.Set("ok", false);
              r.Set("error",
                    "Not enough disk space. Need " +
                        std::to_string(required_bytes / (1024 * 1024)) +
                        " MB but only " +
                        std::to_string(disk_space / (1024 * 1024)) +
                        " MB available.");
              return r;
            }
            base::FilePath partial_path =
                base::FilePath::FromUTF8Unsafe(info.file_path + ".partial");
            int64_t existing_bytes = 0;
            auto file_size = base::GetFileSize(partial_path);
            if (file_size.has_value()) {
              existing_bytes = file_size.value();
            }
            r.Set("ok", true);
            r.Set("existing_bytes", static_cast<double>(existing_bytes));
            return r;
          },
          info),
      base::BindOnce(&MoltAIChatHandler::OnDownloadPrecheckComplete,
                     weak_ptr_factory_.GetWeakPtr(), callback_id, model_id,
                     info));
}

void MoltAIChatHandler::OnDownloadPrecheckComplete(
    std::string callback_id,
    std::string model_id,
    molt_ai::ModelInfo info,
    base::DictValue precheck) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!IsJavascriptAllowed()) return;

  bool ok = precheck.FindBool("ok").value_or(false);
  if (!ok) {
    base::DictValue result;
    result.Set("success", false);
    if (auto* err = precheck.FindString("error")) {
      result.Set("error", *err);
    } else {
      result.Set("error", "Download precheck failed");
    }
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
    return;
  }

  int64_t existing_bytes = static_cast<int64_t>(
      precheck.FindDouble("existing_bytes").value_or(0));
  if (existing_bytes > 0) {
    LOG(INFO) << "[MoltAI] Found partial download: " << existing_bytes
              << " bytes, resuming...";
  }

  // Build HuggingFace download URL
  base::FilePath file_path = base::FilePath::FromUTF8Unsafe(info.file_path);
  std::string filename = file_path.BaseName().AsUTF8Unsafe();
  std::string url = "https://huggingface.co/" + info.huggingface_id +
                    "/resolve/main/" + filename;
  base::FilePath partial_path =
      base::FilePath::FromUTF8Unsafe(info.file_path + ".partial");

  LOG(INFO) << "[MoltAI] Starting download: " << url
            << " -> " << info.file_path
            << " (resume from " << existing_bytes << " bytes)";

  auto resource_request = std::make_unique<network::ResourceRequest>();
  resource_request->url = GURL(url);
  resource_request->method = "GET";

  // Add Range header for download resume
  if (existing_bytes > 0) {
    resource_request->headers.SetHeader(
        "Range", "bytes=" + std::to_string(existing_bytes) + "-");
  }

  net::NetworkTrafficAnnotationTag traffic_annotation =
      net::DefineNetworkTrafficAnnotation("molt_ai_model_download", R"(
        semantics {
          sender: "MoltBrowser AI"
          description: "Downloads AI model files from HuggingFace Hub"
          trigger: "User clicks Download button in AI model manager"
          data: "HTTP GET request for model file"
          destination: WEBSITE
        }
        policy {
          cookies_allowed: NO
          setting: "User-initiated model download in MoltBrowser AI settings"
        }
      )");

  url_loader_ = network::SimpleURLLoader::Create(
      std::move(resource_request), traffic_annotation);

  // Allow redirects (HuggingFace redirects to CDN)
  url_loader_->SetAllowHttpErrorResults(false);

  // Progress callback
  url_loader_->SetOnDownloadProgressCallback(
      base::BindRepeating(&MoltAIChatHandler::OnDownloadProgress,
                          weak_ptr_factory_.GetWeakPtr()));

  // Store state for callbacks
  download_callback_id_ = callback_id;
  downloading_model_id_ = model_id;
  download_total_bytes_ = info.file_size_bytes;
  download_resume_bytes_ = existing_bytes;
  download_final_path_ = file_path;
  download_start_time_ = base::TimeTicks::Now();
  download_last_time_ = download_start_time_;
  download_last_bytes_ = existing_bytes;

  // Notify UI of starting progress (including any resumed bytes)
  FireWebUIListener("download-progress",
                    base::Value(model_id),
                    base::Value(static_cast<double>(existing_bytes)),
                    base::Value(static_cast<double>(info.file_size_bytes)));

  // Get URL loader factory from profile
  auto url_loader_factory =
      profile_->GetDefaultStoragePartition()
          ->GetURLLoaderFactoryForBrowserProcess();

  // Download to .partial file, rename to final on completion
  url_loader_->DownloadToFile(
      url_loader_factory.get(),
      base::BindOnce(&MoltAIChatHandler::OnDownloadComplete,
                     weak_ptr_factory_.GetWeakPtr()),
      partial_path);
}

void MoltAIChatHandler::OnDownloadProgress(uint64_t current) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!IsJavascriptAllowed()) return;

  // Add resume offset to show total progress
  uint64_t total_current = current + download_resume_bytes_;

  // Calculate download speed (bytes/sec) and ETA
  base::TimeTicks now = base::TimeTicks::Now();
  double elapsed_sec = (now - download_last_time_).InSecondsF();
  double speed_bps = 0;
  double eta_sec = -1;
  if (elapsed_sec > 0.5) {
    uint64_t bytes_delta = total_current - download_last_bytes_;
    speed_bps = static_cast<double>(bytes_delta) / elapsed_sec;
    download_last_bytes_ = total_current;
    download_last_time_ = now;
    if (speed_bps > 0 && download_total_bytes_ > total_current) {
      eta_sec = static_cast<double>(download_total_bytes_ - total_current) /
                speed_bps;
    }
  }

  FireWebUIListener("download-progress",
                    base::Value(downloading_model_id_),
                    base::Value(static_cast<double>(total_current)),
                    base::Value(static_cast<double>(download_total_bytes_)),
                    base::Value(speed_bps),
                    base::Value(eta_sec));
}

void MoltAIChatHandler::OnDownloadComplete(base::FilePath path) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  bool network_success = !path.empty();
  LOG(INFO) << "[MoltAI] Download complete: network_success=" << network_success
            << " path=" << path.value();

  // Capture net error before we drop url_loader_.
  int net_error = url_loader_ ? url_loader_->NetError() : 0;

  if (!network_success) {
    FinishDownload(false, net_error);
    return;
  }

  // Rename .partial → final and refresh model registry on a worker thread
  // (both touch the filesystem and would DCHECK on the UI thread).
  base::FilePath final_path = download_final_path_;
  auto* runtime = GetOrCreateRuntime();
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::TaskPriority::USER_BLOCKING, base::MayBlock()},
      base::BindOnce(
          [](base::FilePath partial, base::FilePath final,
             molt_ai::BrowserAIRuntime* rt) -> bool {
            if (final.value().empty()) return false;
            base::File::Error error;
            if (!base::ReplaceFile(partial, final, &error)) {
              LOG(ERROR)
                  << "[MoltAI] Failed to rename partial file, error: " << error;
              return false;
            }
            LOG(INFO) << "[MoltAI] Renamed partial to: " << final.value();
            rt->RefreshModelStatus();
            return true;
          },
          path, final_path, base::Unretained(runtime)),
      base::BindOnce(
          [](base::WeakPtr<MoltAIChatHandler> self, int err,
             bool rename_ok) {
            if (self) self->FinishDownload(rename_ok, err);
          },
          weak_ptr_factory_.GetWeakPtr(), net_error));
}

void MoltAIChatHandler::FinishDownload(bool success, int net_error) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  if (IsJavascriptAllowed()) {
    FireWebUIListener("download-complete",
                      base::Value(downloading_model_id_),
                      base::Value(success));

    base::DictValue result;
    result.Set("success", success);
    result.Set("model_id", downloading_model_id_);
    if (!success) {
      result.Set("error", "Download failed (net error: " +
                              std::to_string(net_error) + ")");
    }
    ResolveJavascriptCallback(base::Value(download_callback_id_),
                              base::Value(std::move(result)));
  }

  url_loader_.reset();
  download_callback_id_.clear();
  downloading_model_id_.clear();
  download_total_bytes_ = 0;
  download_resume_bytes_ = 0;
  download_final_path_ = base::FilePath();
}

// ------------------------------------------------------------------
// HandleDeleteModel: Delete a downloaded model file
// JS: chrome.send('deleteModel', [callback_id, model_id])
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleDeleteModel(const base::ListValue& args) {
  AllowJavascript();

  CHECK_GE(args.size(), 2u);
  const std::string callback_id = args[0].GetString();
  const std::string model_id =
      args[1].is_string() ? args[1].GetString() : std::string();

  auto* runtime = GetOrCreateRuntime();

  // DeleteModel touches the filesystem (std::filesystem::remove + GGUF unload).
  // Run on a worker, reply on UI thread.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::TaskPriority::USER_BLOCKING, base::MayBlock()},
      base::BindOnce(
          [](molt_ai::BrowserAIRuntime* rt, std::string mid) {
            return rt->DeleteModel(mid);
          },
          base::Unretained(runtime), model_id),
      base::BindOnce(&MoltAIChatHandler::OnModelDeleted,
                     weak_ptr_factory_.GetWeakPtr(), callback_id, model_id));
}

void MoltAIChatHandler::OnModelDeleted(std::string callback_id,
                                       std::string model_id,
                                       bool success) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!IsJavascriptAllowed()) return;

  if (success) {
    model_loaded_ = false;
  }

  base::DictValue result;
  result.Set("success", success);
  result.Set("model_id", model_id);

  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(result)));
}

// ------------------------------------------------------------------
// HandleGetPageContent: Extract text content from the active tab
// JS: chrome.send('getPageContent', [callback_id])
// Uses JS injection to get document.body.innerText
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleGetPageContent(const base::ListValue& args) {
  AllowJavascript();

  CHECK_GE(args.size(), 1u);
  const std::string callback_id = args[0].GetString();

  content::WebContents* webui_contents = web_ui()->GetWebContents();
  Browser* browser = chrome::FindBrowserWithTab(webui_contents);

  if (!browser || !browser->tab_strip_model()) {
    base::DictValue result;
    result.Set("has_content", false);
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
    return;
  }

  content::WebContents* active_tab =
      browser->tab_strip_model()->GetActiveWebContents();

  if (!active_tab || active_tab == webui_contents) {
    base::DictValue result;
    result.Set("has_content", false);
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
    return;
  }

  // Store tab info before async JS call
  std::string url = active_tab->GetLastCommittedURL().spec();
  std::string title = base::UTF16ToUTF8(active_tab->GetTitle());

  // Inject JS to extract page text (limited to 4000 chars for context window)
  content::RenderFrameHost* rfh = active_tab->GetPrimaryMainFrame();
  if (!rfh) {
    base::DictValue result;
    result.Set("has_content", false);
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
    return;
  }

  rfh->ExecuteJavaScriptForTests(
      u"(function(){"
      u"var text = document.body ? document.body.innerText : '';"
      u"return text.substring(0, 4000);"
      u"})()",
      base::BindOnce(&MoltAIChatHandler::OnPageContentExtracted,
                     weak_ptr_factory_.GetWeakPtr(), callback_id),
      content::ISOLATED_WORLD_ID_CONTENT_END);
}

void MoltAIChatHandler::OnPageContentExtracted(std::string callback_id,
                                                base::Value result) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!IsJavascriptAllowed()) return;

  base::DictValue response;

  // Get the page URL/title from the active tab again
  content::WebContents* webui_contents = web_ui()->GetWebContents();
  Browser* browser = chrome::FindBrowserWithTab(webui_contents);
  if (browser && browser->tab_strip_model()) {
    content::WebContents* active_tab =
        browser->tab_strip_model()->GetActiveWebContents();
    if (active_tab && active_tab != webui_contents) {
      response.Set("url", active_tab->GetLastCommittedURL().spec());
      response.Set("title", base::UTF16ToUTF8(active_tab->GetTitle()));
    }
  }

  if (result.is_string() && !result.GetString().empty()) {
    response.Set("has_content", true);
    response.Set("content", result.GetString());
  } else {
    response.Set("has_content", false);
  }

  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(response)));
}

// ------------------------------------------------------------------
// HandleCancelDownload: Cancel an in-progress model download
// JS: chrome.send('cancelDownload', [])
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleCancelDownload(const base::ListValue& args) {
  AllowJavascript();
  if (url_loader_) {
    LOG(INFO) << "[MoltAI] Cancelling download of " << downloading_model_id_;
    std::string cancelled_model = downloading_model_id_;
    url_loader_.reset();

    // Clean up partial file on a worker thread (DeleteFile blocks on I/O).
    if (!download_final_path_.value().empty()) {
      base::FilePath partial_path = base::FilePath::FromUTF8Unsafe(
          download_final_path_.AsUTF8Unsafe() + ".partial");
      base::ThreadPool::PostTask(
          FROM_HERE, {base::MayBlock()},
          base::BindOnce(
              [](base::FilePath p) { base::DeleteFile(p); }, partial_path));
    }

    FireWebUIListener("download-complete",
                      base::Value(cancelled_model),
                      base::Value(false));

    if (!download_callback_id_.empty()) {
      base::DictValue result;
      result.Set("success", false);
      result.Set("error", "Download cancelled by user");
      result.Set("model_id", cancelled_model);
      ResolveJavascriptCallback(base::Value(download_callback_id_),
                                base::Value(std::move(result)));
    }

    download_callback_id_.clear();
    downloading_model_id_.clear();
    download_total_bytes_ = 0;
    download_resume_bytes_ = 0;
    download_final_path_ = base::FilePath();
  }
}

// ------------------------------------------------------------------
// HandleExportHistory: Export chat history as JSON
// JS: chrome.send('exportHistory', [callback_id, history_json])
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleExportHistory(const base::ListValue& args) {
  AllowJavascript();

  CHECK_GE(args.size(), 2u);
  const std::string callback_id = args[0].GetString();
  std::string history_json =
      args[1].is_string() ? args[1].GetString() : std::string();
  // Cap exported history to keep the on-disk file size bounded.
  // 16 MiB is plenty for a multi-month chat log.
  constexpr size_t kMaxHistoryExportBytes = 16 << 20;
  if (history_json.size() > kMaxHistoryExportBytes) {
    history_json.resize(kMaxHistoryExportBytes);
  }

  // Generate timestamp-based filename on UI thread (no I/O).
  base::Time now = base::Time::Now();
  base::Time::Exploded exploded;
  now.LocalExplode(&exploded);
  std::string filename = "chat-" +
      std::to_string(exploded.year) + "-" +
      (exploded.month < 10 ? "0" : "") + std::to_string(exploded.month) + "-" +
      (exploded.day_of_month < 10 ? "0" : "") +
      std::to_string(exploded.day_of_month) + "-" +
      (exploded.hour < 10 ? "0" : "") + std::to_string(exploded.hour) +
      (exploded.minute < 10 ? "0" : "") + std::to_string(exploded.minute) +
      (exploded.second < 10 ? "0" : "") + std::to_string(exploded.second) +
      ".json";

  base::FilePath home_dir;
  base::PathService::Get(base::DIR_HOME, &home_dir);
  base::FilePath file_path =
      home_dir.Append(base::FilePath::FromUTF8Unsafe(".moltbrowser"))
          .Append(base::FilePath::FromUTF8Unsafe("chat_exports"))
          .Append(base::FilePath::FromUTF8Unsafe(filename));

  // mkdir + WriteFile on a ThreadPool worker; reply with success bool.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::TaskPriority::USER_BLOCKING, base::MayBlock()},
      base::BindOnce(
          [](base::FilePath path, std::string contents) {
            base::FilePath dir = path.DirName();
            if (!base::DirectoryExists(dir)) {
              base::CreateDirectory(dir);
            }
            return base::WriteFile(path, contents);
          },
          file_path, history_json),
      base::BindOnce(&MoltAIChatHandler::OnHistoryExported,
                     weak_ptr_factory_.GetWeakPtr(), callback_id, file_path,
                     filename));
}

void MoltAIChatHandler::OnHistoryExported(std::string callback_id,
                                          base::FilePath file_path,
                                          std::string filename,
                                          bool success) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!IsJavascriptAllowed()) return;

  base::DictValue result;
  result.Set("success", success);
  result.Set("path", file_path.AsUTF8Unsafe());
  result.Set("filename", filename);

  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(result)));
}

// ------------------------------------------------------------------
// HandleRunMoltAction: Run one automation action against the active
// tab in this WebUI's owning Browser. Used by the side-panel chat to
// let users (or, downstream, the LLM) drive page actions in plain
// language. The chat HTML side parses /click /type /scroll /navigate
// slash-commands and posts them here.
//
// JS: chrome.send('runMoltAction',
//                  [callback_id,
//                   {type:"click", selector:".foo"} |
//                   {type:"type", selector:"#email", value:"a@b.c"} |
//                   {type:"scroll", value:"600"} |
//                   {type:"navigate", value:"https://..."}])
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleRunMoltAction(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 2u);
  const std::string callback_id = args[0].GetString();

  base::DictValue result;
  if (!args[1].is_dict()) {
    result.Set("success", false);
    result.Set("error", "expected action dict");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
    return;
  }
  const base::DictValue& action = args[1].GetDict();
  const std::string* type = action.FindString("type");
  if (!type || type->empty()) {
    result.Set("success", false);
    result.Set("error", "missing action type");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
    return;
  }
  const std::string* sel = action.FindString("selector");
  const std::string* value = action.FindString("value");

  // Resolve the browser window that should receive the action.
  // The side-panel's WebContents is NOT a tab in the tab strip, so
  // FindBrowserWithTab returns null for it. Fall back to
  // chrome::FindLastActive() — the most-recently-focused browser window,
  // which is the one the user is actively looking at.
  content::WebContents* webui_wc = web_ui()->GetWebContents();
  Browser* browser = chrome::FindBrowserWithTab(webui_wc);
  if (!browser) {
    browser = chrome::FindLastActive();
  }
  if (!browser || !browser->tab_strip_model()) {
    result.Set("success", false);
    result.Set("error", "no active browser window");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
    return;
  }
  content::WebContents* target =
      browser->tab_strip_model()->GetActiveWebContents();
  if (!target) {
    result.Set("success", false);
    result.Set("error", "no active tab");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
    return;
  }

  std::string t = *type;

  // P2.3 / P3: select-dropdown, hover, right-click, drag are simple
  // enough to execute via direct JS injection — no need to spin up
  // an AutomationRunner (which assumes WebContentsObserver hooks
  // and selector-ladder retries that buy nothing for fire-and-forget
  // events). We still require an http(s) page so injecting into
  // chrome:// or other privileged URLs is impossible.
  if (t == "select" || t == "hover" || t == "right-click" || t == "drag") {
    if (!sel || sel->empty()) {
      result.Set("success", false);
      result.Set("error", "selector required for " + t);
      ResolveJavascriptCallback(base::Value(callback_id),
                                base::Value(std::move(result)));
      return;
    }
    if (t == "drag" && (!value || value->empty())) {
      result.Set("success", false);
      result.Set("error", "drag needs a target selector in 'value'");
      ResolveJavascriptCallback(base::Value(callback_id),
                                base::Value(std::move(result)));
      return;
    }
    if (!target->GetLastCommittedURL().SchemeIsHTTPOrHTTPS()) {
      result.Set("success", false);
      result.Set("error", "only http(s) pages support " + t);
      ResolveJavascriptCallback(base::Value(callback_id),
                                base::Value(std::move(result)));
      return;
    }
    // JS-escape the selector + value via JsStringLiteral (file-scope
    // helper using base::JSONWriter::Write). Replaces a former hand-
    // rolled escaper that handled \", \\, \n, \r but missed control
    // chars and U+2028 / U+2029. Code-review HIGH #4.
    std::string js;
    if (t == "select") {
      std::string v = value ? *value : "";
      js = "(function(){var el=document.querySelector(" +
           JsStringLiteral(*sel) +
           ");if(!el)return false;el.value=" + JsStringLiteral(v) +
           ";el.dispatchEvent(new Event('input',{bubbles:true}));"
           "el.dispatchEvent(new Event('change',{bubbles:true}));"
           "return true;})()";
    } else if (t == "hover") {
      js = "(function(){var el=document.querySelector(" +
           JsStringLiteral(*sel) +
           ");if(!el)return false;"
           "['mouseover','mouseenter','mousemove'].forEach(function(t){"
           "el.dispatchEvent(new MouseEvent(t,{bubbles:true,"
           "cancelable:true,view:window}));});return true;})()";
    } else if (t == "right-click") {
      // Synthesize a contextmenu event with button=2. Most sites'
      // custom right-click menus listen for this; the native macOS/
      // OS menu won't appear, but page-level menus do.
      js = "(function(){var el=document.querySelector(" +
           JsStringLiteral(*sel) +
           ");if(!el)return false;"
           "var r=el.getBoundingClientRect();"
           "el.dispatchEvent(new MouseEvent('contextmenu',{bubbles:true,"
           "cancelable:true,view:window,button:2,buttons:2,"
           "clientX:r.left+r.width/2,clientY:r.top+r.height/2}));"
           "return true;})()";
    } else {  // drag
      // Source = selector, target = value. Synthesize the full
      // HTML5 drag-drop event sequence with a shared DataTransfer.
      // Many SPAs (Trello-style, Notion-style) listen for these
      // events directly so this is enough to drive them.
      std::string tgt = value ? *value : "";
      js = "(function(){"
           "var src=document.querySelector(" + JsStringLiteral(*sel) + ");"
           "var tgt=document.querySelector(" + JsStringLiteral(tgt) + ");"
           "if(!src||!tgt)return false;"
           "var dt=new DataTransfer();"
           "var sr=src.getBoundingClientRect();var tr=tgt.getBoundingClientRect();"
           "function ev(el,type,box){return new DragEvent(type,{bubbles:true,"
           "cancelable:true,view:window,dataTransfer:dt,"
           "clientX:box.left+box.width/2,clientY:box.top+box.height/2});}"
           "src.dispatchEvent(ev(src,'dragstart',sr));"
           "tgt.dispatchEvent(ev(tgt,'dragenter',tr));"
           "tgt.dispatchEvent(ev(tgt,'dragover',tr));"
           "tgt.dispatchEvent(ev(tgt,'drop',tr));"
           "src.dispatchEvent(ev(src,'dragend',sr));"
           "return true;})()";
    }
    std::string cb_id_copy = callback_id;
    auto weak_this = weak_ptr_factory_.GetWeakPtr();
    target->GetPrimaryMainFrame()->ExecuteJavaScriptInIsolatedWorld(
        base::UTF8ToUTF16(js),
        base::BindOnce(
            [](base::WeakPtr<MoltAIChatHandler> self, std::string cb_id,
               std::string type_str, base::Value v) {
              if (!self) return;
              bool ok = v.is_bool() && v.GetBool();
              base::DictValue out;
              out.Set("success", ok);
              out.Set("message", ok ? type_str + " ok" :
                                       type_str + ": selector not found");
              self->ResolveJavascriptCallback(base::Value(cb_id),
                                              base::Value(std::move(out)));
            },
            weak_this, cb_id_copy, t),
        /*world_id=*/1);
    return;
  }

  // Build a single-step Script. We map the slash commands to the
  // existing AutomationRunner step types so we get all the
  // selector-recovery, retry, and snapshot-on-failure plumbing for
  // free.
  molt_ai::automation::Script s;
  s.id = "ad-hoc-chat-action";
  s.name = "Chat action";
  s.security.trust = molt_ai::automation::TrustLevel::TRUSTED;
  s.security.max_runtime_seconds = 30;

  molt_ai::automation::Step step;
  if (t == "click") {
    step.type = molt_ai::automation::StepType::CLICK;
    step.target = sel ? *sel : "";
  } else if (t == "type") {
    step.type = molt_ai::automation::StepType::TYPE;
    step.target = sel ? *sel : "";
    step.value = value ? *value : "";
  } else if (t == "scroll") {
    step.type = molt_ai::automation::StepType::SCROLL;
    step.value = value ? *value : "600";
  } else if (t == "navigate") {
    step.type = molt_ai::automation::StepType::NAVIGATE;
    step.target = value ? *value : "";
  } else if (t == "wait") {
    // P3: simple sleep. value is ms (default 1000).
    step.type = molt_ai::automation::StepType::WAIT;
    int ms = 1000;
    if (value && !value->empty())
      base::StringToInt(*value, &ms);
    step.timeout_ms = ms;
  } else if (t == "wait-for") {
    // P3: poll until a selector appears, up to value ms (default 5000).
    step.type = molt_ai::automation::StepType::WAIT_FOR;
    step.target = sel ? *sel : "";
    int ms = 5000;
    if (value && !value->empty())
      base::StringToInt(*value, &ms);
    step.timeout_ms = ms;
  } else {
    result.Set("success", false);
    result.Set("error", "unknown action type: " + t);
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
    return;
  }
  step.timeout_ms = 8000;
  s.steps.push_back(std::move(step));

  // Run via a transient AutomationRunner. Storage is null — we don't
  // persist this script's stats since it's an ad-hoc chat action.
  // The runner is captured by a shared_ptr in the completion callback
  // so it stays alive across async step execution.
  auto runner = std::make_shared<molt_ai::automation::AutomationRunner>(
      target, /*ai_runtime=*/nullptr, /*storage=*/nullptr);
  std::string callback_id_copy = callback_id;
  auto weak_this = weak_ptr_factory_.GetWeakPtr();
  runner->Run(std::move(s),
              base::DoNothing(),
              base::BindOnce(
                  [](base::WeakPtr<MoltAIChatHandler> self,
                     std::string cb_id,
                     std::shared_ptr<molt_ai::automation::AutomationRunner>
                         keep_alive,
                     molt_ai::automation::RunResult r) {
                    if (!self) return;
                    base::DictValue out;
                    out.Set("success", r.success);
                    out.Set("message", r.message);
                    out.Set("duration_ms", r.duration_ms);
                    self->ResolveJavascriptCallback(
                        base::Value(cb_id),
                        base::Value(std::move(out)));
                  },
                  weak_this, callback_id_copy, runner));
}

// ------------------------------------------------------------------
// HandleQueryMemory: Personal Vector Memory grounding for the chat.
// Args: [callback_id, query_text, top_k_int]
// Returns: {hits: [{url, title, snippet, score, visited_at}, ...]}
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleQueryMemory(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 2u);
  const std::string callback_id = args[0].GetString();
  const std::string query =
      args[1].is_string() ? args[1].GetString() : "";
  int top_k = 3;
  if (args.size() > 2 && args[2].is_int())
    top_k = std::max(1, args[2].GetInt());

  base::DictValue empty;
  empty.Set("hits", base::ListValue());
  if (query.empty()) {
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(empty)));
    return;
  }

  Profile* profile = Profile::FromBrowserContext(
      web_ui()->GetWebContents()->GetBrowserContext());
  if (!profile) {
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(empty)));
    return;
  }
  molt_ai::memory::MemoryService* svc =
      molt_ai::memory::MemoryServiceFactory::GetForProfile(profile);
  if (!svc) {
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(empty)));
    return;
  }

  std::string cb_id = callback_id;
  auto weak_this = weak_ptr_factory_.GetWeakPtr();
  svc->Query(query, top_k, base::BindOnce(
      [](base::WeakPtr<MoltAIChatHandler> self, std::string cb_id,
         std::vector<molt_ai::memory::QueryHit> hits) {
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
        self->ResolveJavascriptCallback(base::Value(cb_id),
                                        base::Value(std::move(r)));
      },
      weak_this, cb_id));
}

// ------------------------------------------------------------------
// HandleListTabsInWindow: Tab Triage feature.
// Enumerates every tab in the Browser that owns this side-panel WebUI,
// returning {id, url, title, snippet, pinned, active, last_active_unix}.
// The chat then asks the LLM (or shows a manual list) which tabs to
// close / bookmark / pin.
// Args: [callback_id]
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleListTabsInWindow(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 1u);
  const std::string callback_id = args[0].GetString();

  base::DictValue out;
  base::ListValue tabs;

  content::WebContents* webui_wc = web_ui()->GetWebContents();
  Browser* browser = chrome::FindBrowserWithTab(webui_wc);
  if (!browser || !browser->tab_strip_model()) {
    out.Set("tabs", std::move(tabs));
    out.Set("error", "no owning browser");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  TabStripModel* model = browser->tab_strip_model();
  const int active = model->active_index();
  const int n = model->count();
  for (int i = 0; i < n; ++i) {
    content::WebContents* wc = model->GetWebContentsAt(i);
    if (!wc) continue;
    base::DictValue d;
    d.Set("index", i);
    d.Set("url", wc->GetLastCommittedURL().spec());
    d.Set("title", base::UTF16ToUTF8(wc->GetTitle()));
    d.Set("pinned", model->IsTabPinned(i));
    d.Set("active", i == active);
    // Cheap snippet: last commit URL's host + path tail. Anything richer
    // would require a content-extraction JS round-trip across N tabs;
    // the side panel can fall back to that on demand.
    std::string host = std::string(wc->GetLastCommittedURL().host());
    std::string path = std::string(wc->GetLastCommittedURL().path());
    if (path.size() > 40) path = path.substr(0, 40) + "…";
    d.Set("snippet", host + path);
    d.Set("last_active_unix",
          static_cast<double>(wc->GetLastActiveTime().ToTimeT()));
    tabs.Append(std::move(d));
  }
  out.Set("tabs", std::move(tabs));
  out.Set("active_index", active);
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}

// ------------------------------------------------------------------
// HandleTriageActOnTabs: Tab Triage bulk action.
// Args: [callback_id, {action:"close"|"bookmark"|"pin",
//                       indices:[int, int, ...]}]
// Returns: {success, affected_count, error?}
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleTriageActOnTabs(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 2u);
  const std::string callback_id = args[0].GetString();

  base::DictValue out;
  if (!args[1].is_dict()) {
    out.Set("success", false);
    out.Set("error", "expected action dict");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  const base::DictValue& dict = args[1].GetDict();
  const std::string* action = dict.FindString("action");
  const base::ListValue* indices = dict.FindList("indices");
  if (!action || action->empty() || !indices) {
    out.Set("success", false);
    out.Set("error", "missing action or indices");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  content::WebContents* webui_wc = web_ui()->GetWebContents();
  Browser* browser = chrome::FindBrowserWithTab(webui_wc);
  if (!browser || !browser->tab_strip_model()) {
    out.Set("success", false);
    out.Set("error", "no owning browser");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  TabStripModel* model = browser->tab_strip_model();

  // Collect & sort indices descending so closing doesn't shift the
  // remaining indices out from under us.
  std::vector<int> idxs;
  for (const base::Value& v : *indices) {
    if (v.is_int()) idxs.push_back(v.GetInt());
  }
  std::sort(idxs.begin(), idxs.end(), std::greater<int>());

  int affected = 0;
  if (*action == "close") {
    for (int i : idxs) {
      if (i < 0 || i >= model->count()) continue;
      model->CloseWebContentsAt(i, TabCloseTypes::CLOSE_CREATE_HISTORICAL_TAB |
                                       TabCloseTypes::CLOSE_USER_GESTURE);
      ++affected;
    }
  } else if (*action == "pin") {
    for (int i : idxs) {
      if (i < 0 || i >= model->count()) continue;
      if (!model->IsTabPinned(i)) {
        model->SetTabPinned(i, /*pinned=*/true);
        ++affected;
      }
    }
  } else if (*action == "bookmark") {
    Profile* profile = Profile::FromBrowserContext(
        webui_wc->GetBrowserContext());
    bookmarks::BookmarkModel* bm =
        profile ? BookmarkModelFactory::GetForBrowserContext(profile)
                : nullptr;
    if (!bm || !bm->loaded()) {
      out.Set("success", false);
      out.Set("error", "bookmark model unavailable");
      ResolveJavascriptCallback(base::Value(callback_id),
                                base::Value(std::move(out)));
      return;
    }
    const bookmarks::BookmarkNode* parent = bm->other_node();
    for (int i : idxs) {
      if (i < 0 || i >= model->count()) continue;
      content::WebContents* wc = model->GetWebContentsAt(i);
      if (!wc) continue;
      GURL url = wc->GetLastCommittedURL();
      if (!url.is_valid()) continue;
      bm->AddURL(parent, parent->children().size(), wc->GetTitle(), url);
      ++affected;
    }
  } else {
    out.Set("success", false);
    out.Set("error", "unknown action: " + *action);
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  out.Set("success", true);
  out.Set("affected_count", affected);
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}

// ------------------------------------------------------------------
// HandleCreateWatcher: Page Watcher feature.
// Builds a Script with an INTERVAL trigger that:
//   1. NAVIGATEs to the watched URL
//   2. WAIT_FORs the selector
//   3. EXTRACTs the selector's text into {{value}}
//   4. NOTIFYs the user with that value
// then saves it via AutomationStorage so the existing scheduler picks
// it up on its next tick. Per user spec, watchers use the default
// (TinyLlama) model and run in a background window.
//
// Args: [callback_id, {url, selector, interval_seconds, name?}]
// Returns: {success, script_id, error?}
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleCreateWatcher(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 2u);
  const std::string callback_id = args[0].GetString();

  base::DictValue out;
  if (!args[1].is_dict()) {
    out.Set("success", false);
    out.Set("error", "expected watcher dict");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  const base::DictValue& d = args[1].GetDict();
  const std::string* url = d.FindString("url");
  const std::string* selector = d.FindString("selector");
  std::optional<int> interval = d.FindInt("interval_seconds");
  const std::string* name = d.FindString("name");
  if (!url || url->empty() || !selector || selector->empty()) {
    out.Set("success", false);
    out.Set("error", "url and selector are required");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  int seconds = interval.value_or(900);  // default 15 min
  if (seconds < 60) seconds = 60;        // floor: 1 min

  GURL gurl(*url);
  if (!gurl.is_valid() || !gurl.SchemeIsHTTPOrHTTPS()) {
    out.Set("success", false);
    out.Set("error", "url must be http(s)");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  molt_ai::automation::Script s;
  // Stable id from a hash of the url+selector — re-creating the watcher
  // with the same args updates the existing script in place.
  size_t h = std::hash<std::string>{}(*url + "|" + *selector);
  s.id = base::StringPrintf("watcher-%zx", h);
  s.name = (name && !name->empty()) ? *name
                                    : ("Watch " + std::string(gurl.host()));
  s.created_at_unix = static_cast<int64_t>(time(nullptr));
  s.trigger.type = molt_ai::automation::TriggerType::INTERVAL;
  s.trigger.expression = base::NumberToString(seconds);
  s.security.trust = molt_ai::automation::TrustLevel::APPROVED;
  s.security.max_runtime_seconds = 120;
  s.security.domain_whitelist.push_back(std::string(gurl.host()));

  {
    molt_ai::automation::Step step;
    step.type = molt_ai::automation::StepType::NAVIGATE;
    step.target = *url;
    step.timeout_ms = 20000;
    step.description = "Open watched page";
    s.steps.push_back(std::move(step));
  }
  {
    molt_ai::automation::Step step;
    step.type = molt_ai::automation::StepType::WAIT_FOR;
    step.target = *selector;
    step.timeout_ms = 15000;
    step.description = "Wait for selector";
    s.steps.push_back(std::move(step));
  }
  {
    molt_ai::automation::Step step;
    step.type = molt_ai::automation::StepType::EXTRACT;
    step.target = *selector;
    step.store_as = "value";
    step.timeout_ms = 5000;
    step.description = "Extract current value";
    s.steps.push_back(std::move(step));
  }
  {
    molt_ai::automation::Step step;
    step.type = molt_ai::automation::StepType::NOTIFY;
    step.target = s.name;
    step.value = "Current value: {{value}}";
    step.timeout_ms = 2000;
    step.description = "Notify user";
    s.steps.push_back(std::move(step));
  }

  // Save the script to disk. AutomationStorage methods touch the
  // filesystem on the UI thread, so wrap in ScopedAllowBlockingForMolt.
  bool saved = false;
  {
    ScopedAllowBlockingForMolt allow;
    molt_ai::automation::AutomationStorage storage;
    storage.EnsureDirectory();
    saved = storage.Save(s);
  }

  if (!saved) {
    out.Set("success", false);
    out.Set("error", "failed to save watcher script");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  // Nudge the scheduler so it picks up the new INTERVAL trigger right
  // away rather than waiting for its next reload tick.
  Profile* profile = Profile::FromBrowserContext(
      web_ui()->GetWebContents()->GetBrowserContext());
  if (profile) {
    auto* svc =
        molt_ai::automation::AutomationSchedulerServiceFactory::GetForProfile(
            profile);
    if (svc && svc->scheduler()) {
      svc->scheduler()->Reschedule();
    }
  }

  out.Set("success", true);
  out.Set("script_id", s.id);
  out.Set("interval_seconds", seconds);
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}

// ------------------------------------------------------------------
// HandleListActiveAgents: Agent Inbox feature.
// Reads the process-wide AgentInboxRegistry and returns one row per
// currently-running (or just-finished) background automation run.
// The side-panel polls this every few seconds to keep the "Running
// agents" tray fresh.
// Args: [callback_id]
// Returns: {agents: [{id, script_id, script_name, start_url,
//                     current_step, total_steps, status_note,
//                     started_at_unix, finished_at_unix, succeeded}]}
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleListActiveAgents(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 1u);
  const std::string callback_id = args[0].GetString();

  base::ListValue arr;
  auto entries = molt_ai::automation::AgentInboxRegistry::Get()->List();
  for (const auto& e : entries) {
    base::DictValue d;
    d.Set("id", static_cast<double>(e.id));
    d.Set("script_id", e.script_id);
    d.Set("script_name", e.script_name);
    d.Set("start_url", e.start_url);
    d.Set("current_step", e.current_step);
    d.Set("total_steps", e.total_steps);
    d.Set("status_note", e.status_note);
    d.Set("started_at_unix", static_cast<double>(e.started_at_unix));
    d.Set("finished_at_unix", static_cast<double>(e.finished_at_unix));
    d.Set("succeeded", e.succeeded);
    arr.Append(std::move(d));
  }
  base::DictValue out;
  out.Set("agents", std::move(arr));
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}

// ------------------------------------------------------------------
// Form Filler — load/save the encrypted local profile.
// Profile lives at ~/.moltbrowser/profile.enc, encrypted via OSCrypt.
// We do synchronous file I/O on the UI thread guarded by
// ScopedAllowBlockingForMolt; the blob is ~hundreds-of-bytes small so
// the cost is negligible vs. the bounce-to-worker boilerplate.
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleGetMoltProfile(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 1u);
  const std::string callback_id = args[0].GetString();

  base::DictValue dict;
  {
    ScopedAllowBlockingForMolt allow;
    molt_ai::profile::MoltProfileStore store;
    dict = store.Load();
  }
  base::DictValue out;
  out.Set("profile", std::move(dict));
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}

void MoltAIChatHandler::HandleSaveMoltProfile(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 2u);
  const std::string callback_id = args[0].GetString();

  base::DictValue out;
  if (!args[1].is_dict()) {
    out.Set("success", false);
    out.Set("error", "expected profile dict");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  base::DictValue dict = args[1].GetDict().Clone();
  bool ok = false;
  {
    ScopedAllowBlockingForMolt allow;
    molt_ai::profile::MoltProfileStore store;
    ok = store.Save(dict);
  }
  out.Set("success", ok);
  if (!ok) out.Set("error", "failed to save profile");
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}

// ------------------------------------------------------------------
// Form Filler — autofill the active tab from the saved profile.
//
// We do the matching in JS injected into an isolated world. Reasons:
//   - The matcher needs full DOM access (labels, autocomplete attrs,
//     aria-labelledby chains, surrounding text) which is cheaper to
//     express in JS than to round-trip across IPC.
//   - The matcher dispatches input/change events so SPA frameworks
//     (React, Angular, Vue) see the change.
//
// Native side just loads the profile, JSON-stringifies it, and hands
// it to the injected JS as a literal.
// ------------------------------------------------------------------
namespace {

// Heuristic field-name matcher. Returns the list of candidate substrings
// for each profile key — case-insensitive substring search against the
// concatenation of input.name, input.id, input.placeholder, label-for
// text, autocomplete attribute, and aria-label.
constexpr char kFormFillJS[] = R"JS(
(function(profile){
  if (!profile || typeof profile !== 'object') return {filled:0, total:0};
  // Match table: profile key -> array of regex strings to test against
  // the input's concatenated identity (name|id|placeholder|label|...).
  var rules = {
    email:          [/e[-_ ]?mail/i, /^email/i, /username.*email/i],
    phone:          [/phone/i, /tel(ephone)?/i, /mobile/i, /cell/i],
    full_name:      [/^name$/i, /full[-_ ]?name/i, /your[-_ ]?name/i],
    first_name:    [/first[-_ ]?name/i, /given[-_ ]?name/i, /\bfname\b/i],
    last_name:     [/last[-_ ]?name/i, /family[-_ ]?name/i, /sur[-_ ]?name/i, /\blname\b/i],
    address_line1: [/address[-_ ]?(line)?[-_ ]?1/i, /street[-_ ]?address/i, /^address$/i, /\baddr1?\b/i],
    address_line2: [/address[-_ ]?(line)?[-_ ]?2/i, /apt|apartment|suite|unit/i, /\baddr2\b/i],
    city:          [/city/i, /town/i, /locality/i],
    state:         [/state/i, /province/i, /region/i],
    zip:           [/zip/i, /postal/i, /post[-_ ]?code/i],
    country:       [/country/i, /nation/i],
    company:       [/company/i, /organi[sz]ation/i, /employer/i],
    job_title:     [/job[-_ ]?title/i, /position/i, /role/i],
    website:       [/website/i, /\burl\b/i, /homepage/i]
  };
  // Build identity string for a form control.
  function identity(el) {
    var s = (el.name || '') + ' ' + (el.id || '') + ' ' +
            (el.placeholder || '') + ' ' +
            (el.getAttribute('autocomplete') || '') + ' ' +
            (el.getAttribute('aria-label') || '');
    if (el.labels) {
      for (var i = 0; i < el.labels.length; i++) {
        s += ' ' + (el.labels[i].innerText || '');
      }
    }
    return s;
  }
  function fillField(el, value) {
    if (value == null || value === '') return false;
    try {
      var d = Object.getOwnPropertyDescriptor(
          window.HTMLInputElement.prototype, 'value');
      if (d && d.set) d.set.call(el, value);
      else el.value = value;
      el.dispatchEvent(new Event('input', {bubbles:true}));
      el.dispatchEvent(new Event('change', {bubbles:true}));
      return true;
    } catch (e) { return false; }
  }
  var inputs = Array.prototype.slice.call(
      document.querySelectorAll('input, textarea, select'));
  var filled = 0;
  var skipped = 0;
  for (var i = 0; i < inputs.length; i++) {
    var el = inputs[i];
    if (el.type === 'hidden' || el.type === 'submit' ||
        el.type === 'button' || el.type === 'file' ||
        el.type === 'password' || el.type === 'checkbox' ||
        el.type === 'radio' || el.disabled || el.readOnly) {
      skipped++;
      continue;
    }
    if (el.value && el.value.length > 0) {
      // Don't overwrite existing values — user may have typed.
      skipped++;
      continue;
    }
    var id = identity(el).toLowerCase();
    if (!id.trim()) continue;
    var matched = null;
    for (var key in rules) {
      if (!Object.prototype.hasOwnProperty.call(rules, key)) continue;
      if (!profile[key]) continue;
      var patterns = rules[key];
      for (var p = 0; p < patterns.length; p++) {
        if (patterns[p].test(id)) { matched = key; break; }
      }
      if (matched) break;
    }
    if (matched && fillField(el, profile[matched])) {
      filled++;
      el.setAttribute('data-molt-filled', matched);
    }
  }
  return {filled: filled, total: inputs.length, skipped: skipped};
})
)JS";

}  // namespace

void MoltAIChatHandler::HandleRunFormFill(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 1u);
  const std::string callback_id = args[0].GetString();

  base::DictValue out;

  // Resolve target tab.
  content::WebContents* webui_wc = web_ui()->GetWebContents();
  Browser* browser = chrome::FindBrowserWithTab(webui_wc);
  if (!browser || !browser->tab_strip_model()) {
    out.Set("success", false);
    out.Set("error", "no owning browser");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  content::WebContents* target =
      browser->tab_strip_model()->GetActiveWebContents();
  if (!target || !target->GetLastCommittedURL().SchemeIsHTTPOrHTTPS()) {
    out.Set("success", false);
    out.Set("error", "active tab is not an http(s) page");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  // Load profile and stringify.
  base::DictValue profile;
  {
    ScopedAllowBlockingForMolt allow;
    molt_ai::profile::MoltProfileStore store;
    profile = store.Load();
  }
  if (profile.empty()) {
    out.Set("success", false);
    out.Set("error", "profile is empty — open settings to set one up");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  std::string profile_json;
  base::JSONWriter::Write(profile, &profile_json);

  std::string js = std::string(kFormFillJS) + "(" + profile_json + ")";

  auto weak_this = weak_ptr_factory_.GetWeakPtr();
  std::string cb_id = callback_id;
  target->GetPrimaryMainFrame()->ExecuteJavaScriptInIsolatedWorld(
      base::UTF8ToUTF16(js),
      base::BindOnce(
          [](base::WeakPtr<MoltAIChatHandler> self, std::string cb_id,
             base::Value v) {
            if (!self) return;
            base::DictValue r;
            if (v.is_dict()) {
              const base::DictValue& d = v.GetDict();
              std::optional<int> filled = d.FindInt("filled");
              std::optional<int> total = d.FindInt("total");
              r.Set("success", filled.value_or(0) > 0);
              r.Set("filled", filled.value_or(0));
              r.Set("total", total.value_or(0));
              if (filled.value_or(0) == 0)
                r.Set("error", "no matching fields found");
            } else {
              r.Set("success", false);
              r.Set("error", "fill script returned no data");
            }
            self->ResolveJavascriptCallback(base::Value(cb_id),
                                            base::Value(std::move(r)));
          },
          weak_this, cb_id),
      /*world_id=*/1);
}

// ------------------------------------------------------------------
// HandleListMemoryDocs — Personal Vector Memory dump for the AI-grouped
// history WebUI. We don't cluster server-side; the JS does
// keyword-overlap clustering so the user can re-cluster (filter by
// host, "merge near-duplicates", etc.) without a round-trip.
// Args: [callback_id, limit?]  (limit defaults to 200, capped at 2000)
// Returns: {docs:[{url,title,visited_at,word_count,host}]}
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleListMemoryDocs(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 1u);
  const std::string callback_id = args[0].GetString();

  int limit = 200;
  if (args.size() > 1 && args[1].is_int()) {
    limit = std::max(1, std::min(2000, args[1].GetInt()));
  }

  Profile* profile = Profile::FromBrowserContext(
      web_ui()->GetWebContents()->GetBrowserContext());
  if (!profile) {
    base::DictValue empty;
    empty.Set("docs", base::ListValue());
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(empty)));
    return;
  }
  molt_ai::memory::MemoryService* svc =
      molt_ai::memory::MemoryServiceFactory::GetForProfile(profile);
  if (!svc) {
    base::DictValue empty;
    empty.Set("docs", base::ListValue());
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(empty)));
    return;
  }
  std::string cb_id = callback_id;
  auto weak_this = weak_ptr_factory_.GetWeakPtr();
  svc->ListRecent(limit, base::BindOnce(
      [](base::WeakPtr<MoltAIChatHandler> self, std::string cb_id,
         std::vector<molt_ai::memory::Document> docs) {
        if (!self) return;
        base::ListValue arr;
        for (const auto& d : docs) {
          base::DictValue dv;
          dv.Set("doc_id", static_cast<double>(d.doc_id));
          dv.Set("url", d.url);
          dv.Set("title", d.title);
          dv.Set("visited_at_unix",
                 static_cast<double>(d.visited_at_unix));
          dv.Set("word_count", d.word_count);
          // Convenience: pre-extract the host so the JS clusterer
          // doesn't need to instantiate a URL per doc.
          GURL g(d.url);
          dv.Set("host", g.is_valid() ? std::string(g.host()) : std::string());
          arr.Append(std::move(dv));
        }
        base::DictValue out;
        out.Set("docs", std::move(arr));
        self->ResolveJavascriptCallback(base::Value(cb_id),
                                        base::Value(std::move(out)));
      },
      weak_this, cb_id));
}

// ------------------------------------------------------------------
// HandleExtractPdfText: download a PDF URL via SimpleURLLoader, run
// our minimal PDF text scraper, and ship the result back to chat.
// The result includes the host so the UI can show a "Chatting with
// foo.pdf from example.com" chip.
//
// Args: [callback_id, url]
// Returns: {success, url, text, char_count, error?}
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleExtractPdfText(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 2u);
  const std::string callback_id = args[0].GetString();
  const std::string url = args[1].is_string() ? args[1].GetString() : "";

  base::DictValue err;
  if (url.empty()) {
    err.Set("success", false);
    err.Set("error", "missing url");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(err)));
    return;
  }
  GURL gurl(url);
  // http(s) only. The earlier version also allowed file://, but the
  // browser-process URLLoaderFactory we use here doesn't actually
  // honor file:// — so it was a dormant LFI primitive waiting for
  // someone to swap in a permissive factory. Code-review HIGH #3.
  // If file:// PDF support is ever needed, route it through a
  // dedicated handler that explicitly opts in (and adds path-prefix
  // restrictions).
  if (!gurl.is_valid() || !gurl.SchemeIsHTTPOrHTTPS()) {
    err.Set("success", false);
    err.Set("error", "url must be http(s)");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(err)));
    return;
  }

  pdf_callback_id_ = callback_id;
  pdf_source_url_ = url;

  auto resource_request = std::make_unique<network::ResourceRequest>();
  resource_request->url = gurl;
  resource_request->method = "GET";
  resource_request->credentials_mode = network::mojom::CredentialsMode::kOmit;

  net::NetworkTrafficAnnotationTag traffic_annotation =
      net::DefineNetworkTrafficAnnotation("molt_ai_pdf_extract", R"(
        semantics {
          sender: "MoltBrowser AI"
          description: "Downloads a PDF the user asked to chat with"
          trigger: "User invokes /pdf in the side panel chat"
          data: "HTTP GET for the PDF the user asked about"
          destination: WEBSITE
        }
        policy {
          cookies_allowed: NO
          setting: "User-initiated PDF extraction in the AI side panel"
        })");

  pdf_loader_ = network::SimpleURLLoader::Create(
      std::move(resource_request), traffic_annotation);
  pdf_loader_->SetAllowHttpErrorResults(false);
  // Cap at 25 MB so a giant PDF doesn't OOM the browser process.
  static constexpr size_t kMaxPdfBytes = 25 * 1024 * 1024;

  Profile* profile = Profile::FromBrowserContext(
      web_ui()->GetWebContents()->GetBrowserContext());
  network::mojom::URLLoaderFactory* factory =
      profile ? profile->GetDefaultStoragePartition()
                    ->GetURLLoaderFactoryForBrowserProcess().get()
              : nullptr;
  if (!factory) {
    err.Set("success", false);
    err.Set("error", "no url loader factory");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(err)));
    pdf_loader_.reset();
    return;
  }

  auto weak_this = weak_ptr_factory_.GetWeakPtr();
  pdf_loader_->DownloadToString(
      factory,
      base::BindOnce(
          [](base::WeakPtr<MoltAIChatHandler> self,
             std::optional<std::string> body) {
            if (!self) return;
            std::string cb_id = self->pdf_callback_id_;
            std::string src_url = self->pdf_source_url_;
            self->pdf_loader_.reset();
            self->pdf_callback_id_.clear();
            self->pdf_source_url_.clear();

            base::DictValue out;
            out.Set("url", src_url);
            if (!body.has_value() || body->empty()) {
              out.Set("success", false);
              out.Set("error", "PDF download failed");
              self->ResolveJavascriptCallback(base::Value(cb_id),
                                              base::Value(std::move(out)));
              return;
            }
            // Hand to the scraper. CPU-only, ~100ms for typical PDFs.
            std::vector<uint8_t> bytes(body->begin(), body->end());
            std::string text =
                molt_ai::pdf::ExtractText(bytes, /*max_chars=*/50000);
            if (!text.empty()) {
              out.Set("success", true);
              out.Set("text", text);
              out.Set("char_count", static_cast<int>(text.size()));
              out.Set("source", "text_layer");
              self->ResolveJavascriptCallback(base::Value(cb_id),
                                              base::Value(std::move(out)));
              return;
            }
            // No text layer — fall back to OCR via bundled tesseract.
            // We keep the URL in the response by routing through a
            // tiny lambda that re-wraps the result.
            std::string raw(body->begin(), body->end());
            std::string cb_inner = cb_id;
            molt_ai::ocr::OcrService::Get()->OcrPdf(
                std::move(raw), /*max_chars=*/50000,
                base::BindOnce(
                    [](base::WeakPtr<MoltAIChatHandler> self,
                       std::string cb_id, std::string src_url,
                       molt_ai::ocr::OcrResult r) {
                      if (!self) return;
                      base::DictValue out;
                      out.Set("url", src_url);
                      out.Set("source", "ocr");
                      out.Set("ocr_binary_source", r.binary_source);
                      out.Set("ocr_duration_ms", r.duration_ms);
                      if (r.success) {
                        out.Set("success", true);
                        out.Set("text", r.text);
                        out.Set("char_count",
                                static_cast<int>(r.text.size()));
                      } else {
                        out.Set("success", false);
                        // Be explicit about whether tesseract was
                        // missing vs. failed.
                        if (r.binary_source == "none") {
                          out.Set("error",
                                  "no text layer and no OCR available "
                                  "— run scripts/bundle-tesseract.sh "
                                  "or install tesseract.");
                        } else {
                          out.Set("error", "no text layer; OCR failed: " +
                                            r.error);
                        }
                      }
                      self->ResolveJavascriptCallback(
                          base::Value(cb_id),
                          base::Value(std::move(out)));
                    },
                    self, cb_id, src_url));
          },
          weak_this),
      kMaxPdfBytes);
}

// ------------------------------------------------------------------
// HandleSearchBookmarks: walk the BookmarkModel tree and rank entries
// against the query using simple unique-token overlap on title+url+host.
// Same ranker as the chunk-ranker in the chat (deterministic, no
// embedding needed). Returns top-K.
//
// Args: [callback_id, query, top_k]
// Returns: {hits: [{title, url, host, score, parent}]}
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleSearchBookmarks(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 2u);
  const std::string callback_id = args[0].GetString();
  const std::string query = args[1].is_string() ? args[1].GetString() : "";
  int top_k = 8;
  if (args.size() > 2 && args[2].is_int())
    top_k = std::max(1, args[2].GetInt());

  base::DictValue out;
  base::ListValue hits;

  Profile* profile = Profile::FromBrowserContext(
      web_ui()->GetWebContents()->GetBrowserContext());
  bookmarks::BookmarkModel* bm =
      profile ? BookmarkModelFactory::GetForBrowserContext(profile)
              : nullptr;
  if (!bm || !bm->loaded() || query.empty()) {
    out.Set("hits", std::move(hits));
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  // Tokenise query.
  auto tokenise = [](const std::string& s) {
    std::vector<std::string> out_tokens;
    std::string cur;
    for (char c : s) {
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9')) {
        cur += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      } else {
        if (cur.size() >= 2) out_tokens.push_back(cur);
        cur.clear();
      }
    }
    if (cur.size() >= 2) out_tokens.push_back(cur);
    return out_tokens;
  };
  std::vector<std::string> q_tokens = tokenise(query);
  std::set<std::string> q_set(q_tokens.begin(), q_tokens.end());
  if (q_set.empty()) {
    out.Set("hits", std::move(hits));
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  struct Cand {
    int score = 0;
    std::string title;
    std::string url;
    std::string host;
    std::string parent;
  };
  std::vector<Cand> cands;

  // DFS over the bookmark tree.
  std::function<void(const bookmarks::BookmarkNode*, const std::string&)> dfs;
  dfs = [&](const bookmarks::BookmarkNode* node,
            const std::string& parent_name) {
    if (!node) return;
    if (node->is_url()) {
      std::string title = base::UTF16ToUTF8(node->GetTitle());
      std::string url = node->url().spec();
      std::string host = std::string(node->url().host());
      std::vector<std::string> doc_tokens = tokenise(title + " " + url);
      std::set<std::string> seen;
      int score = 0;
      for (const auto& t : doc_tokens) {
        if (q_set.count(t) && !seen.count(t)) {
          ++score;
          seen.insert(t);
        }
      }
      if (score > 0) {
        cands.push_back({score, std::move(title), std::move(url),
                         std::move(host), parent_name});
      }
    } else {
      std::string my_name = base::UTF16ToUTF8(node->GetTitle());
      for (size_t i = 0; i < node->children().size(); ++i) {
        dfs(node->children()[i].get(),
            my_name.empty() ? parent_name : my_name);
      }
    }
  };
  dfs(bm->root_node(), "");

  std::sort(cands.begin(), cands.end(),
            [](const Cand& a, const Cand& b) { return a.score > b.score; });
  int kept = 0;
  for (const auto& c : cands) {
    if (kept++ >= top_k) break;
    base::DictValue d;
    d.Set("title", c.title);
    d.Set("url", c.url);
    d.Set("host", c.host);
    d.Set("score", c.score);
    d.Set("parent", c.parent);
    hits.Append(std::move(d));
  }
  out.Set("hits", std::move(hits));
  out.Set("query_token_count", static_cast<int>(q_set.size()));
  out.Set("total_candidates", static_cast<int>(cands.size()));
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}

// ------------------------------------------------------------------
// HandleExtractAllTabsText: pull innerText from every tab in the owning
// Browser's tab strip via ExecuteJavaScriptInIsolatedWorld. Returns a
// flat array of {url, title, text}. Each tab is capped at
// max_chars_per_tab (default 4000) so the LLM context isn't blown.
//
// The N async JS calls all hit the same UI thread; we use a small
// shared counter to know when all returned and emit the final response
// once.
//
// Args: [callback_id, max_chars_per_tab]
// Returns: {tabs: [{url, title, text}], total_chars}
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleExtractAllTabsText(
    const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 1u);
  const std::string callback_id = args[0].GetString();
  int max_chars_per_tab = 4000;
  if (args.size() > 1 && args[1].is_int())
    max_chars_per_tab = std::max(200, args[1].GetInt());

  content::WebContents* webui_wc = web_ui()->GetWebContents();
  Browser* browser = chrome::FindBrowserWithTab(webui_wc);

  base::DictValue out;
  base::ListValue tabs;
  if (!browser || !browser->tab_strip_model()) {
    out.Set("tabs", std::move(tabs));
    out.Set("error", "no owning browser");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  TabStripModel* model = browser->tab_strip_model();
  const int n = model->count();
  if (n == 0) {
    out.Set("tabs", std::move(tabs));
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  // Shared collector held by shared_ptr so each per-tab callback can
  // contribute. When pending drops to 0 we emit the resolve.
  struct Collector {
    int pending = 0;
    base::ListValue results;
    int total_chars = 0;
  };
  auto collector = std::make_shared<Collector>();
  collector->pending = n;
  std::string cb_id_copy = callback_id;
  auto weak_this = weak_ptr_factory_.GetWeakPtr();

  for (int i = 0; i < n; ++i) {
    content::WebContents* wc = model->GetWebContentsAt(i);
    base::DictValue tab_row;
    tab_row.Set("index", i);
    if (!wc || !wc->GetLastCommittedURL().SchemeIsHTTPOrHTTPS()) {
      tab_row.Set("url", wc ? wc->GetLastCommittedURL().spec() : std::string());
      tab_row.Set("title",
                  wc ? base::UTF16ToUTF8(wc->GetTitle()) : std::string());
      tab_row.Set("text", "");
      collector->results.Append(std::move(tab_row));
      --collector->pending;
      if (collector->pending == 0) {
        base::DictValue resolved;
        resolved.Set("tabs", std::move(collector->results));
        resolved.Set("total_chars", collector->total_chars);
        ResolveJavascriptCallback(base::Value(cb_id_copy),
                                  base::Value(std::move(resolved)));
      }
      continue;
    }
    tab_row.Set("url", wc->GetLastCommittedURL().spec());
    tab_row.Set("title", base::UTF16ToUTF8(wc->GetTitle()));
    // Inject a tiny script that returns the body innerText sliced to
    // |max_chars_per_tab|. Isolated world 1 — same world the chat
    // already uses for page-context extraction.
    std::string js = "(function(){"
        "var t=(document.body && document.body.innerText) || '';"
        "return t.length > " + std::to_string(max_chars_per_tab) + "?"
        "t.slice(0," + std::to_string(max_chars_per_tab) + "):t;})()";
    wc->GetPrimaryMainFrame()->ExecuteJavaScriptInIsolatedWorld(
        base::UTF8ToUTF16(js),
        base::BindOnce(
            [](base::WeakPtr<MoltAIChatHandler> self,
               std::shared_ptr<Collector> c, std::string cb_id,
               base::DictValue row, base::Value result) {
              std::string txt = result.is_string() ? result.GetString() : "";
              row.Set("text", txt);
              c->total_chars += static_cast<int>(txt.size());
              c->results.Append(std::move(row));
              if (--c->pending == 0 && self) {
                base::DictValue resolved;
                resolved.Set("tabs", std::move(c->results));
                resolved.Set("total_chars", c->total_chars);
                self->ResolveJavascriptCallback(
                    base::Value(cb_id),
                    base::Value(std::move(resolved)));
              }
            },
            weak_this, collector, cb_id_copy, std::move(tab_row)),
        /*world_id=*/1);
  }
}

// ------------------------------------------------------------------
// HandleOpenSandboxTab: open |url| in an off-the-record session so the
// page's cookies and storage are discarded when the OTR window closes.
// Reuses Chromium's existing chrome::OpenURLOffTheRecord which handles
// the OTR profile lifecycle for us.
//
// Args: [callback_id, url]
// Returns: {success, error?}
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleOpenSandboxTab(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 2u);
  const std::string callback_id = args[0].GetString();
  std::string url_str = args[1].is_string() ? args[1].GetString() : "";

  base::DictValue out;
  if (url_str.empty()) {
    out.Set("success", false);
    out.Set("error", "missing url");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  if (!url_str.starts_with("http://") && !url_str.starts_with("https://")) {
    url_str = "https://" + url_str;
  }
  GURL g(url_str);
  if (!g.is_valid()) {
    out.Set("success", false);
    out.Set("error", "invalid url");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  Profile* profile = Profile::FromBrowserContext(
      web_ui()->GetWebContents()->GetBrowserContext());
  if (!profile) {
    out.Set("success", false);
    out.Set("error", "no profile");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  // OpenURLOffTheRecord wants the *original* (non-OTR) profile.
  Profile* original = profile->GetOriginalProfile();
  chrome::OpenURLOffTheRecord(original, g);

  out.Set("success", true);
  out.Set("url", g.spec());
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}

// ------------------------------------------------------------------
// HandleGetTrackerBreakdown: Privacy heatmap.
//
// Inject a JS probe into the active tab's primary frame (isolated
// world 1) that walks performance.getEntriesByType('resource') —
// every subresource the page already loaded. We classify each remote
// host into ads / analytics / cdn / other, then return aggregate
// counts plus the top 12 unique hosts.
//
// We deliberately do *not* use a giant block-list dependency. The
// classifier here is intentionally conservative — its purpose is
// transparency about who the page talks to, not to be EasyList.
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleGetTrackerBreakdown(
    const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 1u);
  const std::string callback_id = args[0].GetString();

  base::DictValue out;
  content::WebContents* webui_wc = web_ui()->GetWebContents();
  Browser* browser = chrome::FindBrowserWithTab(webui_wc);
  if (!browser || !browser->tab_strip_model()) {
    out.Set("error", "no owning browser");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  content::WebContents* target =
      browser->tab_strip_model()->GetActiveWebContents();
  if (!target || !target->GetLastCommittedURL().SchemeIsHTTPOrHTTPS()) {
    out.Set("error", "active tab is not an http(s) page");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  // The JS does the classification client-side and returns a struct.
  // Why: avoids round-tripping every resource URL to C++; resource
  // lists can be hundreds long on news pages.
  static constexpr char kProbeJS[] = R"JS(
(function(){
  try {
    var origin = location.host;
    var entries = performance.getEntriesByType('resource') || [];
    var counts = {ads:0, analytics:0, cdn:0, other:0, first_party:0};
    var hostSet = {};
    function classify(h) {
      if (/doubleclick|adservice|adsystem|adnxs|ads-twitter|pubmatic|rubiconproject|criteo|taboola|outbrain|adsrvr|adform|amazon-adsystem|googlesyndication|googletagservices/.test(h))
        return 'ads';
      if (/google-analytics|googletagmanager|segment\.io|mixpanel|hotjar|fullstory|chartbeat|optimizely|amplitude|heap|matomo|plausible|cloudflareinsights|datadoghq|sentry/.test(h))
        return 'analytics';
      if (/cloudfront|akamai|fastly|cdn77|jsdelivr|unpkg|bunnycdn|cloudflare\.com|akamaihd|edgekey|imgix/.test(h))
        return 'cdn';
      return 'other';
    }
    for (var i = 0; i < entries.length; i++) {
      var u;
      try { u = new URL(entries[i].name); } catch(e) { continue; }
      var h = u.host;
      if (!h) continue;
      if (h === origin || h.endsWith('.' + origin) ||
          origin.endsWith('.' + h)) {
        counts.first_party += 1;
        continue;
      }
      hostSet[h] = (hostSet[h] || 0) + 1;
      var c = classify(h);
      counts[c] += 1;
    }
    var hosts = Object.keys(hostSet).map(function(h){
      return {host: h, count: hostSet[h], category: classify(h)};
    }).sort(function(a,b){ return b.count - a.count; }).slice(0, 12);
    return JSON.stringify({
      origin: origin,
      counts: counts,
      total_third_party: counts.ads + counts.analytics +
                         counts.cdn + counts.other,
      unique_third_party_hosts: Object.keys(hostSet).length,
      top_hosts: hosts
    });
  } catch (e) { return JSON.stringify({error: '' + e}); }
})();
)JS";

  std::string callback_id_copy = callback_id;
  auto weak_this = weak_ptr_factory_.GetWeakPtr();
  target->GetPrimaryMainFrame()->ExecuteJavaScriptInIsolatedWorld(
      base::UTF8ToUTF16(std::string(kProbeJS)),
      base::BindOnce(
          [](base::WeakPtr<MoltAIChatHandler> self, std::string cb_id,
             base::Value v) {
            if (!self) return;
            base::DictValue resolved;
            if (v.is_string()) {
              auto parsed = base::JSONReader::Read(v.GetString(),
                                                   /*options=*/0);
              if (parsed && parsed->is_dict()) {
                self->ResolveJavascriptCallback(
                    base::Value(cb_id),
                    base::Value(std::move(parsed->GetDict())));
                return;
              }
            }
            resolved.Set("error", "tracker probe returned no data");
            self->ResolveJavascriptCallback(base::Value(cb_id),
                                            base::Value(std::move(resolved)));
          },
          weak_this, callback_id_copy),
      /*world_id=*/1);
}

// ------------------------------------------------------------------
// HandleGetDomainReputation: ask MemoryService for everything we've
// indexed from |host| (or the active tab's host), and return a small
// summary: visit count, first/last visited, total word_count read.
// Plus a tiny risk-flag heuristic for sketchy TLDs / IP-as-host.
//
// The user can use this to answer "have I been here before? when?
// how much have I read here?" without trusting any external
// reputation service.
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleGetDomainReputation(
    const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 1u);
  const std::string callback_id = args[0].GetString();
  std::string host = (args.size() > 1 && args[1].is_string())
                         ? args[1].GetString()
                         : std::string();

  // If no host was supplied, infer from the active tab.
  if (host.empty()) {
    content::WebContents* webui_wc = web_ui()->GetWebContents();
    Browser* browser = chrome::FindBrowserWithTab(webui_wc);
    if (browser && browser->tab_strip_model()) {
      content::WebContents* t =
          browser->tab_strip_model()->GetActiveWebContents();
      if (t) host = std::string(t->GetLastCommittedURL().host());
    }
  }
  base::DictValue out;
  out.Set("host", host);
  if (host.empty()) {
    out.Set("error", "no host");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  // Risk heuristics (super conservative — purely informational chips
  // for the user, not blocking).
  bool risky_tld = false;
  for (const char* t :
       {".tk", ".ml", ".ga", ".cf", ".gq", ".top", ".xyz", ".click"}) {
    size_t tl = std::strlen(t);
    if (host.size() >= tl &&
        host.compare(host.size() - tl, tl, t) == 0) {
      risky_tld = true;
      break;
    }
  }
  bool is_ip_host = !host.empty() &&
                    (host[0] >= '0' && host[0] <= '9');
  out.Set("risky_tld", risky_tld);
  out.Set("is_ip_host", is_ip_host);

  Profile* profile = Profile::FromBrowserContext(
      web_ui()->GetWebContents()->GetBrowserContext());
  molt_ai::memory::MemoryService* svc =
      profile ? molt_ai::memory::MemoryServiceFactory::GetForProfile(profile)
              : nullptr;
  if (!svc) {
    out.Set("first_visit_on_device", true);
    out.Set("visit_count", 0);
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  std::string cb = callback_id;
  std::string host_copy = host;
  auto weak_this = weak_ptr_factory_.GetWeakPtr();
  // Pull a wide window of recent docs and filter here. The memory
  // index is held in RAM so this is sub-ms even at 50k docs.
  svc->ListRecent(
      10000,
      base::BindOnce(
          [](base::WeakPtr<MoltAIChatHandler> self, std::string cb_id,
             std::string host, base::DictValue out_init,
             std::vector<molt_ai::memory::Document> docs) {
            if (!self) return;
            base::DictValue out = std::move(out_init);
            int visit_count = 0;
            int64_t first = 0;
            int64_t last = 0;
            int64_t total_words = 0;
            for (const auto& d : docs) {
              GURL u(d.url);
              if (!u.is_valid()) continue;
              std::string h = std::string(u.host());
              if (h != host && !(h.size() > host.size() &&
                                  h.compare(h.size() - host.size() - 1,
                                            host.size() + 1,
                                            "." + host) == 0)) {
                continue;
              }
              ++visit_count;
              total_words += d.word_count;
              if (first == 0 || d.visited_at_unix < first)
                first = d.visited_at_unix;
              if (d.visited_at_unix > last) last = d.visited_at_unix;
            }
            out.Set("visit_count", visit_count);
            out.Set("first_visit_on_device", visit_count == 0);
            out.Set("first_visit_unix", static_cast<double>(first));
            out.Set("last_visit_unix", static_cast<double>(last));
            out.Set("total_words_read", static_cast<double>(total_words));
            self->ResolveJavascriptCallback(base::Value(cb_id),
                                            base::Value(std::move(out)));
          },
          weak_this, cb, host_copy, std::move(out)));
}

// ------------------------------------------------------------------
// HandleGetConnectionPath: v0 of the network-path visualizer.
//
// At this point we don't actually route through Tor or do live
// traceroutes — those land in Phase B. What we return now is honest
// about that: it's the URL structure, whether the tab is in an OTR
// session, and a templated 3-hop "you -> ISP -> destination" diagram.
// The same response shape will later carry the real circuit nodes.
//
// Args: [callback_id, url?]
// Returns: {url, host, scheme, is_https, is_anonymous_session,
//           hops: [{label, kind}], notes}
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleGetConnectionPath(
    const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 1u);
  const std::string callback_id = args[0].GetString();
  std::string url_str = (args.size() > 1 && args[1].is_string())
                            ? args[1].GetString()
                            : std::string();

  content::WebContents* webui_wc = web_ui()->GetWebContents();
  Browser* browser = chrome::FindBrowserWithTab(webui_wc);
  content::WebContents* active =
      (browser && browser->tab_strip_model())
          ? browser->tab_strip_model()->GetActiveWebContents()
          : nullptr;
  if (url_str.empty() && active) {
    url_str = active->GetLastCommittedURL().spec();
  }

  base::DictValue out;
  GURL g(url_str);
  if (!g.is_valid() || g.host().empty()) {
    out.Set("error", "no valid URL");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  out.Set("url", g.spec());
  out.Set("host", std::string(g.host()));
  out.Set("scheme", g.scheme());
  out.Set("is_https", g.SchemeIs("https"));

  bool otr = active && active->GetBrowserContext()->IsOffTheRecord();
  out.Set("is_anonymous_session", otr);

  // Three logical hops for direct browsing. Phase B will replace this
  // with real Tor circuit data when an Anonymous Tab is active.
  base::ListValue hops;
  auto add_hop = [&](const std::string& label, const std::string& kind,
                     const std::string& detail) {
    base::DictValue h;
    h.Set("label", label);
    h.Set("kind", kind);
    h.Set("detail", detail);
    hops.Append(std::move(h));
  };
  add_hop("You", "origin", "this device");
  add_hop("Your ISP", "isp",
          "the network you're connected to right now");
  add_hop(std::string(g.host()), "destination",
          g.SchemeIs("https") ? "TLS-encrypted to the destination"
                              : "PLAINTEXT — not HTTPS, anyone on the "
                                "path can read this");
  out.Set("hops", std::move(hops));
  out.Set(
      "notes",
      otr ? "Anonymous session — cookies and storage discarded on close."
          : "Direct connection. Tor multi-hop integration is in "
            "development — coming next.");
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}

// ------------------------------------------------------------------
// HandleSetJsForDomain: toggle JavaScript for |host| via
// HostContentSettingsMap. Persisted across launches by Chromium's
// content settings infrastructure. The active tab is reloaded so the
// change takes effect immediately.
//
// Args: [callback_id, host, enabled_bool]
// Returns: {success, host, enabled}
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleSetJsForDomain(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 3u);
  const std::string callback_id = args[0].GetString();
  std::string host = args[1].is_string() ? args[1].GetString() : "";
  bool enabled = args[2].is_bool() ? args[2].GetBool() : true;

  base::DictValue out;
  if (host.empty()) {
    out.Set("success", false);
    out.Set("error", "missing host");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  // Restrict to plausible hostname characters. Without this we'd
  // accept things like "*", "//evil.com", "..", "foo bar" that
  // either produce nonsense ContentSettingsPatterns or, worse,
  // produce a valid wildcard that disables JS far too broadly.
  // Code-review LOW #16. Allowed: a-z A-Z 0-9 . - _ ; 1-253 chars
  // (DNS hostname limit). Bare IP addresses are accepted by the
  // same regex.
  auto host_is_safe = [](const std::string& h) {
    if (h.empty() || h.size() > 253) return false;
    if (h[0] == '.' || h[0] == '-') return false;  // can't start with these
    for (char c : h) {
      bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') ||
                c == '.' || c == '-' || c == '_';
      if (!ok) return false;
    }
    // Reject the all-wildcard "*"-equivalent pattern just in case the
    // wildcard-like host slipped through (it shouldn't given the
    // alphabet check above, but cheap belt-and-suspenders).
    return h != "*";
  };
  if (!host_is_safe(host)) {
    out.Set("success", false);
    out.Set("error", "invalid host (allowed: letters, digits, . - _)");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  Profile* profile = Profile::FromBrowserContext(
      web_ui()->GetWebContents()->GetBrowserContext());
  HostContentSettingsMap* map =
      profile ? HostContentSettingsMapFactory::GetForProfile(profile)
              : nullptr;
  if (!map) {
    out.Set("success", false);
    out.Set("error", "no content settings");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  ContentSettingsPattern pattern =
      ContentSettingsPattern::FromString("[*.]" + host);
  if (!pattern.IsValid()) {
    out.Set("success", false);
    out.Set("error", "invalid host pattern");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  map->SetContentSettingCustomScope(
      pattern, ContentSettingsPattern::Wildcard(),
      ContentSettingsType::JAVASCRIPT,
      enabled ? CONTENT_SETTING_ALLOW : CONTENT_SETTING_BLOCK);

  // Reload the active tab so the change takes effect now.
  Browser* browser = chrome::FindBrowserWithTab(
      web_ui()->GetWebContents());
  if (browser && browser->tab_strip_model()) {
    content::WebContents* t =
        browser->tab_strip_model()->GetActiveWebContents();
    if (t && std::string(t->GetLastCommittedURL().host()) == host) {
      t->GetController().Reload(content::ReloadType::NORMAL,
                                /*check_for_repost=*/false);
    }
  }
  out.Set("success", true);
  out.Set("host", host);
  out.Set("enabled", enabled);
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}


// ------------------------------------------------------------------
// HandleAppendReceipt: Tier 3 — write one CSV row to
// ~/.moltbrowser/ledger.csv. Plaintext, dead-simple, so the user
// can pull the file into a spreadsheet without any extra tooling.
//
// Schema:
//   date,merchant,total,currency,source_url,saved_at_unix,items_json
//
// The items array is serialized as inline JSON inside its own
// quoted column — cheap and reversible, avoids the row-explosion
// you'd get from one-row-per-item.
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleAppendReceipt(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 2u);
  const std::string callback_id = args[0].GetString();
  base::DictValue out;
  if (!args[1].is_dict()) {
    out.Set("success", false);
    out.Set("error", "expected receipt dict");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  const base::DictValue& r = args[1].GetDict();
  std::string source_url = (args.size() > 2 && args[2].is_string())
                               ? args[2].GetString()
                               : std::string();

  const std::string* merchant = r.FindString("merchant");
  const std::string* date     = r.FindString("date");
  std::optional<double> total = r.FindDouble("total");
  const std::string* currency = r.FindString("currency");
  const base::ListValue* items = r.FindList("items");

  // Resolve ledger path: ~/.moltbrowser/ledger.csv
  base::FilePath home;
  if (!base::PathService::Get(base::DIR_HOME, &home)) {
    out.Set("success", false);
    out.Set("error", "no home dir");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  base::FilePath dir = home.AppendASCII(".moltbrowser");
  base::FilePath ledger = dir.AppendASCII("ledger.csv");

  // CSV-escape: wrap in quotes, double internal quotes.
  auto csv = [](const std::string& s) {
    bool need_quote = s.find(',') != std::string::npos ||
                      s.find('"') != std::string::npos ||
                      s.find('\n') != std::string::npos;
    if (!need_quote) return s;
    std::string out;
    out.reserve(s.size() + 4);
    out += '"';
    for (char c : s) { if (c == '"') out += '"'; out += c; }
    out += '"';
    return out;
  };

  // Serialize items as inline JSON.
  std::string items_json = "[]";
  if (items) {
    base::Value items_val(items->Clone());
    base::JSONWriter::Write(items_val, &items_json);
  }

  std::string row;
  row += csv(date ? *date : "") + ",";
  row += csv(merchant ? *merchant : "") + ",";
  row += (total ? base::NumberToString(*total) : "") + ",";
  row += csv(currency ? *currency : "USD") + ",";
  row += csv(source_url) + ",";
  row += base::NumberToString(static_cast<int64_t>(time(nullptr))) + ",";
  row += csv(items_json) + "\n";

  bool wrote = false;
  {
    ScopedAllowBlockingForMolt allow;
    base::CreateDirectory(dir);
    // Write header if the file is brand new.
    if (!base::PathExists(ledger)) {
      const char* header =
          "date,merchant,total,currency,source_url,saved_at_unix,"
          "items_json\n";
      wrote = base::WriteFile(ledger, header);
    } else {
      wrote = true;
    }
    if (wrote) {
      wrote = base::AppendToFile(ledger, row);
    }
  }
  if (!wrote) {
    out.Set("success", false);
    out.Set("error", "failed to write ledger");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  out.Set("success", true);
  out.Set("path", ledger.AsUTF8Unsafe());
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}

// ------------------------------------------------------------------
// HandleSavePlanScript: Tier 3 — turn a list of natural-language step
// lines (from the LLM's /plan output) into a Script and save it via
// AutomationStorage. The parser is intentionally permissive — small
// models are inconsistent about formatting, so we accept any leading
// verb token (navigate / click / type / wait-for / scroll / extract /
// notify) and try to fill out the rest of the Step fields heuristically.
//
// We never auto-run the resulting script — it's saved as CASUAL trust
// so the user has to open the manager UI and click Run. That's the
// safety boundary against an LLM plan that hallucinates a navigation
// to a phishing URL.
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleSavePlanScript(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 2u);
  const std::string callback_id = args[0].GetString();
  base::DictValue out;
  if (!args[1].is_dict()) {
    out.Set("success", false);
    out.Set("error", "expected plan dict");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  const base::DictValue& plan = args[1].GetDict();
  const std::string* name_in = plan.FindString("name");
  const std::string* task_in = plan.FindString("task");
  const base::ListValue* step_lines = plan.FindList("steps");
  if (!step_lines || step_lines->empty()) {
    out.Set("success", false);
    out.Set("error", "no steps");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  molt_ai::automation::Script s;
  s.id = base::StringPrintf("plan-%lx",
                             static_cast<unsigned long>(time(nullptr)));
  s.name = (name_in && !name_in->empty()) ? *name_in
                                          : std::string("Untitled plan");
  s.created_at_unix = static_cast<int64_t>(time(nullptr));
  s.trigger.type = molt_ai::automation::TriggerType::MANUAL;
  s.security.trust = molt_ai::automation::TrustLevel::CASUAL;
  s.security.max_runtime_seconds = 300;
  if (task_in) {
    // Stash the original task description in a default variable so the
    // Manager UI can show "Generated for: <task>".
    s.default_variables["plan_task"] = *task_in;
  }

  auto parse_line = [](const std::string& line) {
    molt_ai::automation::Step step;
    step.description = line;
    step.timeout_ms = 10000;
    // Tokenize on whitespace; first token is the verb.
    std::vector<std::string> parts = base::SplitString(
        line, " ", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
    if (parts.empty()) return step;
    std::string verb = parts[0];
    std::transform(verb.begin(), verb.end(), verb.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    auto join_from = [&](size_t i) {
      std::string out;
      for (size_t k = i; k < parts.size(); ++k) {
        if (!out.empty()) out += " ";
        out += parts[k];
      }
      return out;
    };
    if (verb == "navigate" || verb == "goto" || verb == "open") {
      step.type = molt_ai::automation::StepType::NAVIGATE;
      step.target = join_from(1);
    } else if (verb == "click") {
      step.type = molt_ai::automation::StepType::CLICK;
      step.target = join_from(1);
    } else if (verb == "type" && parts.size() >= 3) {
      step.type = molt_ai::automation::StepType::TYPE;
      step.target = parts[1];
      step.value = join_from(2);
    } else if (verb == "wait-for" || verb == "waitfor") {
      step.type = molt_ai::automation::StepType::WAIT_FOR;
      step.target = join_from(1);
      step.timeout_ms = 15000;
    } else if (verb == "scroll") {
      step.type = molt_ai::automation::StepType::SCROLL;
      step.value = parts.size() > 1 ? parts[1] : "600";
    } else if (verb == "extract" && parts.size() >= 4) {
      // extract <selector> as <varname>
      step.type = molt_ai::automation::StepType::EXTRACT;
      step.target = parts[1];
      // Find "as" token; varname is what follows.
      for (size_t i = 2; i + 1 < parts.size(); ++i) {
        if (parts[i] == "as" || parts[i] == "AS") {
          step.store_as = parts[i + 1];
          break;
        }
      }
    } else if (verb == "notify") {
      step.type = molt_ai::automation::StepType::NOTIFY;
      step.target = "MoltBrowser";
      step.value = join_from(1);
    } else {
      step.type = molt_ai::automation::StepType::UNKNOWN;
    }
    return step;
  };

  int parsed_ok = 0;
  for (const base::Value& v : *step_lines) {
    if (!v.is_string()) continue;
    molt_ai::automation::Step step = parse_line(v.GetString());
    if (step.type != molt_ai::automation::StepType::UNKNOWN) {
      ++parsed_ok;
    }
    s.steps.push_back(std::move(step));
  }
  if (parsed_ok == 0) {
    out.Set("success", false);
    out.Set("error", "no recognizable steps");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  bool saved = false;
  {
    ScopedAllowBlockingForMolt allow;
    molt_ai::automation::AutomationStorage storage;
    storage.EnsureDirectory();
    saved = storage.Save(s);
  }
  if (!saved) {
    out.Set("success", false);
    out.Set("error", "AutomationStorage::Save failed");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  out.Set("success", true);
  out.Set("script_id", s.id);
  out.Set("step_count", static_cast<int>(s.steps.size()));
  out.Set("parsed_ok", parsed_ok);
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}


// ==================================================================
// Tier 4: Form filler v2 — LLM fallback path.
//
// The existing /fill works by heuristic matching on input
// name/placeholder/autocomplete. When the heuristic misses (sites
// with non-English labels, custom widget libraries, or fields named
// "fld_3"), this path collects the page's visible input attributes,
// sends them to the LLM with the user's profile, and asks the LLM
// to emit a JSON mapping of field selector → profile key. We then
// re-run a filler with that mapping.
//
// This is NOT bypass: if the heuristic finds matches, we still use
// them. The LLM only fills the leftovers. Visible in /fill --ai or
// `runFormFillAI` IPC.
//
// Args: [callback_id]
// ==================================================================

// JS probe that enumerates visible inputs and returns a compact
// summary the LLM can reason over.
static constexpr char kFormProbeJS[] = R"JS(
(function() {
  function isVisible(el) {
    var r = el.getBoundingClientRect();
    if (r.width === 0 || r.height === 0) return false;
    var s = getComputedStyle(el);
    return s.visibility !== 'hidden' && s.display !== 'none';
  }
  function cssPath(el) {
    if (el.id) return '#' + CSS.escape(el.id);
    var n = (el.getAttribute('name') || '').trim();
    if (n) return el.tagName.toLowerCase() + '[name="' +
                 n.replace(/"/g,'\\"') + '"]';
    // Fallback: structural path.
    var parts = [];
    var cur = el;
    for (var i = 0; cur && i < 6; i++) {
      var sib = cur, idx = 1;
      while ((sib = sib.previousElementSibling) != null) {
        if (sib.tagName === cur.tagName) idx++;
      }
      parts.unshift(cur.tagName.toLowerCase() + ':nth-of-type(' + idx + ')');
      cur = cur.parentElement;
    }
    return parts.join(' > ');
  }
  function labelFor(el) {
    if (el.id) {
      var L = document.querySelector('label[for="' + el.id + '"]');
      if (L && L.innerText) return L.innerText.trim();
    }
    var p = el.parentElement;
    if (p && p.tagName === 'LABEL') return p.innerText.trim();
    return '';
  }
  var inputs = Array.from(document.querySelectorAll(
      'input, textarea, select'));
  var rows = [];
  inputs.forEach(function(el) {
    if (!isVisible(el)) return;
    var t = (el.type || '').toLowerCase();
    if (t === 'hidden' || t === 'submit' || t === 'button' ||
        t === 'password') return;
    if (el.disabled || el.readOnly) return;
    rows.push({
      selector: cssPath(el),
      type: t,
      name: el.getAttribute('name') || '',
      id: el.id || '',
      placeholder: el.getAttribute('placeholder') || '',
      autocomplete: el.getAttribute('autocomplete') || '',
      label: labelFor(el).slice(0, 60)
    });
    if (rows.length >= 40) return;
  });
  return JSON.stringify({inputs: rows});
})()
)JS";

// JS shim that applies a {selector: value} map to the active page.
static constexpr char kFormFillByMapJS[] = R"JS(
(function(map) {
  function setNative(el, value) {
    var proto = el.tagName === 'TEXTAREA'
        ? HTMLTextAreaElement.prototype
        : HTMLInputElement.prototype;
    var setter = Object.getOwnPropertyDescriptor(proto, 'value').set;
    setter.call(el, value);
    el.dispatchEvent(new Event('input', {bubbles: true}));
    el.dispatchEvent(new Event('change', {bubbles: true}));
  }
  var filled = 0, total = 0;
  Object.keys(map).forEach(function(sel){
    total++;
    var el;
    try { el = document.querySelector(sel); } catch (e) { return; }
    if (!el) return;
    setNative(el, String(map[sel]));
    filled++;
  });
  return {filled: filled, total: total};
})
)JS";

void MoltAIChatHandler::HandleRunFormFillAI(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 1u);
  const std::string callback_id = args[0].GetString();
  base::DictValue out;

  // Resolve target tab.
  content::WebContents* webui_wc = web_ui()->GetWebContents();
  Browser* browser = chrome::FindBrowserWithTab(webui_wc);
  if (!browser || !browser->tab_strip_model()) {
    out.Set("success", false);
    out.Set("error", "no owning browser");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  content::WebContents* target =
      browser->tab_strip_model()->GetActiveWebContents();
  if (!target || !target->GetLastCommittedURL().SchemeIsHTTPOrHTTPS()) {
    out.Set("success", false);
    out.Set("error", "active tab is not an http(s) page");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  // Load profile.
  base::DictValue profile;
  {
    ScopedAllowBlockingForMolt allow;
    molt_ai::profile::MoltProfileStore store;
    profile = store.Load();
  }
  if (profile.empty()) {
    out.Set("success", false);
    out.Set("error", "profile is empty — set one via /profile first");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  std::string cb_id = callback_id;
  auto weak_this = weak_ptr_factory_.GetWeakPtr();
  base::DictValue profile_copy = profile.Clone();

  // Step 1: enumerate visible form fields.
  target->GetPrimaryMainFrame()->ExecuteJavaScriptInIsolatedWorld(
      base::UTF8ToUTF16(std::string(kFormProbeJS)),
      base::BindOnce(
          [](base::WeakPtr<MoltAIChatHandler> self, std::string cb_id,
             content::WebContents* target,
             base::DictValue profile,
             base::Value probe_result) {
            if (!self) return;
            base::DictValue out;
            if (!probe_result.is_string()) {
              out.Set("success", false);
              out.Set("error", "form probe returned no data");
              self->ResolveJavascriptCallback(
                  base::Value(cb_id), base::Value(std::move(out)));
              return;
            }
            auto parsed = base::JSONReader::Read(probe_result.GetString(),
                                                  /*options=*/0);
            if (!parsed || !parsed->is_dict()) {
              out.Set("success", false);
              out.Set("error", "probe parse failed");
              self->ResolveJavascriptCallback(
                  base::Value(cb_id), base::Value(std::move(out)));
              return;
            }
            const base::ListValue* inputs =
                parsed->GetDict().FindList("inputs");
            if (!inputs || inputs->empty()) {
              out.Set("success", false);
              out.Set("error", "no fillable inputs found");
              self->ResolveJavascriptCallback(
                  base::Value(cb_id), base::Value(std::move(out)));
              return;
            }

            // Step 2: build LLM prompt. We ask for a strict JSON
            // object mapping CSS selector → string value to type.
            std::string inputs_json;
            base::JSONWriter::Write(*inputs, &inputs_json);
            std::string profile_json;
            base::JSONWriter::Write(profile, &profile_json);
            std::string prompt =
                "You are a web form filler. Below is the user's saved "
                "profile (key → value) and the visible form fields on "
                "the current page. Match each field to a profile value "
                "if and only if the match is unambiguous. Respond with "
                "ONE JSON object: keys are the field selectors, values "
                "are the strings to type. Skip any field you can't "
                "confidently match. No prose, no markdown.\n\n"
                "Profile:\n" + profile_json +
                "\n\nForm fields:\n" + inputs_json +
                "\n\nMapping:";

            // Use the existing sendPrompt pipeline by reaching into the
            // runtime directly. We need the streaming machinery so we
            // get the final text. Simplest path: kick off a normal
            // sendPrompt and let the chat finalize it; the user sees
            // the JSON in the chat, then a follow-up action fills.
            //
            // For a cleaner v1, we instead call the runtime synchronously
            // through GetOrCreateRuntime() and a tiny wrapper. To keep
            // this commit reviewable, route through the existing
            // streaming sendPrompt path: emit the prompt text directly
            // and have the JS side (which already drives /receipt and
            // /plan) own the parse + fill step. That keeps the
            // contract for /fill --ai identical to /receipt: LLM
            // streams JSON, JS parses, JS calls a tiny IPC to apply.
            //
            // Here we just return the prompt + the JS shim so the
            // JS layer can stream the LLM call itself.
            out.Set("success", true);
            out.Set("phase", "ready_for_llm");
            out.Set("prompt", prompt);
            self->ResolveJavascriptCallback(
                base::Value(cb_id), base::Value(std::move(out)));
          },
          weak_this, cb_id, target, std::move(profile_copy)),
      /*world_id=*/1);
}

// HandleApplyFormFillMap: phase 2 of the Form Filler v2. The JS
// layer has already streamed the LLM, parsed its JSON output, and
// now hands us a dict of {selector: value} pairs to apply.
//
// Args: [callback_id, {map: {selector: value, ...}}]
// Returns: {success, filled, total}
void MoltAIChatHandler::HandleApplyFormFillMap(
    const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 2u);
  const std::string callback_id = args[0].GetString();
  base::DictValue out;
  if (!args[1].is_dict()) {
    out.Set("success", false);
    out.Set("error", "expected {map: {...}} dict");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  const base::DictValue& d = args[1].GetDict();
  const base::DictValue* map = d.FindDict("map");
  if (!map || map->empty()) {
    out.Set("success", false);
    out.Set("error", "empty mapping");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  // Resolve target tab.
  content::WebContents* webui_wc = web_ui()->GetWebContents();
  Browser* browser = chrome::FindBrowserWithTab(webui_wc);
  if (!browser || !browser->tab_strip_model()) {
    out.Set("success", false);
    out.Set("error", "no owning browser");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  content::WebContents* target =
      browser->tab_strip_model()->GetActiveWebContents();
  if (!target || !target->GetLastCommittedURL().SchemeIsHTTPOrHTTPS()) {
    out.Set("success", false);
    out.Set("error", "active tab is not an http(s) page");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  // Serialize the map and apply via the shim.
  std::string map_json;
  base::Value map_val(map->Clone());
  base::JSONWriter::Write(map_val, &map_json);
  std::string js = std::string(kFormFillByMapJS) + "(" + map_json + ")";

  std::string cb_id = callback_id;
  auto weak_this = weak_ptr_factory_.GetWeakPtr();
  target->GetPrimaryMainFrame()->ExecuteJavaScriptInIsolatedWorld(
      base::UTF8ToUTF16(js),
      base::BindOnce(
          [](base::WeakPtr<MoltAIChatHandler> self, std::string cb_id,
             base::Value v) {
            if (!self) return;
            base::DictValue r;
            if (v.is_dict()) {
              const base::DictValue& d = v.GetDict();
              auto filled = d.FindInt("filled");
              auto total = d.FindInt("total");
              r.Set("success", filled.value_or(0) > 0);
              r.Set("filled", filled.value_or(0));
              r.Set("total", total.value_or(0));
            } else {
              r.Set("success", false);
              r.Set("error", "apply script returned no data");
            }
            self->ResolveJavascriptCallback(base::Value(cb_id),
                                            base::Value(std::move(r)));
          },
          weak_this, cb_id),
      /*world_id=*/1);
}

// ==================================================================
// Tier 5: Voice mode
//
// The client side (chat_ui.cc) records audio via WebAudio, encodes
// it as a 16-kHz mono WAV byte string, base64-encodes the bytes, and
// posts here. We decode, hand to VoiceService, and stream the
// transcription back via callback. Audio never leaves the device.
// ==================================================================
void MoltAIChatHandler::HandleTranscribeAudio(
    const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 2u);
  const std::string callback_id = args[0].GetString();
  const std::string b64 = args[1].is_string() ? args[1].GetString() : "";
  base::DictValue out;
  if (b64.empty()) {
    out.Set("success", false);
    out.Set("error", "no audio");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  // Pre-decode size cap. The recorder caps at 30 s of 16-kHz mono
  // PCM = ~960 KB raw + WAV header. Base64 inflates by ~4/3 so the
  // worst-case legitimate payload is ~1.3 MB. 8 MB gives generous
  // headroom for a custom uploader without letting a compromised
  // WebUI OOM the browser on a gigabyte string. Code-review MEDIUM #9.
  constexpr size_t kMaxAudioB64Bytes = 8 << 20;
  if (b64.size() > kMaxAudioB64Bytes) {
    out.Set("success", false);
    out.Set("error", "audio exceeds max size (8 MiB base64)");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  std::string wav_bytes;
  if (!base::Base64Decode(b64, &wav_bytes)) {
    out.Set("success", false);
    out.Set("error", "base64 decode failed");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  if (wav_bytes.size() < 44) {
    out.Set("success", false);
    out.Set("error", "WAV too short");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  std::string cb = callback_id;
  auto weak_this = weak_ptr_factory_.GetWeakPtr();
  molt_ai::voice::VoiceService::Get()->Transcribe(
      std::move(wav_bytes),
      base::BindOnce(
          [](base::WeakPtr<MoltAIChatHandler> self, std::string cb,
             molt_ai::voice::TranscribeResult r) {
            if (!self) return;
            base::DictValue out;
            out.Set("success", r.success);
            out.Set("text", r.text);
            out.Set("error", r.error);
            out.Set("binary_source", r.binary_source);
            out.Set("binary_path", r.binary_path);
            out.Set("duration_ms", r.duration_ms);
            if (!r.success && r.binary_source == "none") {
              out.Set(
                  "install_hint",
                  "No whisper binary is bundled with this build. "
                  "Run scripts/bundle-whisper.sh, or use a fresh "
                  "release DMG which ships with Whisper inside.");
            }
            self->ResolveJavascriptCallback(base::Value(cb),
                                            base::Value(std::move(out)));
          },
          weak_this, cb));
}

// ------------------------------------------------------------------
// HandleStartWebAgent — launch a ReAct-loop web browsing agent.
//
// JS calls: chrome.send('startWebAgent', [callback_id, goal])
//   - Each completed step fires: FireWebUIListener('agent-step', {...})
//   - Final result resolves: ResolveJavascriptCallback(callback_id, {...})
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleStartWebAgent(const base::ListValue& args) {
  AllowJavascript();
  if (args.size() < 2) return;

  const std::string callback_id = args[0].GetString();
  const std::string goal        = args[1].GetString();

  if (goal.empty()) {
    base::DictValue err;
    err.Set("success", false);
    err.Set("result", "Goal cannot be empty");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(err)));
    return;
  }

  // Find the active browsing tab (not the side-panel WebContents itself).
  content::WebContents* webui_wc = web_ui()->GetWebContents();
  Browser* browser = chrome::FindBrowserWithTab(webui_wc);
  content::WebContents* target_wc = nullptr;
  if (browser) {
    target_wc = browser->tab_strip_model()->GetActiveWebContents();
  }
  if (!target_wc) {
    base::DictValue err;
    err.Set("success", false);
    err.Set("result", "No active browser tab found");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(err)));
    return;
  }

  // Cancel any previous agent run.
  if (web_agent_) web_agent_->Cancel();

  molt_ai::BrowserAIRuntime* runtime = GetOrCreateRuntime();
  web_agent_ = std::make_unique<molt_ai::WebAgent>(target_wc, runtime);

  auto weak_this = weak_ptr_factory_.GetWeakPtr();

  // Step callback: stream each completed step to the UI in real time.
  auto on_step = base::BindRepeating(
      [](base::WeakPtr<MoltAIChatHandler> self,
         const molt_ai::WebAgent::Step& step) {
        if (!self || !self->IsJavascriptAllowed()) return;
        base::DictValue d;
        d.Set("number",      step.number);
        d.Set("action",      step.action);
        d.Set("target",      step.target);
        d.Set("value",       step.value);
        d.Set("reason",      step.reason);
        d.Set("observation", step.observation.substr(
                                 0, std::min<int>(500,
                                    static_cast<int>(step.observation.size()))));
        d.Set("success",     step.success);
        d.Set("note",        step.note);
        self->FireWebUIListener("agent-step", base::Value(std::move(d)));
      },
      weak_this);

  // Done callback: resolve the JS promise with the final result.
  auto on_done = base::BindOnce(
      [](base::WeakPtr<MoltAIChatHandler> self, std::string cb_id,
         bool success, std::string result) {
        if (!self || !self->IsJavascriptAllowed()) return;
        self->web_agent_.reset();
        base::DictValue d;
        d.Set("success", success);
        d.Set("result",  result);
        self->ResolveJavascriptCallback(base::Value(cb_id),
                                        base::Value(std::move(d)));
      },
      weak_this, callback_id);

  web_agent_->Start(goal, std::move(on_step), std::move(on_done));

  LOG(INFO) << "[MoltAI] Web agent started. Goal: " << goal;
}

// ------------------------------------------------------------------
// HandleCancelWebAgent — stop a running agent immediately.
// JS calls: chrome.send('cancelWebAgent', [])
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleCancelWebAgent(const base::ListValue& args) {
  if (web_agent_) {
    web_agent_->Cancel();
    web_agent_.reset();
    LOG(INFO) << "[MoltAI] Web agent cancelled by user";
  }
}
