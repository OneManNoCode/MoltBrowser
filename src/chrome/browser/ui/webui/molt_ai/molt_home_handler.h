// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The home page's microphone (chrome://molt-home): "Which site would you
// like me to open?". Its messages:
//   - "getVoice" [callbackId]: whether voice mode can start in this page's
//     window ({available, listening}): not while #molt-voice-mode is off, nor
//     in a MoltNet window.
//   - "startVoice" [callbackId]: with the click itself (molt_user_click.h),
//     starts voice mode in this page's window and asks which site to open
//     (VoiceSession::StartAndAskWhichSite()); resolves with the new state.
// The microphone opens only from a real click (or OS voice control's): a
// page can't send these, and the home page's script can't make one up.

#ifndef CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_HOME_HANDLER_H_
#define CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_HOME_HANDLER_H_

#include "base/values.h"
#include "content/public/browser/web_ui_message_handler.h"

class BrowserWindowInterface;

class MoltHomeHandler : public content::WebUIMessageHandler {
 public:
  MoltHomeHandler();
  MoltHomeHandler(const MoltHomeHandler&) = delete;
  MoltHomeHandler& operator=(const MoltHomeHandler&) = delete;
  ~MoltHomeHandler() override;

  // content::WebUIMessageHandler:
  void RegisterMessages() override;

 private:
  void HandleGetVoice(const base::ListValue& args);
  void HandleStartVoice(const base::ListValue& args);
  // The window this page is in, if any (none while it is a prerender).
  BrowserWindowInterface* Window();
  base::DictValue VoiceState(bool started);
};

#endif  // CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_HOME_HANDLER_H_
