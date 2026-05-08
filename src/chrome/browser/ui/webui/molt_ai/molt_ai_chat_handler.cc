// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltAIChatHandler: WebUI message handler bridging the AI chat frontend
// to BrowserAIRuntime for local llama.cpp inference.

#include "chrome/browser/ui/webui/molt_ai/molt_ai_chat_handler.h"

#include <memory>
#include <string>
#include <utility>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/strings/utf_string_conversions.h"
#include "base/system/sys_info.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/common/molt_blocking_scope.h"
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
      "privacy. Instructions:\n"
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
