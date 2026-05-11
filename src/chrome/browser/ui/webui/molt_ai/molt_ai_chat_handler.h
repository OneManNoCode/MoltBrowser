// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltAIChatHandler: WebUIMessageHandler that bridges the AI chat WebUI
// JavaScript frontend to BrowserAIRuntime for local LLM inference.
//
// JS → C++:  chrome.send('sendPrompt', [callback_id, prompt_text])
//            chrome.send('loadModel', [callback_id, model_id])
//            chrome.send('cancelGeneration', [])
//            chrome.send('getModelStatus', [callback_id])
//            chrome.send('initChat', [callback_id])
//
// C++ → JS:  FireWebUIListener('ai-token', {token, is_done, ...})
//            ResolveJavascriptCallback(callback_id, result)

#ifndef CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_CHAT_HANDLER_H_
#define CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_CHAT_HANDLER_H_

#include <memory>
#include <string>

#include "base/files/file_path.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/runtime/browser_ai_runtime.h"
#include "content/public/browser/web_ui_message_handler.h"

class Profile;

namespace network {
class SimpleURLLoader;
}  // namespace network

class MoltAIChatHandler : public content::WebUIMessageHandler {
 public:
  explicit MoltAIChatHandler(Profile* profile);
  ~MoltAIChatHandler() override;

  MoltAIChatHandler(const MoltAIChatHandler&) = delete;
  MoltAIChatHandler& operator=(const MoltAIChatHandler&) = delete;

  // content::WebUIMessageHandler:
  void RegisterMessages() override;
  void OnJavascriptAllowed() override;
  void OnJavascriptDisallowed() override;

 private:
  // Message handlers (called from JavaScript via chrome.send)
  void HandleInitChat(const base::ListValue& args);
  void HandleSendPrompt(const base::ListValue& args);
  void HandleLoadModel(const base::ListValue& args);
  void HandleCancelGeneration(const base::ListValue& args);
  void HandleGetModelStatus(const base::ListValue& args);
  void HandleGetPageContext(const base::ListValue& args);
  void HandleDownloadModel(const base::ListValue& args);
  void HandleDeleteModel(const base::ListValue& args);
  void HandleGetPageContent(const base::ListValue& args);
  void HandleCancelDownload(const base::ListValue& args);
  void HandleExportHistory(const base::ListValue& args);
  // Side panel automation bridge. Runs a single click/type/scroll/
  // navigate against the currently-active tab in the Browser that
  // owns this WebUI's WebContents.
  void HandleRunMoltAction(const base::ListValue& args);

  // Async callbacks (run on UI thread after background work)
  void FinishInitChat(std::string callback_id,
                      base::DictValue settings_dict);
  void OnModelLoaded(std::string callback_id, bool success,
                     const std::string& error);
  void OnPromptComplete(std::string callback_id, bool success,
                        const std::string& full_text,
                        const std::string& error);
  void OnDownloadProgress(uint64_t current);
  void OnDownloadComplete(base::FilePath path);
  void OnDownloadPrecheckComplete(std::string callback_id,
                                  std::string model_id,
                                  molt_ai::ModelInfo info,
                                  base::DictValue precheck);
  void FinishDownload(bool success, int net_error);
  void OnModelDeleted(std::string callback_id, std::string model_id,
                      bool success);
  void OnHistoryExported(std::string callback_id,
                         base::FilePath file_path,
                         std::string filename,
                         bool success);
  void OnPageContentExtracted(std::string callback_id, base::Value result);

  // Ensure BrowserAIRuntime is created and initialized
  molt_ai::BrowserAIRuntime* GetOrCreateRuntime();

  raw_ptr<Profile> profile_;
  std::unique_ptr<molt_ai::BrowserAIRuntime> runtime_;
  bool model_loaded_ = false;
  std::string active_prompt_callback_id_;

  // Model download state
  std::unique_ptr<network::SimpleURLLoader> url_loader_;
  std::string download_callback_id_;
  std::string downloading_model_id_;
  uint64_t download_total_bytes_ = 0;
  uint64_t download_resume_bytes_ = 0;
  base::FilePath download_final_path_;
  base::TimeTicks download_start_time_;
  uint64_t download_last_bytes_ = 0;
  base::TimeTicks download_last_time_;

  base::WeakPtrFactory<MoltAIChatHandler> weak_ptr_factory_{this};
};

#endif  // CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_CHAT_HANDLER_H_
