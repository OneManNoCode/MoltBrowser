// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/webui/molt_ai/molt_web_ai_sites_handler.h"

#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/molt_ai/common/molt_settings_file.h"
#include "chrome/browser/molt_ai/web_ai/runtime_web_ai_backend.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_settings_switch.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_site_permissions.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_site_permissions_service.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/webui/molt_ai/molt_ai_chat_handler_helpers.h"
#include "components/url_formatter/elide_url.h"
#include "content/public/browser/web_ui.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace {

using molt_ai::chat_handler_helpers::BoolArg;
using molt_ai::chat_handler_helpers::BoundedStringArg;
using molt_ai::chat_handler_helpers::HasCallbackArgs;
using molt_ai::web_ai::SiteListEntry;
using molt_ai::web_ai::WebAISitePermissions;

const char* KindName(SiteListEntry::Kind kind) {
  switch (kind) {
    case SiteListEntry::Kind::kAllowed:
      return "allowed";
    case SiteListEntry::Kind::kBlocked:
      return "blocked";
    case SiteListEntry::Kind::kPaused:
      return "paused";
  }
  return "blocked";
}

// "example.com", or "http://example.com" for a site that isn't https.
std::string DisplayName(const std::string& site) {
  return base::UTF16ToUTF8(url_formatter::FormatOriginForSecurityDisplay(
      url::Origin::Create(GURL(site)),
      url_formatter::SchemeDisplay::OMIT_CRYPTOGRAPHIC));
}

// After settings.json was written (|ok|) with |enabled|, or not: pages see
// the change on their next call instead of within 30 s, and their sessions
// at once (switched off, they end). Done whether or not the settings page
// is still open; then |reply| tells the page.
void ApplySwitchWrite(bool enabled,
                      base::OnceCallback<void(bool)> reply,
                      bool ok) {
  molt_ai::web_ai::RuntimeWebAIBackend* backend =
      molt_ai::web_ai::RuntimeWebAIBackend::GetInstance();
  if (ok) {
    backend->SetWebApisSwitch(enabled);
  } else {
    backend->InvalidateDeviceFacts();
  }
  std::move(reply).Run(ok);
}

}  // namespace

MoltWebAISitesHandler::MoltWebAISitesHandler() = default;
MoltWebAISitesHandler::~MoltWebAISitesHandler() = default;

