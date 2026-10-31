// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/webui/molt_ai/molt_voice_settings_handler.h"

#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/molt_ai/common/molt_settings_file.h"
#include "chrome/browser/molt_ai/common/molt_user_click.h"
#include "chrome/browser/molt_ai/tor/molt_net_profile_id.h"
#include "chrome/browser/molt_ai/voice/voice_controller.h"
#include "chrome/browser/molt_ai/voice/voice_controller_features.h"
#include "chrome/browser/molt_ai/voice/voice_language.h"
#include "chrome/browser/molt_ai/voice/voice_language_settings.h"
#include "chrome/browser/molt_ai/voice/voice_session.h"
#include "chrome/browser/molt_ai/voice/voice_speech.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/webui/molt_ai/molt_ai_chat_handler_helpers.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"

namespace {

using molt_ai::chat_handler_helpers::BoolArg;
using molt_ai::chat_handler_helpers::BoundedStringArg;
using molt_ai::chat_handler_helpers::HasCallbackArgs;
using molt_ai::voice::VoiceModeSettings;
using molt_ai::voice::VoiceSession;

constexpr char kClickToChange[] = "Click the setting to change it.";

// Read, change one setting, write: on settings.json's own sequence
// (common/molt_settings_file.h), so no other change comes in between. The
// settings as written, and whether they were.
std::pair<VoiceModeSettings, bool> ReadChangeWrite(
    MoltVoiceSettingsHandler::Setting which,
    bool value) {
  VoiceModeSettings settings = molt_ai::voice::ReadVoiceModeSettings();
  switch (which) {
    case MoltVoiceSettingsHandler::Setting::kEnabled:
      settings.start_with_browser = value;
      break;
    case MoltVoiceSettingsHandler::Setting::kAlwaysListening:
      settings.always_listening = value;
      break;
    case MoltVoiceSettingsHandler::Setting::kSayWordAloud:
      settings.say_word_aloud = value;
      break;
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
  web_ui()->RegisterMessageCallback(
      "voiceSettingsSetSayWordAloud",
      base::BindRepeating(&MoltVoiceSettingsHandler::HandleSetSayWordAloud,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "voiceSettingsSetLanguage",
      base::BindRepeating(&MoltVoiceSettingsHandler::HandleSetLanguage,
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
  molt_ai::MoltSettingsFileTaskRunner()->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&molt_ai::voice::ReadVoiceModeSettings),
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
  // Then the language (the same sequence: it sees every change before).
  molt_ai::MoltSettingsFileTaskRunner()->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&molt_ai::voice::ReadVoiceLanguageSetting),
      base::BindOnce(&MoltVoiceSettingsHandler::OnLanguageRead,
                     weak_factory_.GetWeakPtr(), std::move(callback_id),
                     std::move(error), settings));
}

void MoltVoiceSettingsHandler::OnLanguageRead(std::string callback_id,
                                              std::optional<std::string> error,
                                              VoiceModeSettings settings,
                                              std::string language) {
  if (!IsJavascriptAllowed()) {
    return;
  }
  const molt_ai::voice::Language in_use = molt_ai::voice::VoiceLanguageFor(
      language, g_browser_process->GetApplicationLocale());
  BrowserWindowInterface* window = Window();
  VoiceSession* session = VoiceSession::FromBrowser(window);
  base::DictValue state;
  state.Set("available", molt_ai::voice::VoiceModeEnabled());
  state.Set("enabled", settings.start_with_browser);
  state.Set("alwaysListening", settings.always_listening);
  state.Set("sayWordAloud", settings.say_word_aloud);
  state.Set("listening", session && session->listening());
  state.Set("moltnet",
            molt_ai::tor::IsMoltNetProfile(Profile::FromWebUI(web_ui())));
  state.Set("language", language);
  state.Set("languageInUse", molt_ai::voice::LanguageCode(in_use));
  state.Set("canListen", molt_ai::voice::ListeningAvailable(in_use));
  state.Set("hasVoice", molt_ai::voice::VoiceSpeech::HasPlatformVoice(
                            Profile::FromWebUI(web_ui()), in_use));
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
  HandleSet(args, Setting::kEnabled);
}

void MoltVoiceSettingsHandler::HandleSetAlwaysListening(
    const base::ListValue& args) {
  HandleSet(args, Setting::kAlwaysListening);
}

void MoltVoiceSettingsHandler::HandleSetSayWordAloud(
    const base::ListValue& args) {
  HandleSet(args, Setting::kSayWordAloud);
}

void MoltVoiceSettingsHandler::HandleSet(const base::ListValue& args,
                                         Setting setting) {
  if (!HasCallbackArgs(args, 2u, __func__)) {
    return;
  }
  AllowJavascript();
  const std::optional<bool> value = BoolArg(args, 1);
  if (!value || !molt_ai::voice::VoiceModeEnabled()) {
    ResolveWithState(args[0].GetString(), std::nullopt);
    return;
  }
  // Only with the click itself (molt_user_click.h). OS voice control's
  // clicks count; a page's script can't make one up, and neither can
  // voice mode (nothing it hears changes these).
  if (!molt_ai::ConsumeUserClick(true, web_ui()->GetRenderFrameHost())) {
    ResolveWithState(args[0].GetString(), kClickToChange);
    return;
  }
  Change(args[0].GetString(), setting, *value);
}

void MoltVoiceSettingsHandler::HandleSetLanguage(const base::ListValue& args) {
  if (!HasCallbackArgs(args, 2u, __func__)) {
    return;
  }
  AllowJavascript();
  // "", "en" or "es"; anything else (not a string, longer, another
  // language) changes nothing.
  const std::optional<std::string> language = BoundedStringArg(args, 1, 2);
  if (!molt_ai::voice::VoiceModeEnabled() || !language ||
      (!language->empty() && !molt_ai::voice::StoredVoiceLanguage(*language))) {
    ResolveWithState(args[0].GetString(), std::nullopt);
    return;
  }
  molt_ai::MoltSettingsFileTaskRunner()->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&molt_ai::voice::WriteVoiceLanguageSetting, *language),
      base::BindOnce(&MoltVoiceSettingsHandler::OnLanguageWritten,
                     weak_factory_.GetWeakPtr(), args[0].GetString(),
                     *language));
}

