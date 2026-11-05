// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/webui/molt_ai/molt_home_handler.h"

#include <string>

#include "base/functional/bind.h"
#include "chrome/browser/molt_ai/common/molt_user_click.h"
#include "chrome/browser/molt_ai/tor/molt_net_profile_id.h"
#include "chrome/browser/molt_ai/voice/voice_controller_features.h"
#include "chrome/browser/molt_ai/voice/voice_session.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/webui/molt_ai/molt_ai_chat_handler_helpers.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"

using molt_ai::chat_handler_helpers::HasCallbackArgs;
using molt_ai::voice::VoiceSession;

MoltHomeHandler::MoltHomeHandler() = default;
MoltHomeHandler::~MoltHomeHandler() = default;

void MoltHomeHandler::RegisterMessages() {
  web_ui()->RegisterMessageCallback(
      "getVoice", base::BindRepeating(&MoltHomeHandler::HandleGetVoice,
                                      base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "startVoice", base::BindRepeating(&MoltHomeHandler::HandleStartVoice,
                                        base::Unretained(this)));
}

BrowserWindowInterface* MoltHomeHandler::Window() {
  tabs::TabInterface* tab =
      tabs::TabInterface::MaybeGetFromContents(web_ui()->GetWebContents());
  return tab ? tab->GetBrowserWindowInterface() : nullptr;
}

base::DictValue MoltHomeHandler::VoiceState(bool started) {
  BrowserWindowInterface* window = Window();
  const bool moltnet =
      molt_ai::tor::IsMoltNetProfile(Profile::FromWebUI(web_ui()));
  VoiceSession* session =
      window && !moltnet ? VoiceSession::FromBrowser(window) : nullptr;
  base::DictValue state;
  state.Set("available",
            molt_ai::voice::VoiceModeEnabled() && window && !moltnet);
  state.Set("listening", session && session->listening());
  state.Set("started", started);
  return state;
}

void MoltHomeHandler::HandleGetVoice(const base::ListValue& args) {
  if (!HasCallbackArgs(args, 1u, __func__)) {
    return;
  }
  AllowJavascript();
  ResolveJavascriptCallback(args[0], base::Value(VoiceState(false)));
}

void MoltHomeHandler::HandleStartVoice(const base::ListValue& args) {
  if (!HasCallbackArgs(args, 1u, __func__)) {
    return;
  }
  AllowJavascript();
  bool started = false;
  BrowserWindowInterface* window = Window();
  // Only with the click itself: the microphone never opens otherwise.
  const bool available =
      molt_ai::voice::VoiceModeEnabled() && window &&
      !molt_ai::tor::IsMoltNetProfile(Profile::FromWebUI(web_ui()));
  if (molt_ai::ConsumeUserClick(available, web_ui()->GetRenderFrameHost())) {
    if (VoiceSession* session = VoiceSession::GetOrCreateForBrowser(window)) {
      started = session->StartAndAskWhichSite();
    }
  }
  ResolveJavascriptCallback(args[0], base::Value(VoiceState(started)));
}
