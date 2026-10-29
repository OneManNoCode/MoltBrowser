// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The agent panel's "Type a voice command" field (chrome://molt-ai-agent,
// with chrome://flags/#molt-voice-mode on): until the browser can hear,
// what the user would say is typed here, and runs in the panel's window as
// a spoken one would (molt_ai/voice/voice_controller.h). Also how the build
// machine tries the touchless scenario by hand.
//
// Messages (arguments checked with HasCallbackArgs()/StringArg(), never
// CHECKed):
//   voiceStatus(cb)        -> {enabled, moltnet}
//   voiceCommand(cb, text) -> {ok} or {ok: false, error}; at most
//                             kMaxVoiceCommandBytes of text.
// Events: 'voice-caption' {state, heard, status} and 'voice-speech'
// {text}: what the browser heard, does and says, shown as text.

#ifndef CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_AGENT_VOICE_HANDLER_H_
#define CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_AGENT_VOICE_HANDLER_H_

#include <cstddef>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/voice/voice_controller.h"
#include "content/public/browser/web_ui_message_handler.h"

class BrowserWindowInterface;

class MoltAIAgentVoiceHandler
    : public content::WebUIMessageHandler,
      public molt_ai::voice::VoiceController::Observer {
 public:
  // The grammar's own limit (voice_command.h kMaxUtteranceBytes).
  static constexpr size_t kMaxVoiceCommandBytes = 1000;

  MoltAIAgentVoiceHandler();
  MoltAIAgentVoiceHandler(const MoltAIAgentVoiceHandler&) = delete;
  MoltAIAgentVoiceHandler& operator=(const MoltAIAgentVoiceHandler&) = delete;
  ~MoltAIAgentVoiceHandler() override;

  // content::WebUIMessageHandler:
  void RegisterMessages() override;
  void OnJavascriptDisallowed() override;

  // molt_ai::voice::VoiceController::Observer:
  void OnVoiceCaption(molt_ai::voice::VoiceActivity activity,
                      const std::string& heard,
                      const std::string& status) override;
  void OnVoiceSpeech(const std::string& text) override;
  void OnVoiceControllerDestroying(
      molt_ai::voice::VoiceController* controller) override;

 private:
  void HandleVoiceStatus(const base::ListValue& args);
  void HandleVoiceCommand(const base::ListValue& args);

  // The window the panel works for: the profile's last active one, as the
  // studio's other actions use.
  BrowserWindowInterface* Window();
  void Observe(molt_ai::voice::VoiceController* controller);

  raw_ptr<molt_ai::voice::VoiceController> observed_ = nullptr;
};

#endif  // CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_AGENT_VOICE_HANDLER_H_
