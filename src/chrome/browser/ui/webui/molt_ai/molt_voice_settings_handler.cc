// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/webui/molt_ai/molt_voice_settings_handler.h"

#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/thread_pool.h"
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

namespace {

using molt_ai::chat_handler_helpers::BoolArg;
using molt_ai::chat_handler_helpers::HasCallbackArgs;
using molt_ai::voice::VoiceModeSettings;
using molt_ai::voice::VoiceSession;

constexpr char kClickToChange[] = "Click the setting to change it.";

constexpr base::TaskTraits kFileTraits = {
    base::MayBlock(), base::TaskPriority::USER_VISIBLE,
    base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN};

// Read, change one setting, write: on a worker. The settings as written,
// and whether they were.
std::pair<VoiceModeSettings, bool> ReadChangeWrite(bool enabled_changed,
                                                   bool value) {
  VoiceModeSettings settings = molt_ai::voice::ReadVoiceModeSettings();
  if (enabled_changed) {
    settings.start_with_browser = value;
  } else {
    settings.always_listening = value;
  }
  const bool ok = molt_ai::voice::WriteVoiceModeSettings(settings);
  return {settings, ok};
}

}  // namespace

MoltVoiceSettingsHandler::MoltVoiceSettingsHandler() = default;
MoltVoiceSettingsHandler::~MoltVoiceSettingsHandler() = default;

void MoltVoiceSettingsHandler::RegisterMessages() {
  web_ui()->RegisterMessageCallback(
      "voiceSettingsGetState",
      base::BindRepeating(&MoltVoiceSettingsHandler::HandleGetState,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "voiceSettingsSetEnabled",
      base::BindRepeating(&MoltVoiceSettingsHandler::HandleSetEnabled,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "voiceSettingsSetAlwaysListening",
      base::BindRepeating(&MoltVoiceSettingsHandler::HandleSetAlwaysListening,
                          base::Unretained(this)));
}

void MoltVoiceSettingsHandler::OnJavascriptDisallowed() {
  weak_factory_.InvalidateWeakPtrs();
}

BrowserWindowInterface* MoltVoiceSettingsHandler::Window() {
  tabs::TabInterface* tab =
      tabs::TabInterface::MaybeGetFromContents(web_ui()->GetWebContents());
  return tab ? tab->GetBrowserWindowInterface() : nullptr;
}

void MoltVoiceSettingsHandler::ResolveWithState(
    std::string callback_id,
    std::optional<std::string> error) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, kFileTraits,
      base::BindOnce(&molt_ai::voice::ReadVoiceModeSettings),
      base::BindOnce(&MoltVoiceSettingsHandler::OnRead,
                     weak_factory_.GetWeakPtr(), std::move(callback_id),
                     std::move(error)));
}

void MoltVoiceSettingsHandler::OnRead(std::string callback_id,
                                      std::optional<std::string> error,
                                      const VoiceModeSettings& settings) {
  if (!IsJavascriptAllowed()) {
    return;
  }
  BrowserWindowInterface* window = Window();
  VoiceSession* session = VoiceSession::FromBrowser(window);
  base::DictValue state;
  state.Set("available", molt_ai::voice::VoiceModeEnabled());
  state.Set("enabled", settings.start_with_browser);
  state.Set("alwaysListening", settings.always_listening);
  state.Set("listening", session && session->listening());
  state.Set("moltnet",
            molt_ai::tor::IsMoltNetProfile(Profile::FromWebUI(web_ui())));
  if (error) {
    state.Set("error", *error);
  }
  ResolveJavascriptCallback(base::Value(std::move(callback_id)),
                            base::Value(std::move(state)));
}

void MoltVoiceSettingsHandler::HandleGetState(const base::ListValue& args) {
  if (!HasCallbackArgs(args, 1u, __func__)) {
    return;
  }
  AllowJavascript();
  ResolveWithState(args[0].GetString(), std::nullopt);
}

