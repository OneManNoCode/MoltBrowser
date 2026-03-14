// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltAIChatHandler: WebUI message handler bridging the AI chat frontend
// to BrowserAIRuntime for local llama.cpp inference.

#include "chrome/browser/ui/webui/molt_ai/molt_ai_chat_handler.h"

#include <memory>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/runtime/browser_ai_runtime.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"

MoltAIChatHandler::MoltAIChatHandler(Profile* profile)
    : profile_(profile) {}

MoltAIChatHandler::~MoltAIChatHandler() {
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
}

void MoltAIChatHandler::OnJavascriptAllowed() {
  // Runtime will be created lazily on first use
}

void MoltAIChatHandler::OnJavascriptDisallowed() {
  weak_ptr_factory_.InvalidateWeakPtrs();
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

  // Gather model info
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

  // Notify UI that we're loading if model isn't ready
  if (!model_loaded_) {
    FireWebUIListener("model-status", base::Value("loading"),
                      base::Value("tinyllama-1.1b"));
  }

  // Run model loading + streaming inference on a background thread.
  // Both LoadModel and StreamPrompt block, so they must run off the UI thread.
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
            // Auto-load model on background thread if needed
            if (needs_load) {
              LOG(INFO) << "[MoltAI] Auto-loading TinyLlama on background thread...";
              // Notify UI that we're loading (via model-status, not ai-token)
              content::GetUIThreadTaskRunner({})->PostTask(
                  FROM_HERE,
                  base::BindOnce(
                      [](base::WeakPtr<MoltAIChatHandler> self) {
                        if (self && self->IsJavascriptAllowed()) {
                          self->FireWebUIListener(
                              "model-status",
                              base::Value("loading"),
                              base::Value("Loading TinyLlama model..."));
                        }
                      },
                      weak_self));

              bool loaded = rt->LoadModel("tinyllama-1.1b");
              if (!loaded) {
                LOG(ERROR) << "[MoltAI] Failed to load TinyLlama model";
                molt_ai::GenerationResult fail;
                fail.success = false;
                fail.error_message =
                    "Failed to load model. Check ~/.moltbrowser/models/";
                return fail;
              }
              LOG(INFO) << "[MoltAI] TinyLlama loaded successfully";

              // Notify UI that model is ready
              content::GetUIThreadTaskRunner({})->PostTask(
                  FROM_HERE,
                  base::BindOnce(
                      [](base::WeakPtr<MoltAIChatHandler> self) {
                        if (self && self->IsJavascriptAllowed()) {
                          self->FireWebUIListener(
                              "model-status", base::Value("ready"),
                              base::Value("tinyllama-1.1b"));
                        }
                      },
                      weak_self));
            }

            molt_ai::PromptOptions opts;
            opts.max_tokens = 512;
            opts.temperature = 0.7f;
            opts.stream = true;

            // Format as TinyLlama chat prompt with optional history
            std::string system_msg =
                "You are MoltBrowser AI, a helpful local AI "
                "assistant built into the MoltBrowser web browser. You run "
                "entirely on the user's device for privacy. Be concise and "
                "helpful.";
            if (!page_ctx.empty()) {
              system_msg += " The user is currently viewing: " + page_ctx;
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
          page_context, !model_loaded_, weak_ptr_factory_.GetWeakPtr()),
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