void MoltWebAISitesHandler::RegisterMessages() {
  web_ui()->RegisterMessageCallback(
      "webAiSitesGetState",
      base::BindRepeating(&MoltWebAISitesHandler::HandleGetState,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "webAiSitesRemove",
      base::BindRepeating(&MoltWebAISitesHandler::HandleRemove,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "webAiSitesClear",
      base::BindRepeating(&MoltWebAISitesHandler::HandleClear,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "webAiSitesSetEnabled",
      base::BindRepeating(&MoltWebAISitesHandler::HandleSetEnabled,
                          base::Unretained(this)));
}

void MoltWebAISitesHandler::OnJavascriptAllowed() {
  if (WebAISitePermissions* permissions = Permissions()) {
    sites_subscription_ = permissions->AddObserver(base::BindRepeating(
        &MoltWebAISitesHandler::OnSitesChanged, weak_factory_.GetWeakPtr()));
  }
}

void MoltWebAISitesHandler::OnJavascriptDisallowed() {
  sites_subscription_ = base::CallbackListSubscription();
  weak_factory_.InvalidateWeakPtrs();
}

WebAISitePermissions* MoltWebAISitesHandler::Permissions() {
  Profile* profile = Profile::FromWebUI(web_ui());
  if (!profile) {
    return nullptr;
  }
  // Incognito and MoltNet windows manage the regular profile's answers; a
  // Guest session has only its own.
  if (!profile->IsGuestSession()) {
    profile = profile->GetOriginalProfile();
  }
  return WebAISitePermissions::Get(profile);
}

void MoltWebAISitesHandler::OnSitesChanged() {
  if (IsJavascriptAllowed()) {
    FireWebUIListener("web-ai-sites-changed");
  }
}

base::DictValue MoltWebAISitesHandler::BuildState(bool enabled) {
  WebAISitePermissions* permissions = Permissions();
  base::DictValue state;
  state.Set("enabled", enabled);
  state.Set("available", permissions != nullptr);
  state.Set("persistent", permissions && permissions->persistent());
  base::ListValue sites;
  if (permissions) {
    for (const SiteListEntry& entry : permissions->List()) {
      base::DictValue row;
      row.Set("site", entry.site);
      row.Set("display", DisplayName(entry.site));
      row.Set("kind", KindName(entry.kind));
      row.Set("since", static_cast<double>(entry.since_ms));
      row.Set("until", static_cast<double>(entry.until_ms));
      sites.Append(std::move(row));
    }
  }
  state.Set("sites", std::move(sites));
  return state;
}

void MoltWebAISitesHandler::ResolveWithState(
    std::string callback_id,
    std::optional<std::string> error) {
  // On settings.json's own sequence: after any change posted before.
  molt_ai::MoltSettingsFileTaskRunner()->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&molt_ai::web_ai::ReadWebAIApisSwitch),
      base::BindOnce(&MoltWebAISitesHandler::OnSwitchRead,
                     weak_factory_.GetWeakPtr(), std::move(callback_id),
                     std::move(error)));
}

void MoltWebAISitesHandler::OnSwitchRead(std::string callback_id,
                                         std::optional<std::string> error,
                                         bool enabled) {
  if (!IsJavascriptAllowed()) {
    return;
  }
  base::DictValue state = BuildState(enabled);
  if (error) {
    state.Set("error", *error);
  }
  ResolveJavascriptCallback(base::Value(std::move(callback_id)),
                            base::Value(std::move(state)));
}

void MoltWebAISitesHandler::HandleGetState(const base::ListValue& args) {
  if (!HasCallbackArgs(args, 1u, __func__)) {
    return;
  }
  AllowJavascript();
  ResolveWithState(args[0].GetString(), std::nullopt);
}

void MoltWebAISitesHandler::HandleRemove(const base::ListValue& args) {
  if (!HasCallbackArgs(args, 2u, __func__)) {
    return;
  }
  AllowJavascript();
  // Only a key the state listed matches; anything else changes nothing.
  const std::optional<std::string> site =
      BoundedStringArg(args, 1, molt_ai::web_ai::kMaxSiteKeyBytes);
  WebAISitePermissions* permissions = Permissions();
  if (site && permissions && molt_ai::web_ai::IsPlausibleSiteKey(*site)) {
    permissions->Remove(*site);
  }
  ResolveWithState(args[0].GetString(), std::nullopt);
}

void MoltWebAISitesHandler::HandleClear(const base::ListValue& args) {
  if (!HasCallbackArgs(args, 1u, __func__)) {
    return;
  }
  AllowJavascript();
  if (WebAISitePermissions* permissions = Permissions()) {
    permissions->RemoveAll();
  }
  ResolveWithState(args[0].GetString(), std::nullopt);
}

void MoltWebAISitesHandler::HandleSetEnabled(const base::ListValue& args) {
  if (!HasCallbackArgs(args, 2u, __func__)) {
    return;
  }
  AllowJavascript();
  std::string callback_id = args[0].GetString();
  const std::optional<bool> enabled = BoolArg(args, 1);
  if (!enabled) {
    ResolveWithState(std::move(callback_id), std::nullopt);
    return;
  }
  // settings.json's one writer (common/molt_settings_file.h).
  molt_ai::MoltSettingsFileTaskRunner()->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&molt_ai::web_ai::WriteWebAIApisSwitch, *enabled),
      base::BindOnce(
          &ApplySwitchWrite, *enabled,
          base::BindOnce(&MoltWebAISitesHandler::OnSwitchWritten,
                         weak_factory_.GetWeakPtr(), std::move(callback_id))));
}

