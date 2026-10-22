// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// "Sites using on-device AI" on molt://ai-settings (id="webAiSection"): the
// sites the user allowed or blocked for the built-in AI APIs (LanguageModel,
// Summarizer, Writer, Rewriter) and those whose prompt is paused, with
// Remove and Remove all, plus the switch for the APIs ("web_ai_apis" in
// settings.json, web_ai_settings_switch.h).
//
// The list is the regular profile's (WebAISitePermissions); an Incognito or
// MoltNet window's page shows that one too, since Incognito keeps its own
// answers only in memory until it closes. A Guest session shows its own.
//
// Messages (every one validated with HasCallbackArgs() and the typed
// argument helpers; nothing from the page reaches a CHECK):
//   webAiSitesGetState(cb)
//   webAiSitesRemove(cb, site)      site: a key the state listed
//   webAiSitesClear(cb)
//   webAiSitesSetEnabled(cb, bool)
// each resolving with the section state:
//   {enabled, persistent, available,
//    sites: [{site, display, kind: "allowed"|"blocked"|"paused", since,
//             until}], error?}
// and "web-ai-sites-changed" fires when the answers change elsewhere (a
// prompt in a tab, Clear browsing data).
//
// The section's script is kWebAISitesSettingsJs, appended to the page's
// app.js; it builds DOM with createElement/textContent only and wires its
// buttons through moltActions().

#ifndef CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_WEB_AI_SITES_HANDLER_H_
#define CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_WEB_AI_SITES_HANDLER_H_

#include <optional>
#include <string>

#include "base/callback_list.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "content/public/browser/web_ui_message_handler.h"

namespace molt_ai::web_ai {
class WebAISitePermissions;
}  // namespace molt_ai::web_ai

class MoltWebAISitesHandler : public content::WebUIMessageHandler {
 public:
  MoltWebAISitesHandler();
  MoltWebAISitesHandler(const MoltWebAISitesHandler&) = delete;
  MoltWebAISitesHandler& operator=(const MoltWebAISitesHandler&) = delete;
  ~MoltWebAISitesHandler() override;

  // content::WebUIMessageHandler:
  void RegisterMessages() override;
  void OnJavascriptAllowed() override;
  void OnJavascriptDisallowed() override;

 private:
  void HandleGetState(const base::ListValue& args);
  void HandleRemove(const base::ListValue& args);
  void HandleClear(const base::ListValue& args);
  void HandleSetEnabled(const base::ListValue& args);

  // Reads the switch (file I/O, on a worker), then resolves |callback_id|
  // with the state; |error| is shown in the section.
  void ResolveWithState(std::string callback_id,
                        std::optional<std::string> error);
  void OnSwitchRead(std::string callback_id,
                    std::optional<std::string> error,
                    bool enabled);
  void OnSwitchWritten(std::string callback_id, bool ok);
  base::DictValue BuildState(bool enabled);
  molt_ai::web_ai::WebAISitePermissions* Permissions();
  void OnSitesChanged();

  base::CallbackListSubscription sites_subscription_;
  base::WeakPtrFactory<MoltWebAISitesHandler> weak_factory_{this};
};

// The section's script, appended to molt://ai-settings' app.js after the
// page's own (it uses sendWithPromise(), showToast(), cr.addWebUIListener()
// and moltActions()).
extern const char kWebAISitesSettingsJs[];

#endif  // CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_WEB_AI_SITES_HANDLER_H_
