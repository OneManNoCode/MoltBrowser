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
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/common/molt_blocking_scope.h"
#include "chrome/browser/molt_ai/vault/vault_store.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/webui/molt_ai/molt_ai_chat_handler_helpers.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"

// ==================================================================
// Tier 4: Password vault
//
// Five small handlers backed by molt_ai::vault::VaultStore (OSCrypt-
// encrypted ~/.moltbrowser/vault.enc). The store is opened
// per-request — sub-millisecond on a small vault, and avoids holding
// any plaintext credentials in long-lived memory.
// ==================================================================

// HandleVaultList: dump entries. Passwords are scrubbed by default;
// callers pass include_passwords=true (boolean second arg) only when
// they need the plaintext (e.g. the dedicated reveal view).
void MoltAIChatHandler::HandleVaultList(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 1u);
  const std::string callback_id = args[0].GetString();
  bool include_passwords = false;
  if (args.size() > 1 && args[1].is_bool())
    include_passwords = args[1].GetBool();

  std::vector<molt_ai::vault::VaultEntry> entries;
  {
    ScopedAllowBlockingForMolt allow;
    molt_ai::vault::VaultStore store;
    entries = store.List();
  }
  base::ListValue arr;
  for (const auto& e : entries) {
    base::DictValue d;
    d.Set("id", e.id);
    d.Set("site_host", e.site_host);
    d.Set("username", e.username);
    if (include_passwords) d.Set("password", e.password);
    d.Set("notes", e.notes);
    d.Set("created_at_unix", static_cast<double>(e.created_at_unix));
    d.Set("last_used_unix", static_cast<double>(e.last_used_unix));
    d.Set("last_changed_unix", static_cast<double>(e.last_changed_unix));
    arr.Append(std::move(d));
  }
  base::DictValue out;
  out.Set("entries", std::move(arr));
  out.Set("count", static_cast<int>(entries.size()));
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}

void MoltAIChatHandler::HandleVaultAdd(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 2u);
  const std::string callback_id = args[0].GetString();
  base::DictValue out;
  if (!args[1].is_dict()) {
    out.Set("success", false);
    out.Set("error", "expected entry dict");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  const base::DictValue& d = args[1].GetDict();
  molt_ai::vault::VaultEntry e;
  if (auto* s = d.FindString("site_host")) e.site_host = *s;
  if (auto* s = d.FindString("username"))  e.username = *s;
  if (auto* s = d.FindString("password"))  e.password = *s;
  if (auto* s = d.FindString("notes"))     e.notes = *s;
  if (e.site_host.empty() || e.password.empty()) {
    out.Set("success", false);
    out.Set("error", "site_host and password required");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  std::string id;
  {
    ScopedAllowBlockingForMolt allow;
    molt_ai::vault::VaultStore store;
    id = store.Add(std::move(e));
  }
  if (id.empty()) {
    out.Set("success", false);
    out.Set("error", "save failed");
  } else {
    out.Set("success", true);
    out.Set("id", id);
  }
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}

void MoltAIChatHandler::HandleVaultDelete(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 2u);
  const std::string callback_id = args[0].GetString();
  std::string id = args[1].is_string() ? args[1].GetString() : "";
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

void MoltAIChatHandler::HandleVaultFindForActive(
    const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 1u);
  const std::string callback_id = args[0].GetString();
  // Resolve the active tab's host.
  std::string host;
  content::WebContents* webui_wc = web_ui()->GetWebContents();
  Browser* browser = chrome::FindBrowserWithTab(webui_wc);
  if (browser && browser->tab_strip_model()) {
    content::WebContents* t =
        browser->tab_strip_model()->GetActiveWebContents();
    if (t) host = std::string(t->GetLastCommittedURL().host());
  }
  std::vector<molt_ai::vault::VaultEntry> matches;
  {
    ScopedAllowBlockingForMolt allow;
    molt_ai::vault::VaultStore store;
    matches = store.FindForHost(host);
  }
  base::ListValue arr;
  for (const auto& e : matches) {
    base::DictValue d;
    d.Set("id", e.id);
    d.Set("site_host", e.site_host);
    d.Set("username", e.username);
    d.Set("last_used_unix", static_cast<double>(e.last_used_unix));
    arr.Append(std::move(d));
  }
  base::DictValue out;
  out.Set("host", host);
  out.Set("matches", std::move(arr));
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}

namespace {

// JS injection that finds the most likely login form and fills it.
// Strategy:
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

void MoltAIChatHandler::HandleVaultAutofill(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 2u);
  const std::string callback_id = args[0].GetString();
  std::string entry_id = args[1].is_string() ? args[1].GetString() : "";
  base::DictValue out;

  // Resolve target tab.
  content::WebContents* webui_wc = web_ui()->GetWebContents();
  Browser* browser = chrome::FindBrowserWithTab(webui_wc);
  if (!browser || !browser->tab_strip_model()) {
    out.Set("success", false);
    out.Set("error", "no owning browser");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  content::WebContents* target =
      browser->tab_strip_model()->GetActiveWebContents();
  if (!target || !target->GetLastCommittedURL().SchemeIsHTTPOrHTTPS()) {
    out.Set("success", false);
    out.Set("error", "active tab is not an http(s) page");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  // Resolve the entry. If no id supplied, pick the most-recently-
  // used match for the active tab's host.
  molt_ai::vault::VaultEntry chosen;
  {
    ScopedAllowBlockingForMolt allow;
    molt_ai::vault::VaultStore store;
    std::vector<molt_ai::vault::VaultEntry> picks;
    if (!entry_id.empty()) {
      for (auto& e : store.List()) {
        if (e.id == entry_id) { picks.push_back(std::move(e)); break; }
      }
    } else {
      picks = store.FindForHost(
          std::string(target->GetLastCommittedURL().host()));
    }
    if (picks.empty()) {
      out.Set("success", false);
      out.Set("error",
              entry_id.empty()
                  ? "no vault entry matches this site"
                  : "vault entry not found");
      ResolveJavascriptCallback(base::Value(callback_id),
                                base::Value(std::move(out)));
      return;
    }
    chosen = std::move(picks[0]);
    store.TouchLastUsed(chosen.id);
  }

  // Compose creds JSON for the JS shim.
  base::DictValue creds;
  creds.Set("username", chosen.username);
  creds.Set("password", chosen.password);
  std::string creds_json;
  base::JSONWriter::Write(creds, &creds_json);
  std::string js = std::string(kVaultAutofillJS) + "(" + creds_json + ")";
  // The renderer-side copy lives in the UTF-16 string passed below
  // for the duration of the eval; nothing we can do about that. But
  // our process-side copies (creds_json, js) get scrubbed as soon as
  // the eval is dispatched. Code-review MEDIUM #7.

  std::string cb_id = callback_id;
  auto weak_this = weak_ptr_factory_.GetWeakPtr();
  target->GetPrimaryMainFrame()->ExecuteJavaScriptInIsolatedWorld(
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
          weak_this, cb_id),
      /*world_id=*/1);
  // Scrub the plaintext password from our local heap. The chosen
  // VaultEntry's own .password field also gets cleared as the local
  // variable goes out of scope at function exit, but its std::string
  // backing buffer survives — scrub that too.
  molt_ai::chat_handler_helpers::SecureClear(creds_json);
  molt_ai::chat_handler_helpers::SecureClear(js);
  molt_ai::chat_handler_helpers::SecureClear(chosen.password);
}
