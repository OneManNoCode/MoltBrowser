// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Password-vault handler methods for MoltAIChatHandler. Split out of
// molt_ai_chat_handler.cc as part of MEDIUM #10 from the 2026-05-20
// code review.

#include "chrome/browser/ui/webui/molt_ai/molt_ai_chat_handler.h"

#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/common/molt_blocking_scope.h"
#include "chrome/browser/molt_ai/vault/vault_host_match.h"
#include "chrome/browser/molt_ai/vault/vault_store.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/simple_message_box.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/webui/molt_ai/molt_ai_chat_handler_helpers.h"
#include "content/public/browser/global_routing_id.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "url/origin.h"

using molt_ai::chat_handler_helpers::HasCallbackArgs;
using molt_ai::chat_handler_helpers::SecureClear;
using molt_ai::chat_handler_helpers::StringArg;

// ==================================================================
// Tier 4: Password vault (chat side)
//
// Backed by molt_ai::vault::VaultStore (OSCrypt-encrypted
// ~/.moltbrowser/vault.enc), opened per request so no plaintext
// credentials sit in long-lived memory.
//
// The chat page never receives a password: list/find return metadata
// only, and autofill sends the secret straight into the target page's
// isolated world. Adding entries and revealing a password live in
// molt://ai-settings (MoltVaultHandler), behind OS re-authentication.
// ==================================================================

namespace {

void ResolveError(base::DictValue& out, const char* error) {
  out.Set("success", false);
  out.Set("error", error);
}

}  // namespace

// vaultList(callback_id) -> {entries:[metadata], count}. Never includes
// passwords or notes (there is no flag to ask for them any more).
void MoltAIChatHandler::HandleVaultList(const base::ListValue& args) {
  AllowJavascript();
  if (!HasCallbackArgs(args, 1u, __func__)) return;
  const std::string callback_id = args[0].GetString();

  std::vector<molt_ai::vault::VaultEntry> entries;
  {
    ScopedAllowBlockingForMolt allow;
    molt_ai::vault::VaultStore store;
    entries = store.List();
  }
  base::ListValue arr;
  for (auto& e : entries) {
    arr.Append(e.ToMetadataDict());
    SecureClear(e.password);
    SecureClear(e.notes);
  }
  base::DictValue out;
  out.Set("entries", std::move(arr));
  out.Set("count", static_cast<int>(entries.size()));
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}

void MoltAIChatHandler::HandleVaultDelete(const base::ListValue& args) {
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
  base::DictValue out;
  out.Set("success", ok);
  if (!ok) out.Set("error", "entry not found");
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}

// vaultFindForActive(callback_id) -> {host, fillable, matches:[metadata +
// match: "exact"|"same_site"]}. Exact host matches come first.
void MoltAIChatHandler::HandleVaultFindForActive(
    const base::ListValue& args) {
  AllowJavascript();
  if (!HasCallbackArgs(args, 1u, __func__)) return;
  const std::string callback_id = args[0].GetString();
  std::string host;
  bool fillable = false;
  content::WebContents* webui_wc = web_ui()->GetWebContents();
  Browser* browser = chrome::FindBrowserWithTab(webui_wc);
  if (browser && browser->tab_strip_model()) {
    content::WebContents* t =
        browser->tab_strip_model()->GetActiveWebContents();
    if (t) {
      const url::Origin& origin =
          t->GetPrimaryMainFrame()->GetLastCommittedOrigin();
      host = origin.host();
      fillable = molt_ai::vault::IsFillableOrigin(origin);
    }
  }
  std::vector<molt_ai::vault::VaultEntry> matches;
  {
    ScopedAllowBlockingForMolt allow;
    molt_ai::vault::VaultStore store;
    matches = store.FindForHost(host);
  }
  base::ListValue arr;
  for (auto& e : matches) {
    base::DictValue d = e.ToMetadataDict();
    d.Set("match", molt_ai::vault::MatchSiteHost(e.site_host, host) ==
                           molt_ai::vault::HostMatch::kExact
                       ? "exact"
                       : "same_site");
    arr.Append(std::move(d));
    SecureClear(e.password);
    SecureClear(e.notes);
  }
  base::DictValue out;
  out.Set("host", host);
  out.Set("fillable", fillable);
  out.Set("matches", std::move(arr));
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}

