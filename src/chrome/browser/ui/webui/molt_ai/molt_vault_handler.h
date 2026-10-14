// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltVaultHandler: password-vault management for molt://ai-settings
// (Passwords section). This is the only WebUI that can add an entry or
// see a stored password, and it sees at most one password per explicit
// request:
//
//   vaultList(callback_id)                 → {entries:[metadata], count,
//                                             secure_storage, message?}
//   vaultAdd(callback_id, {site_host, username, password, notes?})
//   vaultDelete(callback_id, id)
//   vaultRevealEntry(callback_id, id)      → {success, id, password, notes}
//
// vaultRevealEntry requires OS re-authentication (Touch ID / system
// password via device_reauth::DeviceAuthenticator) on macOS, Windows and
// ChromeOS. Chromium has no OS re-auth on Linux, so there it requires a
// yes in a native, browser-drawn confirmation dialog (which page script
// can't click). Only one reveal can be pending at a time.

#ifndef CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_VAULT_HANDLER_H_
#define CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_VAULT_HANDLER_H_

#include <memory>
#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "build/build_config.h"
#include "content/public/browser/web_ui_message_handler.h"

#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_CHROMEOS)
namespace device_reauth {
class DeviceAuthenticator;
}  // namespace device_reauth
#endif

class MoltVaultHandler : public content::WebUIMessageHandler {
 public:
  MoltVaultHandler();
  ~MoltVaultHandler() override;

  MoltVaultHandler(const MoltVaultHandler&) = delete;
  MoltVaultHandler& operator=(const MoltVaultHandler&) = delete;

  // content::WebUIMessageHandler:
  void RegisterMessages() override;
  void OnJavascriptDisallowed() override;

 private:
  void HandleList(const base::ListValue& args);
  void HandleAdd(const base::ListValue& args);
  void HandleDelete(const base::ListValue& args);
  void HandleRevealEntry(const base::ListValue& args);

  // Completes a reveal once the user has (or hasn't) re-authenticated.
  void OnRevealAuthenticated(std::string callback_id,
                             std::string entry_id,
                             bool authenticated);

  void Resolve(const std::string& callback_id, base::DictValue result);

#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_CHROMEOS)
  std::unique_ptr<device_reauth::DeviceAuthenticator> authenticator_;
#endif
  bool reveal_pending_ = false;

  base::WeakPtrFactory<MoltVaultHandler> weak_ptr_factory_{this};
};

#endif  // CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_VAULT_HANDLER_H_
