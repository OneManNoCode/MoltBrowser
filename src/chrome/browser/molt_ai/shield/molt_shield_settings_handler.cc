// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/shield/molt_shield_settings_handler.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "chrome/browser/molt_ai/shield/molt_shield_request_utils.h"
#include "chrome/browser/molt_ai/shield/molt_shield_service.h"
#include "chrome/browser/molt_ai/shield/molt_shield_site_settings.h"
#include "chrome/browser/molt_ai/shield/molt_shield_store.h"
#include "chrome/browser/molt_ai/shield/molt_shield_tab_helper.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/molt_ai/shield/molt_shield_features.h"
#include "chrome/common/webui_url_constants.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/reload_type.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"

namespace molt_ai::shield {

namespace {

constexpr size_t kMaxCallbackIdLength = 256;
constexpr size_t kMaxArgLength = 2048;

// chrome.send() arguments come from the WebUI renderer: validate instead of
// CHECK-failing on a malformed message.
bool HasCallbackArgs(const base::ListValue& args, size_t min_size) {
  if (args.size() >= std::max<size_t>(min_size, 1) && args[0].is_string() &&
      args[0].GetString().size() <= kMaxCallbackIdLength) {
    return true;
  }
  LOG(WARNING) << "[MoltShield] dropping malformed settings message";
  return false;
}

std::optional<bool> BoolArg(const base::ListValue& args, size_t i) {
  return i < args.size() && args[i].is_bool()
             ? std::optional<bool>(args[i].GetBool())
             : std::nullopt;
}

std::optional<std::string> StringArg(const base::ListValue& args, size_t i) {
  return i < args.size() && args[i].is_string() &&
                 args[i].GetString().size() <= kMaxArgLength
             ? std::optional<std::string>(args[i].GetString())
             : std::nullopt;
}

double ToJsTime(base::Time t) {
  return t.is_null() ? 0 : t.InMillisecondsFSinceUnixEpochIgnoringNull();
}

std::string SiteOf(const GURL& url) {
  std::string site = net::registry_controlled_domains::GetDomainAndRegistry(
      url, net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);
  return site.empty() ? std::string(url.host()) : site;
}

}  // namespace

MoltShieldSettingsHandler::MoltShieldSettingsHandler() = default;
MoltShieldSettingsHandler::~MoltShieldSettingsHandler() = default;

void MoltShieldSettingsHandler::RegisterMessages() {
  web_ui()->RegisterMessageCallback(
      "shieldGetState",
      base::BindRepeating(&MoltShieldSettingsHandler::HandleGetState,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "shieldSetGlobal",
      base::BindRepeating(&MoltShieldSettingsHandler::HandleSetGlobal,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "shieldSetListEnabled",
      base::BindRepeating(&MoltShieldSettingsHandler::HandleSetListEnabled,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "shieldUpdateNow",
      base::BindRepeating(&MoltShieldSettingsHandler::HandleUpdateNow,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "shieldSetTabShields",
      base::BindRepeating(&MoltShieldSettingsHandler::HandleSetTabShields,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "shieldRemoveException",
      base::BindRepeating(&MoltShieldSettingsHandler::HandleRemoveException,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "shieldReportBreakage",
      base::BindRepeating(&MoltShieldSettingsHandler::HandleReportBreakage,
                          base::Unretained(this)));
}

base::DictValue MoltShieldSettingsHandler::BuildState() {
  MoltShieldService* service = MoltShieldService::Get();
  Profile* profile = Profile::FromWebUI(web_ui());
  const ShieldSettings& settings = service->settings();

  base::DictValue state;
  state.Set("enabled", settings.enabled);
  state.Set("cosmetic", settings.cosmetic);
  state.Set("consentKiller", settings.consent_killer);
  state.Set("ready", service->IsReady());
  state.Set("updating", service->IsUpdating());
  state.Set("lastUpdateCheck", ToJsTime(service->last_update_check()));
  state.Set("engine", service->EngineVersion());

  // Runtime kill switches (molt_shield_features.h).
  state.Set("disabledByCommandLine",
            molt_shield::IsMoltShieldDisabledByCommandLine());
  base::ListValue hooks_off;
  for (std::string_view flag : molt_shield::FlagNamesOfDisabledHooks()) {
    hooks_off.Append(flag);
  }
  state.Set("hooksOff", std::move(hooks_off));

  const ShieldStats stats = service->GetStats();
  base::DictValue s;
  s.Set("checked", static_cast<double>(stats.total_requests_checked));
  s.Set("blocked", static_cast<double>(stats.trackers_blocked));
  s.Set("ads", static_cast<double>(stats.ads_blocked));
  s.Set("redirects", static_cast<double>(stats.redirects_served));
  s.Set("websockets", static_cast<double>(stats.websockets_blocked));
  s.Set("cosmetic", static_cast<double>(stats.cosmetic_rules_applied));
  s.Set("scriptlets", static_cast<double>(stats.scriptlets_injected));
  state.Set("stats", std::move(s));

  base::ListValue lists;
  for (const ListStatus& l : service->GetListStatus()) {
    base::DictValue d;
    d.Set("id", l.id);
    d.Set("title", l.title);
    d.Set("enabled", l.enabled);
    d.Set("cached", l.cached);
    d.Set("rules", static_cast<int>(l.rule_lines));
    d.Set("version", l.version);
    d.Set("fetchedAt", ToJsTime(l.fetched_at));
    d.Set("checkedAt", ToJsTime(l.checked_at));
    d.Set("error", l.last_error);
    lists.Append(std::move(d));
  }
  state.Set("lists", std::move(lists));

  // Open tabs of this profile (not Incognito ones) with a web page.
  base::ListValue tabs;
  for (MoltShieldTabHelper* helper : MoltShieldTabHelper::GetAll()) {
    content::WebContents* wc = helper->web_contents();
    if (!wc || wc->GetBrowserContext() != profile) {
      continue;
    }
    const GURL& url = wc->GetLastCommittedURL();
    if (!url.SchemeIsHTTPOrHTTPS()) {
      continue;
    }
    base::DictValue t;
    t.Set("key", wc->GetPrimaryMainFrame()->GetFrameTreeNodeId().value());
    t.Set("site", SiteOf(url));
    t.Set("title", wc->GetTitle());
    t.Set("blocked", static_cast<double>(helper->GetBlockedCount()));
    t.Set("shieldsUp", AreShieldsUpForSite(profile, url));
    tabs.Append(std::move(t));
  }
  state.Set("tabs", std::move(tabs));

  base::ListValue exceptions;
  for (const std::string& pattern : GetShieldsDownSites(profile)) {
    exceptions.Append(pattern);
  }
  state.Set("exceptions", std::move(exceptions));
  return state;
}

void MoltShieldSettingsHandler::ResolveWithState(
    const base::Value& callback_id) {
  ResolveJavascriptCallback(callback_id, BuildState());
}

void MoltShieldSettingsHandler::HandleGetState(const base::ListValue& args) {
  if (!HasCallbackArgs(args, 1)) {
    return;
  }
  AllowJavascript();
  ResolveWithState(args[0]);
}

void MoltShieldSettingsHandler::HandleSetGlobal(const base::ListValue& args) {
  if (!HasCallbackArgs(args, 3)) {
    return;
  }
  AllowJavascript();
  std::optional<std::string> key = StringArg(args, 1);
  std::optional<bool> value = BoolArg(args, 2);
  MoltShieldService* service = MoltShieldService::Get();
  if (key && value) {
    if (*key == "enabled") {
      service->SetEnabled(*value);
    } else if (*key == "cosmetic") {
      service->SetCosmeticEnabled(*value);
    } else if (*key == "consentKiller") {
      service->SetConsentKillerEnabled(*value);
    }
  }
  ResolveWithState(args[0]);
}

void MoltShieldSettingsHandler::HandleSetListEnabled(
    const base::ListValue& args) {
  if (!HasCallbackArgs(args, 3)) {
    return;
  }
  AllowJavascript();
  std::optional<std::string> id = StringArg(args, 1);
  std::optional<bool> value = BoolArg(args, 2);
  if (id && value) {
    MoltShieldService::Get()->SetListEnabled(*id, *value);
  }
  ResolveWithState(args[0]);
}

void MoltShieldSettingsHandler::HandleUpdateNow(const base::ListValue& args) {
  if (!HasCallbackArgs(args, 1)) {
    return;
  }
  AllowJavascript();
  MoltShieldService::Get()->UpdateListsNow();
  ResolveWithState(args[0]);
}

void MoltShieldSettingsHandler::HandleSetTabShields(
    const base::ListValue& args) {
  if (!HasCallbackArgs(args, 3)) {
    return;
  }
  AllowJavascript();
  std::optional<bool> up = BoolArg(args, 2);
  if (args[1].is_int() && up) {
    content::WebContents* wc = content::WebContents::FromFrameTreeNodeId(
        content::FrameTreeNodeId(args[1].GetInt()));
    Profile* profile = Profile::FromWebUI(web_ui());
    if (wc && wc->GetBrowserContext() == profile) {
      SetShieldsUpForSite(profile, wc->GetLastCommittedURL(), *up);
      // Like Brave: apply the new setting by reloading the page.
      wc->GetController().Reload(content::ReloadType::NORMAL,
                                 /*check_for_repost=*/true);
    }
  }
  ResolveWithState(args[0]);
}

void MoltShieldSettingsHandler::HandleRemoveException(
    const base::ListValue& args) {
  if (!HasCallbackArgs(args, 2)) {
    return;
  }
  AllowJavascript();
  if (std::optional<std::string> pattern = StringArg(args, 1)) {
    RemoveShieldsDownException(Profile::FromWebUI(web_ui()), *pattern);
  }
  ResolveWithState(args[0]);
}

void MoltShieldSettingsHandler::HandleReportBreakage(
    const base::ListValue& args) {
  if (!HasCallbackArgs(args, 2)) {
    return;
  }
  AllowJavascript();
  base::DictValue result;
  content::WebContents* wc =
      args[1].is_int() ? content::WebContents::FromFrameTreeNodeId(
                             content::FrameTreeNodeId(args[1].GetInt()))
                       : nullptr;
  Profile* profile = Profile::FromWebUI(web_ui());
  if (wc && wc->GetBrowserContext() == profile) {
    MoltShieldTabHelper* helper = MoltShieldTabHelper::FromWebContents(wc);
    MoltShieldService* service = MoltShieldService::Get();
    const GURL& url = wc->GetLastCommittedURL();
    result.Set("report",
               BuildBreakageReport(url, AreShieldsUpForSite(profile, url),
                                   helper ? helper->GetBlockedCount() : 0,
                                   service->GetListStatus(),
                                   chrome::kMoltBrowserVersion,
                                   service->EngineVersion()));
  }
  ResolveJavascriptCallback(args[0], result);
}

// Embedded in app.js after the page's own script.
const char kMoltShieldSettingsJs[] = R"JS(
// ---- MoltShield (ad & tracker blocker) section ----
(function() {
  'use strict';
  var section = document.getElementById('shieldSection');
  if (!section) return;

  function el(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined && text !== null) e.textContent = String(text);
    return e;
  }
  function fmtTime(ms) {
    if (!ms) return 'never';
    var d = new Date(ms);
    return d.toLocaleDateString() + ' ' + d.toLocaleTimeString();
  }
  function fmtNum(n) {
    return Number(n || 0).toLocaleString();
  }
  function toggleRow(label, checked, onChange) {
    var wrap = el('label', 'toggle');
    var input = document.createElement('input');
    input.type = 'checkbox';
    input.checked = !!checked;
    input.addEventListener('change', function() { onChange(input.checked); });
    wrap.appendChild(input);
    wrap.appendChild(el('span', 'track'));
    wrap.appendChild(el('span', 'label', label));
    return wrap;
  }

  var state = null;
  function call(method) {
    var args = Array.prototype.slice.call(arguments);
    return sendWithPromise.apply(null, args).then(render).catch(function() {
      showToast('MoltShield: request failed');
    });
  }

  function renderKillSwitches(box) {
    var text = '';
    if (state.disabledByCommandLine) {
      text = 'MoltShield is off for this run: the browser was started with ' +
             '--disable-molt-shield. Its settings can be changed again ' +
             'after a restart without that switch.';
    } else if (state.hooksOff && state.hooksOff.length) {
      text = 'Parts of MoltShield are turned off in chrome://flags (' +
             state.hooksOff.join(', ') + '). The switches below do not ' +
             'turn them back on.';
    }
    if (text) box.appendChild(el('div', 'desc shield-killswitch', text));
  }

  function renderGlobals() {
    var box = document.getElementById('shieldGlobals');
    box.replaceChildren();
    renderKillSwitches(box);
    var rows = [
      ['enabled', 'Block ads & trackers (all sites)', state.enabled],
      ['cosmetic', 'Hide ad elements and run anti-ad scriptlets',
       state.cosmetic],
      ['consentKiller', 'Auto-reject cookie consent banners',
       state.consentKiller],
    ];
    rows.forEach(function(r) {
      var f = el('div', 'field');
      f.appendChild(toggleRow(r[1], r[2], function(v) {
        call('shieldSetGlobal', r[0], v);
      }));
      box.appendChild(f);
    });
  }

  function renderStats() {
    var s = state.stats || {};
    document.getElementById('shieldStats').textContent =
        'This session: ' + fmtNum(s.blocked) + ' requests blocked of ' +
        fmtNum(s.checked) + ' checked (' + fmtNum(s.ads) + ' ads, ' +
        fmtNum(s.redirects) + ' neutered with a stub, ' +
        fmtNum(s.websockets) + ' WebSockets); cosmetic filters on ' +
        fmtNum(s.cosmetic) + ' pages, scriptlets on ' +
        fmtNum(s.scriptlets) + '.';
  }

  function renderLists() {
    var box = document.getElementById('shieldLists');
    box.replaceChildren();
    (state.lists || []).forEach(function(l) {
      var row = el('div', 'field');
      row.appendChild(toggleRow(l.title, l.enabled, function(v) {
        call('shieldSetListEnabled', l.id, v);
      }));
      var info = l.cached
          ? (fmtNum(l.rules) + ' filters' +
             (l.version ? ' · version ' + l.version : '') +
             ' · updated ' + fmtTime(l.fetchedAt))
          : (l.enabled ? 'not downloaded yet' : 'off');
      if (l.error) info += ' · last update failed: ' + l.error;
      row.appendChild(el('div', 'desc', info));
      box.appendChild(row);
    });
    document.getElementById('shieldUpdateStatus').textContent =
        (state.updating ? 'Updating… ' : '') + 'Last check: ' +
        fmtTime(state.lastUpdateCheck) + ' · engine ' + state.engine +
        (state.disabledByCommandLine ? ' (not loaded: --disable-molt-shield)'
                                     : (state.ready ? '' : ' (loading)'));
  }

  function renderTabs() {
    var box = document.getElementById('shieldTabs');
    box.replaceChildren();
    var tabs = state.tabs || [];
    if (!tabs.length) {
      box.appendChild(el('div', 'desc', 'No open web pages.'));
      return;
    }
    tabs.forEach(function(t) {
      var row = el('div', 'field');
      row.appendChild(el('div', null, t.site + ' — ' + fmtNum(t.blocked) +
                                          ' blocked'));
      row.appendChild(el('div', 'desc', t.title));
      var controls = el('div');
      controls.style.cssText = 'display:flex;gap:10px;align-items:center;' +
                               'margin-top:6px';
      controls.appendChild(toggleRow('Shields up on ' + t.site, t.shieldsUp,
                                     function(v) {
        call('shieldSetTabShields', t.key, v);
      }));
      var report = el('button', 'btn secondary', 'Report broken site');
      report.style.cssText = 'font-size:12px;padding:6px 14px';
      report.addEventListener('click', function() {
        sendWithPromise('shieldReportBreakage', t.key).then(function(r) {
          if (!r || !r.report) return;
          var out = document.getElementById('shieldReport');
          out.value = r.report;
          out.style.display = 'block';
          if (navigator.clipboard) {
            navigator.clipboard.writeText(r.report).then(function() {
              showToast('Report copied (no URLs or personal data)');
            }, function() { out.select(); });
          } else {
            out.select();
          }
        });
      });
      controls.appendChild(report);
      row.appendChild(controls);
      box.appendChild(row);
    });
  }

  function renderExceptions() {
    var box = document.getElementById('shieldExceptions');
    box.replaceChildren();
    var ex = state.exceptions || [];
    if (!ex.length) {
      box.appendChild(el('div', 'desc', 'Shields are up on every site.'));
      return;
    }
    ex.forEach(function(pattern) {
      var row = el('div', 'field');
      row.style.cssText = 'display:flex;gap:10px;align-items:center';
      row.appendChild(el('span', null, pattern));
      var b = el('button', 'btn secondary', 'Shields up');
      b.style.cssText = 'font-size:12px;padding:4px 12px';
      b.addEventListener('click', function() {
        call('shieldRemoveException', pattern);
      });
      row.appendChild(b);
      box.appendChild(row);
    });
  }

  function render(s) {
    if (!s) return;
    state = s;
    renderGlobals();
    renderStats();
    renderLists();
    renderTabs();
    renderExceptions();
    if (state.disabledByCommandLine) {
      // The browser ignores changes this run; say so instead of pretending.
      section.querySelectorAll('input, button').forEach(function(c) {
        c.disabled = true;
      });
    }
  }

  document.getElementById('shieldUpdateBtn').addEventListener('click',
      function() { call('shieldUpdateNow'); });
  call('shieldGetState');
  // Counters and list status change in the background.
  setInterval(function() {
    if (document.visibilityState === 'visible') call('shieldGetState');
  }, 5000);
})();
)JS";

}  // namespace molt_ai::shield