namespace {

// JS injection that finds the most likely login form and fills it.
// Strategy:
//   0. Refuse unless the document is still the origin the browser
//      validated (creds.origin). The browser re-checks the committed
//      origin right before sending this, but the script runs a moment
//      later in the renderer; this closes that last gap.
//   1. Find every visible input[type=password].
//   2. For each, find a username field — preferring inputs in the
//      same <form>, falling back to nearest preceding input with a
//      text-ish type. We pick by autocomplete + name + id signals
//      ("username", "email", "user", "login").
//   3. Set values via a property descriptor write so React/Vue
//      controlled inputs pick up the change, then dispatch
//      input + change events.
// Returns {filled: int, total_pwd: int, used_form: bool}.
constexpr char kVaultAutofillJS[] = R"JS(
(function(creds) {
  if (window.location.origin !== creds.origin) {
    return {filled: 0, total_pwd: 0, used_form: false,
            error: 'the page changed before it could be filled'};
  }
  function isVisible(el) {
    if (!el) return false;
    var r = el.getBoundingClientRect();
    if (r.width === 0 || r.height === 0) return false;
    var s = getComputedStyle(el);
    return s.visibility !== 'hidden' && s.display !== 'none';
  }
  function setNative(el, value) {
    var proto = el.tagName === 'TEXTAREA'
        ? HTMLTextAreaElement.prototype
        : HTMLInputElement.prototype;
    var setter = Object.getOwnPropertyDescriptor(proto, 'value').set;
    setter.call(el, value);
    el.dispatchEvent(new Event('input', {bubbles: true}));
    el.dispatchEvent(new Event('change', {bubbles: true}));
  }
  function userScore(el) {
    // Score 0-100 for "this looks like a username/email input".
    var s = 0;
    var ac = (el.getAttribute('autocomplete') || '').toLowerCase();
    if (/username|email/.test(ac)) s += 60;
    var n = (el.getAttribute('name') || '').toLowerCase();
    var id = (el.id || '').toLowerCase();
    var ph = (el.getAttribute('placeholder') || '').toLowerCase();
    var bag = n + ' ' + id + ' ' + ph;
    if (/(^|[^a-z])(user|login|email|account)([^a-z]|$)/.test(bag)) s += 30;
    var t = (el.type || '').toLowerCase();
    if (t === 'email') s += 30;
    if (t === 'text' || t === 'tel') s += 10;
    return s;
  }
  function pickUsernameFor(pwd) {
    var candidates = [];
    var form = pwd.form;
    if (form) {
      var inputs = form.querySelectorAll('input');
      inputs.forEach(function(i){
        if (i === pwd) return;
        if (!isVisible(i)) return;
        var t = (i.type || '').toLowerCase();
        if (t === 'hidden' || t === 'submit' || t === 'button') return;
        if (t === 'password') return;
        candidates.push(i);
      });
    }
    if (!candidates.length) {
      // Fallback: scan all visible inputs preceding the password field.
      var all = Array.from(document.querySelectorAll('input'));
      var idx = all.indexOf(pwd);
      for (var i = idx - 1; i >= 0; i--) {
        var x = all[i];
        if (!isVisible(x)) continue;
        var t = (x.type || '').toLowerCase();
        if (t === 'password' || t === 'hidden' ||
            t === 'submit' || t === 'button') continue;
        candidates.push(x);
        if (candidates.length >= 4) break;
      }
    }
    if (!candidates.length) return null;
    // Pick the highest-scoring visible candidate.
    candidates.sort(function(a,b){ return userScore(b) - userScore(a); });
    return candidates[0];
  }
  var pwds = Array.from(document.querySelectorAll('input[type=password]'))
                  .filter(isVisible);
  if (!pwds.length) {
    return {filled: 0, total_pwd: 0, used_form: false,
            error: 'no visible password field'};
  }
  var filled = 0;
  var usedForm = false;
  pwds.forEach(function(p) {
    setNative(p, creds.password);
    var u = pickUsernameFor(p);
    if (u) {
      setNative(u, creds.username || '');
      usedForm = !!p.form;
      filled++;
    }
  });
  return {filled: filled, total_pwd: pwds.length, used_form: usedForm};
})
)JS";

}  // namespace

