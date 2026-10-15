// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// WebUIMessageHandler backing chrome://molt-net/ (the toolbar MoltNet
// popover). It reports the routing of the window it is shown in — a MoltNet
// window is "Private via Tor", a regular window is "Not routed" — and drives
// MoltNetWindow / TorManager. It never proxies the profile it runs in.

#ifndef CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_NET_HANDLER_H_
#define CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_NET_HANDLER_H_

#include <memory>
#include <optional>
#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "content/public/browser/web_ui_message_handler.h"

class Profile;

namespace network {
class SimpleURLLoader;
}  // namespace network

class MoltNetHandler : public content::WebUIMessageHandler {
 public:
  MoltNetHandler();
  ~MoltNetHandler() override;

  MoltNetHandler(const MoltNetHandler&) = delete;
  MoltNetHandler& operator=(const MoltNetHandler&) = delete;

  // content::WebUIMessageHandler:
  void RegisterMessages() override;

 private:
  // Each takes [callback_id, ...args] and resolves the promise on the JS side
  // with the status dict (see StatusDict in the .cc), plus "error" on failure.
  void HandleGetStatus(const base::ListValue& args);
  void HandleGetExitCountries(const base::ListValue& args);
  void HandleSetExitCountry(const base::ListValue& args);
  // On: start Tor and open a MoltNet window. Off: close MoltNet windows and
  // stop Tor.
  void HandleToggle(const base::ListValue& args);
  // Legacy mode switch: "direct" == off, anything else == open a window.
  void HandleSetMode(const base::ListValue& args);
  void HandleOpenWindow(const base::ListValue& args);
  void HandleNewIdentity(const base::ListValue& args);

  // Circuit-transparency panel. GetCircuit returns the current Tor path
  // (guard -> middle -> exit with each relay's country + IP, from Tor's own
  // control port). VerifyExit fetches check.torproject.org/api/ip THROUGH
  // the MoltNet profile (i.e. through Tor) to confirm the exit IP websites
  // see. Nothing here ever looks up the user's real IP.
  void HandleGetCircuit(const base::ListValue& args);
  void HandleVerifyExit(const base::ListValue& args);
  void OnVerifyExitDone(std::string callback_id,
                        std::optional<std::string> body);

  // The profile backing this WebUI (the window that opened the popover).
  Profile* GetProfile();

  // Resolves |callback_id| with the current status plus |error| (if any).
  void ResolveWithStatus(const std::string& callback_id,
                         const std::string& error);

  // Loader for the (Tor-proxied) exit-verify fetch. One at a time.
  std::unique_ptr<network::SimpleURLLoader> verify_loader_;

  base::WeakPtrFactory<MoltNetHandler> weak_ptr_factory_{this};
};

#endif  // CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_NET_HANDLER_H_
