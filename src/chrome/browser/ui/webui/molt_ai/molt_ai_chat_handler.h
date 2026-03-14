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

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "content/public/browser/web_ui_message_handler.h"

class Profile;

namespace molt_ai {
class BrowserAIRuntime;
}  // namespace molt_ai

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

  // Async callbacks (run on UI thread after background work)
  void OnModelLoaded(std::string callback_id, bool success,
                     const std::string& error);
  void OnPromptComplete(std::string callback_id, bool success,
                        const std::string& full_text,
                        const std::string& error);

  // Ensure BrowserAIRuntime is created and initialized
  molt_ai::BrowserAIRuntime* GetOrCreateRuntime();

  raw_ptr<Profile> profile_;
  std::unique_ptr<molt_ai::BrowserAIRuntime> runtime_;
  bool model_loaded_ = false;
  std::string active_prompt_callback_id_;

  base::WeakPtrFactory<MoltAIChatHandler> weak_ptr_factory_{this};
};

#endif  // CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_CHAT_HANDLER_H_
