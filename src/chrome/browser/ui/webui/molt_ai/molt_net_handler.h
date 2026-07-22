// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// WebUIMessageHandler backing chrome://molt-net/. Bridges the glass popover's
// chrome.send() calls to the process-global molt_ai::tor::TorManager — the
// same backend the toolbar globe menu drives. No mojo, no Profile plumbing:
// every call goes through TorManager::Get().

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
  // Each takes [callback_id, ...args] and resolves the promise on the JS side.
  void HandleGetStatus(const base::ListValue& args);
  void HandleGetExitCountries(const base::ListValue& args);
  void HandleSetExitCountry(const base::ListValue& args);
  void HandleToggle(const base::ListValue& args);
  void HandleSetMode(const base::ListValue& args);
  void HandleNewIdentity(const base::ListValue& args);

  // Circuit-transparency panel. GetCircuit returns the current Tor path (guard
  // -> middle -> exit with each relay's country + IP) plus the cached origin
  // IP. VerifyExit fetches check.torproject.org/api/ip THROUGH Tor to confirm
  // the exit IP websites actually see.
  void HandleGetCircuit(const base::ListValue& args);
  void HandleVerifyExit(const base::ListValue& args);
  void OnVerifyExitDone(std::string callback_id,
                        std::optional<std::string> body);

  // The profile backing this WebUI (the browser window that opened the
  // popover). Null only if the WebContents/context is gone.
  Profile* GetProfile();

  // Idempotently reconcile this profile's network routing with the Tor
  // daemon's actual running state: Tor up -> apply leak-safe routing; Tor
  // down -> restore direct. Used on the synchronous stop paths.
  void ApplyRoutingForCurrentTorState();

  // Called from TorManager::Launch's callback. |tor_ready| is true only once
  // Tor has an established circuit (can actually carry traffic). Ready ->
  // apply routing; NOT ready (bootstrap failed/timed out) -> stop the daemon
  // and keep the profile on a direct connection, so a stuck Tor never leaves
  // the browser stranded behind a dead proxy.
  void ApplyRoutingForLaunchResult(bool tor_ready);

  // Loader for the (Tor-proxied) exit-verify fetch. One at a time.
  std::unique_ptr<network::SimpleURLLoader> verify_loader_;

  base::WeakPtrFactory<MoltNetHandler> weak_ptr_factory_{this};
};

#endif  // CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_NET_HANDLER_H_
