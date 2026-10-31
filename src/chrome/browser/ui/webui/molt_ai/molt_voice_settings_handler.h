// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// "Voice mode" on molt://ai-settings (id="voiceSection", shown only with
// chrome://flags/#molt-voice-mode): the switch that turns listening on now
// in this page's window and with the browser from then on, always-listening
// mode, and saying the voice gesture's confirmation word aloud even when
// another program could hear it (molt_ai/voice/voice_session_settings.h,
// voice_session.h).
//
// Messages (validated with HasCallbackArgs() and BoolArg(); nothing from
// the page reaches a CHECK; the two that change a setting count only with
// the click itself, molt_user_click.h):
//   voiceSettingsGetState(cb)
//   voiceSettingsSetEnabled(cb, bool)
//   voiceSettingsSetAlwaysListening(cb, bool)
//   voiceSettingsSetSayWordAloud(cb, bool)
// each resolving with {available, enabled, alwaysListening, sayWordAloud,
// listening, moltnet, error?}.
//
// The section's script is kVoiceSettingsJs, appended to the page's app.js;
// it builds no HTML and wires its switches through moltActions().

#ifndef CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_VOICE_SETTINGS_HANDLER_H_
#define CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_VOICE_SETTINGS_HANDLER_H_

#include <optional>
#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/voice/voice_session_settings.h"
#include "content/public/browser/web_ui_message_handler.h"

class BrowserWindowInterface;

class MoltVoiceSettingsHandler : public content::WebUIMessageHandler {
 public:
  // Which setting a message changes.
  enum class Setting { kEnabled, kAlwaysListening, kSayWordAloud };

  MoltVoiceSettingsHandler();
  MoltVoiceSettingsHandler(const MoltVoiceSettingsHandler&) = delete;
  MoltVoiceSettingsHandler& operator=(const MoltVoiceSettingsHandler&) = delete;
  ~MoltVoiceSettingsHandler() override;

  // content::WebUIMessageHandler:
  void RegisterMessages() override;
  void OnJavascriptDisallowed() override;

 private:
  void HandleGetState(const base::ListValue& args);
  void HandleSetEnabled(const base::ListValue& args);
  void HandleSetAlwaysListening(const base::ListValue& args);
  void HandleSetSayWordAloud(const base::ListValue& args);

  // Reads the settings (file I/O, on a worker), then resolves.
  void ResolveWithState(std::string callback_id,
                        std::optional<std::string> error);
  void OnRead(std::string callback_id,
              std::optional<std::string> error,
              const molt_ai::voice::VoiceModeSettings& settings);
  // A setting's message: validated, counted only with the click itself.
  void HandleSet(const base::ListValue& args, Setting setting);
  // Changes the settings: read, |setting| := |value|, write; then applies
  // them.
  void Change(std::string callback_id, Setting setting, bool value);
  void OnWritten(std::string callback_id,
                 Setting setting,
                 const molt_ai::voice::VoiceModeSettings& settings,
                 bool ok);
  // The window this page is a tab of, or null.
  BrowserWindowInterface* Window();

  base::WeakPtrFactory<MoltVoiceSettingsHandler> weak_factory_{this};
};

// The section's script, appended to molt://ai-settings' app.js after the
// page's own (it uses sendWithPromise(), showToast() and moltActions()).
extern const char kVoiceSettingsJs[];

#endif  // CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_VOICE_SETTINGS_HANDLER_H_
