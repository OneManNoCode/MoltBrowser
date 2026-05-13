// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltAIChatHandler: WebUI message handler bridging the AI chat frontend
// to BrowserAIRuntime for local llama.cpp inference.

#include "chrome/browser/ui/webui/molt_ai/molt_ai_chat_handler.h"

#include <algorithm>
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/system/sys_info.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "chrome/browser/bookmarks/bookmark_model_factory.h"
#include "chrome/browser/molt_ai/automation/agent_inbox_registry.h"
#include "chrome/browser/molt_ai/automation/automation_runner.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/automation/automation_scheduler_factory.h"
#include "chrome/browser/molt_ai/automation/automation_scheduler_service.h"
#include "chrome/browser/molt_ai/automation/automation_storage.h"
#include "chrome/browser/molt_ai/common/molt_blocking_scope.h"
#include "chrome/browser/molt_ai/profile/molt_profile_store.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "components/bookmarks/browser/bookmark_model.h"
#include "chrome/browser/molt_ai/memory/memory_service.h"
#include "chrome/browser/molt_ai/memory/memory_service_factory.h"
#include "chrome/browser/molt_ai/memory/memory_types.h"
#include "chrome/browser/molt_ai/runtime/browser_ai_runtime.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"

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
      "soon as it's emitted, so you can interleave them with prose.\n"
      "  [[ACTION click:<css-selector>]]\n"
      "  [[ACTION type:<css-selector>|<text-to-type>]]\n"
      "  [[ACTION select:<css-selector>|<option-value>]]\n"
      "  [[ACTION hover:<css-selector>]]\n"
      "  [[ACTION right-click:<css-selector>]]\n"
      "  [[ACTION drag:<source-selector>|<target-selector>]]\n"
      "  [[ACTION scroll:<pixels>]]\n"
      "  [[ACTION navigate:<full-url>]]\n"
      "  [[ACTION wait:<milliseconds>]]                      (sleep)\n"
      "  [[ACTION wait-for:<css-selector>|<timeout-ms>]]     (poll until visible)\n"
      "\n"
      "Multi-step macros: emit tokens in the order you want them to "
      "run. The dispatcher executes them sequentially and waits for "
      "each to finish before starting the next. Use wait-for after "
      "navigations or clicks that load new content, e.g.:\n"
      "  [[ACTION navigate:https://example.com/login]]\n"
      "  [[ACTION wait-for:input[name=username]|3000]]\n"
      "  [[ACTION type:input[name=username]|raj]]\n"
      "  [[ACTION type:input[name=password]|<from-user>]]\n"
      "  [[ACTION click:button[type=submit]]]\n"
      "\n"
      "Conditionals: there is no if/else syntax yet. If you don't "
      "know whether a selector will exist (CAPTCHA, A/B test), use "
      "wait-for with a short timeout — if it doesn't appear the run "
      "fails cleanly and the user sees the failure in the chat.\n"
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
      home_dir.Append(".moltbrowser").Append("settings.json");
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
  const std::string model_id = args[1].GetString();

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
  const std::string prompt_text = args[1].GetString();

  // Optional: conversation history as pre-formatted string
  std::string history_text;
  if (args.size() >= 3u && args[2].is_string()) {
    history_text = args[2].GetString();
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
  const std::string model_id = args[1].GetString();

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
            base::FilePath model_dir(info.file_path);
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
            base::FilePath partial_path(info.file_path + ".partial");
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
  base::FilePath file_path(info.file_path);
  std::string filename = file_path.BaseName().value();
  std::string url = "https://huggingface.co/" + info.huggingface_id +
                    "/resolve/main/" + filename;
  base::FilePath partial_path(info.file_path + ".partial");

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
  const std::string model_id = args[1].GetString();

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
      base::FilePath partial_path(download_final_path_.value() + ".partial");
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
  const std::string history_json = args[1].GetString();

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
      home_dir.Append(".moltbrowser").Append("chat_exports").Append(filename);

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
  result.Set("path", file_path.value());
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

  // Resolve the target tab: the active tab of the Browser that owns
  // this WebUI's WebContents (the side-panel host).
  content::WebContents* webui_wc = web_ui()->GetWebContents();
  Browser* browser = chrome::FindBrowserWithTab(webui_wc);
  if (!browser || !browser->tab_strip_model()) {
    result.Set("success", false);
    result.Set("error", "no owning browser");
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
    // JS-escape the selector + value. We do simple JSON.stringify-style
    // escaping inside the template since the values are user-supplied.
    auto js_quote = [](const std::string& s) {
      std::string out = "\"";
      for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (c == '\n') { out += "\\n"; continue; }
        if (c == '\r') { out += "\\r"; continue; }
        out += c;
      }
      out += '"';
      return out;
    };
    std::string js;
    if (t == "select") {
      std::string v = value ? *value : "";
      js = "(function(){var el=document.querySelector(" + js_quote(*sel) +
           ");if(!el)return false;el.value=" + js_quote(v) +
           ";el.dispatchEvent(new Event('input',{bubbles:true}));"
           "el.dispatchEvent(new Event('change',{bubbles:true}));"
           "return true;})()";
    } else if (t == "hover") {
      js = "(function(){var el=document.querySelector(" + js_quote(*sel) +
           ");if(!el)return false;"
           "['mouseover','mouseenter','mousemove'].forEach(function(t){"
           "el.dispatchEvent(new MouseEvent(t,{bubbles:true,"
           "cancelable:true,view:window}));});return true;})()";
    } else if (t == "right-click") {
      // Synthesize a contextmenu event with button=2. Most sites'
      // custom right-click menus listen for this; the native macOS/
      // OS menu won't appear, but page-level menus do.
      js = "(function(){var el=document.querySelector(" + js_quote(*sel) +
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
           "var src=document.querySelector(" + js_quote(*sel) + ");"
           "var tgt=document.querySelector(" + js_quote(tgt) + ");"
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
