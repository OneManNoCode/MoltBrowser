// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/webui/molt_ai/molt_vault_handler.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "chrome/browser/molt_ai/common/molt_blocking_scope.h"
#include "chrome/browser/molt_ai/keys/molt_keys_store.h"
#include "chrome/browser/molt_ai/vault/vault_host_match.h"
#include "chrome/browser/molt_ai/vault/vault_store.h"
#include "chrome/browser/ui/webui/molt_ai/molt_ai_chat_handler_helpers.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"

#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_CHROMEOS)
#include "chrome/browser/device_reauth/chrome_device_authenticator_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "components/device_reauth/device_authenticator.h"
#else
#include "chrome/browser/ui/simple_message_box.h"
#endif

namespace {

using molt_ai::chat_handler_helpers::HasCallbackArgs;
using molt_ai::chat_handler_helpers::SecureClear;
using molt_ai::chat_handler_helpers::StringArg;

// Generous limits; anything bigger is not a credential.
constexpr size_t kMaxHostLength = 253;
constexpr size_t kMaxUsernameLength = 512;
constexpr size_t kMaxPasswordLength = 1024;
constexpr size_t kMaxNotesLength = 4096;

base::DictValue ErrorResult(std::string_view message) {
  base::DictValue d;
  d.Set("success", false);
  d.Set("error", message);
  return d;
}

}  // namespace

MoltVaultHandler::MoltVaultHandler() = default;

MoltVaultHandler::~MoltVaultHandler() = default;

void MoltVaultHandler::RegisterMessages() {
  web_ui()->RegisterMessageCallback(
      "vaultList", base::BindRepeating(&MoltVaultHandler::HandleList,
                                       base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "vaultAdd", base::BindRepeating(&MoltVaultHandler::HandleAdd,
                                      base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "vaultDelete", base::BindRepeating(&MoltVaultHandler::HandleDelete,
                                         base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "vaultRevealEntry",
      base::BindRepeating(&MoltVaultHandler::HandleRevealEntry,
                          base::Unretained(this)));
}

void MoltVaultHandler::OnJavascriptDisallowed() {
  // The page went away (reload / navigation): drop any pending reveal so
  // its result can never be delivered to a later document.
  weak_ptr_factory_.InvalidateWeakPtrs();
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_CHROMEOS)
  if (authenticator_) {
    authenticator_->Cancel();
  }
#endif
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_CHROMEOS)
  // Windows authentication can't be cancelled; dropping the authenticator
  // just ignores its result (same as Chrome's password settings).
  authenticator_.reset();
#endif
  reveal_pending_ = false;
}

void MoltVaultHandler::Resolve(const std::string& callback_id,
                               base::DictValue result) {
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(result)));
}

// vaultList: metadata only (no password, no notes text), plus whether new
// secrets can be stored at all (see molt_ai::IsSecureStorageAvailable()).
void MoltVaultHandler::HandleList(const base::ListValue& args) {
  AllowJavascript();
  if (!HasCallbackArgs(args, 1u, __func__)) return;
  const std::string callback_id = args[0].GetString();

  std::vector<molt_ai::vault::VaultEntry> entries;
  bool secure_storage = false;
  {
    ScopedAllowBlockingForMolt allow;
    molt_ai::vault::VaultStore store;
    entries = store.List();
    secure_storage = molt_ai::IsSecureStorageAvailable();
  }
  base::ListValue list;
  for (auto& e : entries) {
    list.Append(e.ToMetadataDict());
    SecureClear(e.password);
    SecureClear(e.notes);
  }
  base::DictValue out;
  out.Set("success", true);
  out.Set("count", static_cast<int>(list.size()));
  out.Set("entries", std::move(list));
  out.Set("secure_storage", secure_storage);
  if (!secure_storage) {
    out.Set("message", molt_ai::SecureStorageUnavailableMessage());
  }
  Resolve(callback_id, std::move(out));
}

void MoltVaultHandler::HandleAdd(const base::ListValue& args) {
  AllowJavascript();
  if (!HasCallbackArgs(args, 2u, __func__)) return;
  const std::string callback_id = args[0].GetString();
  if (!args[1].is_dict()) {
    Resolve(callback_id, ErrorResult("expected entry dict"));
    return;
  }
  const base::DictValue& d = args[1].GetDict();
  molt_ai::vault::VaultEntry e;
  if (const std::string* s = d.FindString("site_host")) e.site_host = *s;
  if (const std::string* s = d.FindString("username")) e.username = *s;
  if (const std::string* s = d.FindString("password")) e.password = *s;
  if (const std::string* s = d.FindString("notes")) e.notes = *s;

  // Store the canonical host ("https://GitHub.com/login" -> "github.com")
  // so autofill matching is exact and predictable.
  const std::string host = molt_ai::vault::CanonicalizeSiteHost(e.site_host);
  std::string error;
  if (host.empty() || host.size() > kMaxHostLength) {
    error = "Enter the site, e.g. github.com";
  } else if (e.password.empty()) {
    error = "Enter the password";
  } else if (e.password.size() > kMaxPasswordLength ||
             e.username.size() > kMaxUsernameLength ||
             e.notes.size() > kMaxNotesLength) {
    error = "Entry is too long";
  }
  if (!error.empty()) {
    SecureClear(e.password);
    Resolve(callback_id, ErrorResult(error));
    return;
  }
  e.site_host = host;

  std::string id;
  bool secure_storage = false;
  {
    ScopedAllowBlockingForMolt allow;
    secure_storage = molt_ai::IsSecureStorageAvailable();
    if (secure_storage) {
      molt_ai::vault::VaultStore store;
      id = store.Add(e);
    }
  }
  SecureClear(e.password);
  SecureClear(e.notes);
  if (id.empty()) {
    Resolve(callback_id,
            ErrorResult(secure_storage ? "Could not save the password"
                                 : molt_ai::SecureStorageUnavailableMessage()));
    return;
  }
  base::DictValue out;
  out.Set("success", true);
  out.Set("id", id);
  out.Set("site_host", host);
  Resolve(callback_id, std::move(out));
}

