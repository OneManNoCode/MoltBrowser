// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/webui/molt_ai/molt_ai_agent_voice_handler.h"

#include <string_view>
#include <utility>

#include "base/functional/bind.h"
#include "chrome/browser/molt_ai/tor/molt_net_profile_id.h"
#include "chrome/browser/molt_ai/voice/voice_controller_features.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/profile_browser_collection.h"
#include "chrome/browser/ui/webui/molt_ai/molt_ai_chat_handler_helpers.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"

namespace {

using molt_ai::voice::VoiceActivity;
using molt_ai::voice::VoiceController;

std::string_view ActivityName(VoiceActivity activity) {
  switch (activity) {
    case VoiceActivity::kOff:
      return "off";
    case VoiceActivity::kListening:
      return "listening";
    case VoiceActivity::kThinking:
      return "thinking";
    case VoiceActivity::kSpeaking:
      return "speaking";
  }
  return "off";
}

base::DictValue Result(bool ok, std::string_view error = {}) {
  base::DictValue out;
  out.Set("ok", ok);
  if (!error.empty()) {
    out.Set("error", error);
  }
  return out;
}

}  // namespace

MoltAIAgentVoiceHandler::MoltAIAgentVoiceHandler() = default;

MoltAIAgentVoiceHandler::~MoltAIAgentVoiceHandler() {
  Observe(nullptr);
}

void MoltAIAgentVoiceHandler::RegisterMessages() {
  web_ui()->RegisterMessageCallback(
      "voiceStatus",
      base::BindRepeating(&MoltAIAgentVoiceHandler::HandleVoiceStatus,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "voiceCommand",
      base::BindRepeating(&MoltAIAgentVoiceHandler::HandleVoiceCommand,
                          base::Unretained(this)));
}

void MoltAIAgentVoiceHandler::OnJavascriptDisallowed() {
  Observe(nullptr);
}

BrowserWindowInterface* MoltAIAgentVoiceHandler::Window() {
  Profile* profile = Profile::FromBrowserContext(
      web_ui()->GetWebContents()->GetBrowserContext());
  ProfileBrowserCollection* browsers =
      profile ? ProfileBrowserCollection::GetForProfile(profile) : nullptr;
  return browsers ? browsers->GetLastActiveBrowser() : nullptr;
}

void MoltAIAgentVoiceHandler::Observe(VoiceController* controller) {
  if (observed_ == controller) {
    return;
  }
  if (observed_) {
    observed_->RemoveObserver(this);
  }
  observed_ = controller;
  if (observed_) {
    observed_->AddObserver(this);
  }
}

void MoltAIAgentVoiceHandler::HandleVoiceStatus(const base::ListValue& args) {
  if (!HasCallbackArgs(args, 1u, __func__)) {
    return;
  }
  AllowJavascript();
  Profile* profile = Profile::FromBrowserContext(
      web_ui()->GetWebContents()->GetBrowserContext());
  base::DictValue out;
  out.Set("enabled", molt_ai::voice::VoiceModeEnabled());
  out.Set("moltnet", molt_ai::tor::IsMoltNetProfile(profile));
  ResolveJavascriptCallback(args[0], base::Value(std::move(out)));
}

void MoltAIAgentVoiceHandler::HandleVoiceCommand(const base::ListValue& args) {
  if (!HasCallbackArgs(args, 2u, __func__)) {
    return;
  }
  AllowJavascript();
  const std::string text = StringArg(args, 1);
  if (!molt_ai::voice::VoiceModeEnabled()) {
    ResolveJavascriptCallback(
        args[0], base::Value(Result(
                     false, "Voice mode is off: turn on "
                            "chrome://flags/#molt-voice-mode first.")));
    return;
  }
  if (text.empty()) {
    ResolveJavascriptCallback(
        args[0], base::Value(Result(false, "Type a command first.")));
    return;
  }
  if (text.size() > kMaxVoiceCommandBytes) {
    ResolveJavascriptCallback(
        args[0],
        base::Value(Result(false, "That's too long. Say it in fewer words.")));
    return;
  }
  VoiceController* controller =
      VoiceController::GetOrCreateForBrowser(Window());
  if (!controller) {
    ResolveJavascriptCallback(
        args[0], base::Value(Result(false, "There's no browser window.")));
    return;
  }
  Observe(controller);
  ResolveJavascriptCallback(args[0], base::Value(Result(true)));
  controller->HandleUtterance(text);
}

void MoltAIAgentVoiceHandler::OnVoiceCaption(VoiceActivity activity,
                                             const std::string& heard,
                                             const std::string& status) {
  if (!IsJavascriptAllowed()) {
    return;
  }
  base::DictValue caption;
  caption.Set("state", ActivityName(activity));
  caption.Set("heard", heard);
  caption.Set("status", status);
  FireWebUIListener("voice-caption", base::Value(std::move(caption)));
}

void MoltAIAgentVoiceHandler::OnVoiceSpeech(const std::string& text) {
  if (!IsJavascriptAllowed()) {
    return;
  }
  base::DictValue speech;
  speech.Set("text", text);
  FireWebUIListener("voice-speech", base::Value(std::move(speech)));
}

void MoltAIAgentVoiceHandler::OnVoiceControllerDestroying(
    VoiceController* controller) {
  if (observed_ == controller) {
    observed_->RemoveObserver(this);
    observed_ = nullptr;
  }
}
