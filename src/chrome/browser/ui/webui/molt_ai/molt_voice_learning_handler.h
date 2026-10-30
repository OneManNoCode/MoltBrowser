// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// "What voice mode learned" on molt://ai-settings (id="voiceLearningSection",
// shown with voice mode's section, chrome://flags/#molt-voice-mode):
// everything voice mode learned in this profile (docs/design/
// TOUCHLESS_BROWSING.md §6.8; molt_ai/voice/voice_learning.h), each with a
// delete control, "Forget everything", and the "Learn from what I say"
// switch.
//
// Messages (validated with HasCallbackArgs(), BoolArg() and StringArg();
// nothing from the page reaches a CHECK; turning learning on or off and
// forgetting everything count only with the click itself, molt_user_click.h):
//   voiceLearningGetState(cb)
//   voiceLearningSetEnabled(cb, bool)
//   voiceLearningForget(cb, kind, key): kind "routine" (key: its id),
//       "word" (the phrase), "correction" (what was heard) or "hint" (the
//       host)
//   voiceLearningForgetAll(cb)
// each resolving with {available, moltnet, enabled, routines: [{id, name,
// steps, draft}], words: [{phrase, uses}], corrections: [{heard, meant}],
// hints: [{host, search, list, player}], error?}. Everything in it is the
// user's words or page names: the page shows it as text only.
//
// The section's script is kVoiceLearningSettingsJs, appended to the page's
// app.js; it builds its rows with DOM APIs and textContent, and wires its
// controls through moltActions().

#ifndef CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_VOICE_LEARNING_HANDLER_H_
#define CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_VOICE_LEARNING_HANDLER_H_

#include <optional>
#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "content/public/browser/web_ui_message_handler.h"

namespace molt_ai {
namespace voice {
class VoiceLearning;
}  // namespace voice
}  // namespace molt_ai

class MoltVoiceLearningHandler : public content::WebUIMessageHandler {
 public:
  MoltVoiceLearningHandler();
  MoltVoiceLearningHandler(const MoltVoiceLearningHandler&) = delete;
  MoltVoiceLearningHandler& operator=(const MoltVoiceLearningHandler&) =
      delete;
  ~MoltVoiceLearningHandler() override;

  // content::WebUIMessageHandler:
  void RegisterMessages() override;
  void OnJavascriptDisallowed() override;

 private:
  void HandleGetState(const base::ListValue& args);
  void HandleSetEnabled(const base::ListValue& args);
  void HandleForget(const base::ListValue& args);
  void HandleForgetAll(const base::ListValue& args);

  // The profile's learning (null off the record, MoltNet's included).
  molt_ai::voice::VoiceLearning* Learning();
  // Resolves |callback_id| with the state, once the learning has loaded.
  void Resolve(std::string callback_id, std::optional<std::string> error);
  void ResolveNow(std::string callback_id, std::optional<std::string> error);

  base::WeakPtrFactory<MoltVoiceLearningHandler> weak_factory_{this};
};

// The section's script, appended to molt://ai-settings' app.js after the
// page's own (it uses sendWithPromise(), showToast() and moltActions()).
extern const char kVoiceLearningSettingsJs[];

#endif  // CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_VOICE_LEARNING_HANDLER_H_