void MoltVoiceSettingsHandler::HandleSetEnabled(const base::ListValue& args) {
  if (!HasCallbackArgs(args, 2u, __func__)) {
    return;
  }
  AllowJavascript();
  const std::optional<bool> enabled = BoolArg(args, 1);
  if (!enabled || !molt_ai::voice::VoiceModeEnabled()) {
    ResolveWithState(args[0].GetString(), std::nullopt);
    return;
  }
  // The microphone: only with the click itself (molt_user_click.h). OS
  // voice control's clicks count; a page's script can't make one up.
  if (!molt_ai::ConsumeUserClick(true, web_ui()->GetRenderFrameHost())) {
    ResolveWithState(args[0].GetString(), kClickToChange);
    return;
  }
  Change(args[0].GetString(), /*enabled_changed=*/true, *enabled);
}

void MoltVoiceSettingsHandler::HandleSetAlwaysListening(
    const base::ListValue& args) {
  if (!HasCallbackArgs(args, 2u, __func__)) {
    return;
  }
  AllowJavascript();
  const std::optional<bool> always = BoolArg(args, 1);
  if (!always || !molt_ai::voice::VoiceModeEnabled()) {
    ResolveWithState(args[0].GetString(), std::nullopt);
    return;
  }
  if (!molt_ai::ConsumeUserClick(true, web_ui()->GetRenderFrameHost())) {
    ResolveWithState(args[0].GetString(), kClickToChange);
    return;
  }
  Change(args[0].GetString(), /*enabled_changed=*/false, *always);
}

void MoltVoiceSettingsHandler::Change(std::string callback_id,
                                      bool enabled_changed,
                                      bool value) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, kFileTraits,
      base::BindOnce(&ReadChangeWrite, enabled_changed, value),
      base::BindOnce(
          [](base::WeakPtr<MoltVoiceSettingsHandler> self,
             std::string callback_id, bool enabled_changed,
             std::pair<VoiceModeSettings, bool> result) {
            if (self) {
              self->OnWritten(std::move(callback_id), enabled_changed,
                              result.first, result.second);
            }
          },
          weak_factory_.GetWeakPtr(), std::move(callback_id), enabled_changed));
}

void MoltVoiceSettingsHandler::OnWritten(std::string callback_id,
                                         bool enabled_changed,
                                         const VoiceModeSettings& settings,
                                         bool ok) {
  if (!IsJavascriptAllowed()) {
    return;
  }
  if (!ok) {
    ResolveWithState(std::move(callback_id),
                     "Couldn't save ~/.moltbrowser/settings.json (is it valid "
                     "JSON?). Nothing was changed.");
    return;
  }
  VoiceSession::OnSettingsChanged(settings);
  if (enabled_changed) {
    // Now too: listening in this page's window, or off wherever it is.
    if (settings.start_with_browser) {
      if (VoiceSession* session =
              VoiceSession::GetOrCreateForBrowser(Window())) {
        session->Start();
      }
    } else if (VoiceSession* listening = VoiceSession::Listening()) {
      listening->Stop();
    }
  }
  ResolveWithState(std::move(callback_id), std::nullopt);
}

// Embedded in app.js after the page's own script.
const char kVoiceSettingsJs[] = R"JS(
// ---- Voice mode ----
(function() {
  'use strict';
  var section = document.getElementById('voiceSection');
  if (!section) return;

  function render(s) {
    if (!s || typeof s !== 'object') return;
    section.hidden = s.available !== true;
    document.getElementById('voiceEnabled').checked = s.enabled === true;
    document.getElementById('voiceAlwaysListening').checked =
        s.alwaysListening === true;
    var said = '';
    if (s.error) {
      said = String(s.error);
    } else if (s.moltnet === true) {
      said = 'Voice mode never listens in MoltNet windows.';
    } else if (s.listening === true) {
      said = 'Listening in this window.';
    }
    var status = document.getElementById('voiceStatus');
    if (status.textContent !== said) status.textContent = said;
  }

  function call() {
    return sendWithPromise.apply(null, arguments).then(render)
        .catch(function() { showToast('Couldn\'t update voice mode'); });
  }

  moltActions({
    voiceSetEnabled: function() {
      call('voiceSettingsSetEnabled', !!this.checked);
    },
    voiceSetAlwaysListening: function() {
      call('voiceSettingsSetAlwaysListening', !!this.checked);
    }
  });

  call('voiceSettingsGetState');
})();
)JS";