void MoltVoiceSettingsHandler::OnLanguageWritten(std::string callback_id,
                                                 std::string language,
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
  molt_ai::voice::VoiceController::SetLanguageForAll(
      molt_ai::voice::VoiceLanguageFor(
          language, g_browser_process->GetApplicationLocale()));
  ResolveWithState(std::move(callback_id), std::nullopt);
}

void MoltVoiceSettingsHandler::Change(std::string callback_id,
                                      Setting setting,
                                      bool value) {
  molt_ai::MoltSettingsFileTaskRunner()->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&ReadChangeWrite, setting, value),
      base::BindOnce(
          [](base::WeakPtr<MoltVoiceSettingsHandler> self,
             std::string callback_id, Setting setting,
             std::pair<VoiceModeSettings, bool> result) {
            if (self) {
              self->OnWritten(std::move(callback_id), setting, result.first,
                              result.second);
            }
          },
          weak_factory_.GetWeakPtr(), std::move(callback_id), setting));
}

void MoltVoiceSettingsHandler::OnWritten(std::string callback_id,
                                         Setting setting,
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
  if (setting == Setting::kEnabled) {
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
    document.getElementById('voiceSayWordAloud').checked =
        s.sayWordAloud === true;
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

    var language = document.getElementById('voiceLanguage');
    var chosen = typeof s.language === 'string' ? s.language : '';
    if (language.value !== chosen) language.value = chosen;
    var note = '';
    if (s.canListen === false) {
      note = 'Voice mode can\'t listen in this language yet: no speech ' +
          'model for it is installed. Commands typed in Agent mode work, ' +
          'and the browser answers in it.';
    }
    if (s.hasVoice === false) {
      note += (note ? ' ' : '') + 'This computer has no voice for this ' +
          'language: add one in the system\'s speech settings, or the ' +
          'default voice reads the answers.';
    }
    var languageNote = document.getElementById('voiceLanguageNote');
    if (languageNote.textContent !== note) languageNote.textContent = note;
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
    },
    voiceSetSayWordAloud: function() {
      call('voiceSettingsSetSayWordAloud', !!this.checked);
    },
    voiceSetLanguage: function() {
      var value = String(this.value);
      if (value !== '' && value !== 'en' && value !== 'es') return;
      call('voiceSettingsSetLanguage', value);
    }
  });

  call('voiceSettingsGetState');
})();
)JS";
