// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltShield section of molt://ai-settings: global switches, filter lists
// (status, versions, enable, "update now"), session counters, open tabs with
// their blocked counts + per-site Shields toggle + "Report broken site", and
// the list of sites with Shields down.
//
// The section's markup lives in molt_ai_settings_ui.cc (id="shieldSection");
// its script is kMoltShieldSettingsJs below, appended to the page's app.js.
// It builds DOM with createElement/textContent only (no HTML sinks), so it
// needs nothing from the page's 'molt-html' Trusted Types policy.

#ifndef CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_SETTINGS_HANDLER_H_
#define CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_SETTINGS_HANDLER_H_

#include "base/values.h"
#include "content/public/browser/web_ui_message_handler.h"

namespace molt_ai::shield {

class MoltShieldSettingsHandler : public content::WebUIMessageHandler {
 public:
  MoltShieldSettingsHandler();
  MoltShieldSettingsHandler(const MoltShieldSettingsHandler&) = delete;
  MoltShieldSettingsHandler& operator=(const MoltShieldSettingsHandler&) =
      delete;
  ~MoltShieldSettingsHandler() override;

  // content::WebUIMessageHandler:
  void RegisterMessages() override;

 private:
  // All take [callback_id, ...] and resolve with the section state (the
  // report handler resolves with {report}).
  void HandleGetState(const base::ListValue& args);
  void HandleSetGlobal(const base::ListValue& args);       // key, bool
  void HandleSetListEnabled(const base::ListValue& args);  // id, bool
  void HandleUpdateNow(const base::ListValue& args);
  void HandleSetTabShields(const base::ListValue& args);   // tab key, bool
  void HandleRemoveException(const base::ListValue& args);  // pattern
  void HandleReportBreakage(const base::ListValue& args);  // tab key

  base::DictValue BuildState();
  void ResolveWithState(const base::Value& callback_id);
};

// Script for the section, appended to molt://ai-settings' app.js. Uses the
// page's sendWithPromise() and showToast().
extern const char kMoltShieldSettingsJs[];

}  // namespace molt_ai::shield

#endif  // CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_SETTINGS_HANDLER_H_