void MoltVaultHandler::HandleDelete(const base::ListValue& args) {
  AllowJavascript();
  if (!HasCallbackArgs(args, 2u, __func__)) return;
  const std::string callback_id = args[0].GetString();
  const std::string id = StringArg(args, 1);
  bool ok = false;
  if (!id.empty()) {
    ScopedAllowBlockingForMolt allow;
    molt_ai::vault::VaultStore store;
    ok = store.Delete(id);
  }
  if (!ok) {
    Resolve(callback_id, ErrorResult("Entry not found"));
    return;
  }
  base::DictValue out;
  out.Set("success", true);
  Resolve(callback_id, std::move(out));
}

void MoltVaultHandler::HandleRevealEntry(const base::ListValue& args) {
  AllowJavascript();
  if (!HasCallbackArgs(args, 2u, __func__)) return;
  const std::string callback_id = args[0].GetString();
  const std::string id = StringArg(args, 1);

  if (reveal_pending_) {
    Resolve(callback_id, ErrorResult("Finish the open confirmation first"));
    return;
  }
  std::string site_host;
  {
    ScopedAllowBlockingForMolt allow;
    molt_ai::vault::VaultStore store;
    std::optional<molt_ai::vault::VaultEntry> e = store.Get(id);
    if (e) {
      site_host = e->site_host;
      SecureClear(e->password);
      SecureClear(e->notes);
    }
  }
  if (site_host.empty()) {
    Resolve(callback_id, ErrorResult("Entry not found"));
    return;
  }

  content::WebContents* web_contents = web_ui()->GetWebContents();
  reveal_pending_ = true;
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_CHROMEOS)
  // OS re-authentication (Touch ID / Windows Hello / system password),
  // the same mechanism Chrome's own password settings use.
  device_reauth::DeviceAuthParams params(
      base::Seconds(60), device_reauth::DeviceAuthSource::kPasswordManager);
  authenticator_ = ChromeDeviceAuthenticatorFactory::GetForProfile(
      Profile::FromWebUI(web_ui()), web_contents->GetTopLevelNativeWindow(),
      params);
  authenticator_->AuthenticateWithMessage(
      base::UTF8ToUTF16(
          base::StrCat({"show the saved password for ", site_host})),
      base::BindOnce(&MoltVaultHandler::OnRevealAuthenticated,
                     weak_ptr_factory_.GetWeakPtr(), callback_id, id));
#else
  // No OS re-authentication in Chromium on this platform: ask in a
  // native dialog drawn by the browser, which the page can't answer.
  chrome::ShowQuestionMessageBoxAsync(
      web_contents->GetTopLevelNativeWindow(), u"Show saved password?",
      base::UTF8ToUTF16(base::StrCat(
          {"Show the saved password for ", site_host,
           " on screen?\n\nMake sure nobody is looking at your screen."})),
      base::BindOnce(
          [](base::WeakPtr<MoltVaultHandler> self, std::string cb_id,
             std::string entry_id, chrome::MessageBoxResult result) {
            if (!self) return;
            self->OnRevealAuthenticated(
                std::move(cb_id), std::move(entry_id),
                result == chrome::MESSAGE_BOX_RESULT_YES);
          },
          weak_ptr_factory_.GetWeakPtr(), callback_id, id));
#endif
}

void MoltVaultHandler::OnRevealAuthenticated(std::string callback_id,
                                             std::string entry_id,
                                             bool authenticated) {
  reveal_pending_ = false;
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_CHROMEOS)
  authenticator_.reset();
#endif
  if (!IsJavascriptAllowed()) return;
  if (!authenticated) {
    base::DictValue out = ErrorResult("Not revealed");
    out.Set("cancelled", true);
    Resolve(callback_id, std::move(out));
    return;
  }
  std::optional<molt_ai::vault::VaultEntry> e;
  {
    ScopedAllowBlockingForMolt allow;
    molt_ai::vault::VaultStore store;
    e = store.Get(entry_id);
  }
  if (!e) {
    Resolve(callback_id, ErrorResult("Entry not found"));
    return;
  }
  // Exactly one secret leaves the browser process, on explicit request.
  base::DictValue out;
  out.Set("success", true);
  out.Set("id", e->id);
  out.Set("password", e->password);
  out.Set("notes", e->notes);
  Resolve(callback_id, std::move(out));
  SecureClear(e->password);
  SecureClear(e->notes);
}