void MoltWebAISitesHandler::OnSwitchWritten(std::string callback_id, bool ok) {
  if (!IsJavascriptAllowed()) {
    return;
  }
  ResolveWithState(
      std::move(callback_id),
      ok ? std::nullopt
         : std::optional<std::string>(
               "Couldn't save ~/.moltbrowser/settings.json (is it valid "
               "JSON?). Nothing was changed."));
}

// Embedded in app.js after the page's own script.
const char kWebAISitesSettingsJs[] = R"JS(
// ---- Sites using on-device AI (built-in AI APIs) ----
(function() {
  'use strict';
  var section = document.getElementById('webAiSection');
  if (!section) return;

  var LISTS = [
    {kind: 'allowed', id: 'webAiAllowed', empty: 'No sites allowed.'},
    {kind: 'blocked', id: 'webAiBlocked', empty: 'No sites blocked.'},
    {kind: 'paused', id: 'webAiPaused', empty: 'None.'}
  ];

  function el(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined && text !== null) e.textContent = String(text);
    return e;
  }
  function fmtDate(ms) {
    var n = Number(ms);
    return n > 0 ? new Date(n).toLocaleDateString() : '';
  }

  function render(s) {
    if (!s || typeof s !== 'object') return;
    var sites = Array.isArray(s.sites) ? s.sites : [];
    var toggle = document.getElementById('webAiEnabled');
    toggle.checked = s.enabled !== false;
    var status = document.getElementById('webAiStatus');
    var said = s.error ? String(s.error) : '';
    if (status.textContent !== said) status.textContent = said;
    document.getElementById('webAiMemoryNote').style.display =
        s.available !== false && s.persistent === false ? '' : 'none';
    LISTS.forEach(function(list) {
      var box = document.getElementById(list.id);
      box.replaceChildren();
      var rows = sites.filter(function(r) {
        return r && typeof r === 'object' && r.kind === list.kind &&
               typeof r.site === 'string';
      });
      if (!rows.length) {
        box.appendChild(el('div', 'desc', list.empty));
        return;
      }
      rows.forEach(function(r) {
        var name = String(r.display || r.site);
        var row = el('div', 'web-ai-site');
        row.style.cssText =
            'display:flex;gap:10px;align-items:center;margin-top:6px';
        var label = el('span', null, name);
        label.title = r.site;
        row.appendChild(label);
        if (list.kind === 'paused' && r.until) {
          row.appendChild(el('span', 'desc', 'can ask again ' +
                                             fmtDate(r.until)));
        }
        var remove = el('button', 'btn secondary', 'Remove');
        remove.type = 'button';
        remove.style.cssText = 'font-size:.75rem;padding:4px 12px;' +
                               'margin-left:auto';
        remove.setAttribute('data-site', r.site);
        remove.setAttribute('aria-label', 'Remove ' + name);
        remove.setAttribute('data-on-click', 'webAiRemoveSite');
        row.appendChild(remove);
        box.appendChild(row);
      });
    });
    document.getElementById('webAiClearBtn').disabled = !sites.length;
  }

  function call() {
    return sendWithPromise.apply(null, arguments).then(render)
        .catch(function() { showToast('Couldn\'t update the site list'); });
  }

  moltActions({
    webAiRemoveSite: function() {
      call('webAiSitesRemove', this.getAttribute('data-site') || '');
    },
    webAiClearSites: function() {
      call('webAiSitesClear').then(function() {
        showToast('Sites will ask again');
      });
    },
    webAiSetEnabled: function() {
      call('webAiSitesSetEnabled', !!this.checked);
    }
  });

  cr.addWebUIListener('web-ai-sites-changed', function() {
    call('webAiSitesGetState');
  });
  call('webAiSitesGetState');
})();
)JS";