// vaultAutofill(callback_id, entry_id?) — fill a saved login into the
// active tab. Without an id, the best match for the tab is used.
//
//   * The tab's committed origin must be https (http only on loopback).
//   * The entry must be for the same host (filled straight away) or the
//     same registrable domain (filled only after the user says yes in a
//     native, browser-drawn dialog that page script can't click). Any
//     other entry is refused, even when picked explicitly by id.
//   * The document is pinned when the user asks; FillVaultEntry()
//     re-checks that the same document with the same origin is still
//     there immediately before the password leaves the browser process.
void MoltAIChatHandler::HandleVaultAutofill(const base::ListValue& args) {
  AllowJavascript();
  if (!HasCallbackArgs(args, 2u, __func__)) return;
  const std::string callback_id = args[0].GetString();
  const std::string entry_id = StringArg(args, 1);
  base::DictValue out;

  // Resolve target tab.
  content::WebContents* webui_wc = web_ui()->GetWebContents();
  Browser* browser = chrome::FindBrowserWithTab(webui_wc);
  if (!browser || !browser->tab_strip_model()) {
    ResolveError(out, "no owning browser");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  content::WebContents* target =
      browser->tab_strip_model()->GetActiveWebContents();
  content::RenderFrameHost* frame =
      target ? target->GetPrimaryMainFrame() : nullptr;
  if (!frame) {
    ResolveError(out, "no active tab");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  const url::Origin origin = frame->GetLastCommittedOrigin();
  if (!molt_ai::vault::IsFillableOrigin(origin)) {
    ResolveError(out,
                 "Autofill only works on https:// pages (http:// only on "
                 "localhost).");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  const std::string page_host = origin.host();

  // Resolve the entry (metadata only; the password is re-read at fill
  // time). If no id was supplied, take the best match for this host:
  // exact matches sort first, then most recently used.
  molt_ai::vault::VaultEntry chosen;
  bool found = false;
  {
    ScopedAllowBlockingForMolt allow;
    molt_ai::vault::VaultStore store;
    if (!entry_id.empty()) {
      if (auto e = store.Get(entry_id)) {
        chosen = *std::move(e);
        found = true;
      }
    } else {
      std::vector<molt_ai::vault::VaultEntry> picks =
          store.FindForHost(page_host);
      if (!picks.empty()) {
        chosen = std::move(picks[0]);
        found = true;
      }
      for (auto& p : picks) SecureClear(p.password);
    }
  }
  SecureClear(chosen.password);
  SecureClear(chosen.notes);
  if (!found) {
    ResolveError(out, entry_id.empty() ? "no vault entry matches this site"
                                       : "vault entry not found");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  const molt_ai::vault::HostMatch match =
      molt_ai::vault::MatchSiteHost(chosen.site_host, page_host);
  if (match == molt_ai::vault::HostMatch::kNone) {
    out.Set("success", false);
    out.Set("error", base::StrCat({"This login is saved for ",
                                   chosen.site_host, ", not ", page_host,
                                   ". Nothing was filled."}));
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  const content::GlobalRenderFrameHostId frame_id = frame->GetGlobalId();
  if (match == molt_ai::vault::HostMatch::kExact) {
    FillVaultEntry(callback_id, chosen.id, frame_id, origin);
    return;
  }

  // Same registrable domain, different host: ask first, in browser UI.
  const std::u16string message = base::UTF8ToUTF16(base::StrCat(
      {"The saved login",
       chosen.username.empty() ? "" : " for " + chosen.username,
       " belongs to ", chosen.site_host, ", but this page is ", page_host,
       ".\n\nFill it into ", page_host, " anyway?"}));
  chrome::ShowQuestionMessageBoxAsync(
      target->GetTopLevelNativeWindow(), u"Fill saved login?", message,
      base::BindOnce(
          [](base::WeakPtr<MoltAIChatHandler> self, std::string cb_id,
             std::string id, content::GlobalRenderFrameHostId frame_id,
             url::Origin origin, chrome::MessageBoxResult result) {
            if (!self) return;
            if (result != chrome::MESSAGE_BOX_RESULT_YES) {
              base::DictValue r;
              r.Set("success", false);
              r.Set("cancelled", true);
              r.Set("error", "Autofill cancelled");
              self->ResolveJavascriptCallback(base::Value(cb_id),
                                              base::Value(std::move(r)));
              return;
            }
            self->FillVaultEntry(cb_id, id, frame_id, origin);
          },
          weak_ptr_factory_.GetWeakPtr(), callback_id, chosen.id, frame_id,
          origin));
}

void MoltAIChatHandler::FillVaultEntry(
    const std::string& callback_id,
    const std::string& entry_id,
    const content::GlobalRenderFrameHostId& frame_id,
    const url::Origin& origin) {
  base::DictValue out;
  // The same document must still be in the tab, with the same origin, as
  // when the user asked: never fill a tab that navigated (or a frame that
  // was swapped) in the meantime — e.g. while the confirmation was up.
  content::RenderFrameHost* frame = content::RenderFrameHost::FromID(frame_id);
  if (!frame || !frame->IsActive() || !frame->IsInPrimaryMainFrame() ||
      frame->GetLastCommittedOrigin() != origin) {
    ResolveError(out,
                 "The page changed since you asked to fill it. Nothing was "
                 "filled.");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  molt_ai::vault::VaultEntry chosen;
  bool found = false;
  {
    ScopedAllowBlockingForMolt allow;
    molt_ai::vault::VaultStore store;
    if (auto e = store.Get(entry_id)) {
      chosen = *std::move(e);
      // The entry could have been edited while a confirmation was up.
      found = molt_ai::vault::MatchSiteHost(chosen.site_host,
                                            origin.host()) !=
              molt_ai::vault::HostMatch::kNone;
      if (found) store.TouchLastUsed(chosen.id);
    }
  }
  if (!found) {
    SecureClear(chosen.password);
    ResolveError(out, "vault entry not found for this site");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  // Compose creds JSON for the JS shim.
  base::DictValue creds;
  creds.Set("username", chosen.username);
  creds.Set("password", chosen.password);
  creds.Set("origin", origin.Serialize());
  std::string creds_json;
  base::JSONWriter::Write(creds, &creds_json);
  std::string js = std::string(kVaultAutofillJS) + "(" + creds_json + ")";
  // The renderer-side copy lives in the UTF-16 string passed below
  // for the duration of the eval; nothing we can do about that. But
  // our process-side copies (creds_json, js) get scrubbed as soon as
  // the eval is dispatched. Code-review MEDIUM #7.
  frame->ExecuteJavaScriptInIsolatedWorld(
      base::UTF8ToUTF16(js),
      base::BindOnce(
          [](base::WeakPtr<MoltAIChatHandler> self, std::string cb_id,
             base::Value v) {
            if (!self) return;
            base::DictValue r;
            if (v.is_dict()) {
              const base::DictValue& d = v.GetDict();
              auto filled = d.FindInt("filled");
              auto total = d.FindInt("total_pwd");
              bool ok = filled.value_or(0) > 0;
              r.Set("success", ok);
              r.Set("filled", filled.value_or(0));
              r.Set("total_password_fields", total.value_or(0));
              if (auto* err = d.FindString("error")) r.Set("error", *err);
            } else {
              r.Set("success", false);
              r.Set("error", "autofill script returned no data");
            }
            self->ResolveJavascriptCallback(base::Value(cb_id),
                                            base::Value(std::move(r)));
          },
          weak_ptr_factory_.GetWeakPtr(), callback_id),
      /*world_id=*/1);
  // Scrub the plaintext password from our local heap.
  SecureClear(creds_json);
  SecureClear(js);
  SecureClear(chosen.password);
  if (std::string* pw = creds.FindString("password")) SecureClear(*pw);
}
