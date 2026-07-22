// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/webui/molt_ai/molt_net_handler.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "base/check_op.h"
#include "base/no_destructor.h"
#include "base/functional/bind.h"
#include "base/strings/string_util.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/molt_ai/tor/molt_net_routing.h"
#include "chrome/browser/molt_ai/tor/tor_manager.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"

namespace {

// Display names for the curated exit-country codes. Mirrors the copy in
// molt_ai_chat_handler_tor.cc (kept file-local there); keep in sync with
// TorManager::GetAvailableExitCountries(). Unknown codes render as their
// uppercased ISO code so the picker never shows a blank row.
std::string ExitCountryDisplayName(const std::string& cc) {
  static const auto* const kNames = new std::map<std::string, std::string>{
      {"us", "United States"}, {"gb", "United Kingdom"},
      {"de", "Germany"},       {"fr", "France"},
      {"nl", "Netherlands"},   {"ch", "Switzerland"},
      {"se", "Sweden"},        {"no", "Norway"},
      {"fi", "Finland"},       {"ca", "Canada"},
      {"jp", "Japan"},         {"sg", "Singapore"},
      {"au", "Australia"},     {"es", "Spain"},
      {"it", "Italy"},         {"at", "Austria"},
      {"pl", "Poland"},        {"cz", "Czechia"},
      {"ro", "Romania"},       {"is", "Iceland"},
  };
  auto it = kNames->find(cc);
  if (it != kNames->end()) {
    return it->second;
  }
  return base::ToUpperASCII(cc);
}

// Last-picked "on" routing mode ("proxy" | "multi_hop"), session-persistent.
// The Tor backend routes identically for both (mirrors the settings page,
// molt_ai_settings_ui.cc HandleMoltnetConnect); the distinction is presented
// in the UI. "direct" is derived from the not-running state. NoDestructor
// avoids an exit-time destructor on the mutable global string.
std::string& OnMode() {
  static base::NoDestructor<std::string> mode("multi_hop");
  return *mode;
}

// The single status shape every mutating call resolves, so the page can
// re-render from one payload:
// { running: bool, selected: "<lc cc or ''>", mode: "direct|proxy|multi_hop" }.
base::DictValue StatusDict() {
  molt_ai::tor::TorManager* mgr = molt_ai::tor::TorManager::Get();
  const bool running = mgr->IsRunning();
  base::DictValue d;
  d.Set("running", running);
  d.Set("selected", mgr->GetExitCountry());
  d.Set("mode", running ? OnMode() : std::string("direct"));
  return d;
}

}  // namespace

MoltNetHandler::MoltNetHandler() = default;
MoltNetHandler::~MoltNetHandler() = default;

Profile* MoltNetHandler::GetProfile() {
  content::WebContents* wc = web_ui()->GetWebContents();
  return wc ? Profile::FromBrowserContext(wc->GetBrowserContext()) : nullptr;
}

void MoltNetHandler::ApplyRoutingForCurrentTorState() {
  Profile* profile = GetProfile();
  if (!profile) {
    return;
  }
  PrefService* local_state = g_browser_process->local_state();
  if (molt_ai::tor::TorManager::Get()->IsRunning()) {
    molt_ai::tor::MoltNetRouting::Enable(profile, local_state);
  } else {
    molt_ai::tor::MoltNetRouting::Disable(profile, local_state);
  }
}

void MoltNetHandler::ApplyRoutingForLaunchResult(bool tor_ready) {
  Profile* profile = GetProfile();
  PrefService* local_state = g_browser_process->local_state();
  if (tor_ready) {
    if (profile) {
      molt_ai::tor::MoltNetRouting::Enable(profile, local_state);
    }
    return;
  }
  // Tor came up on the control port but never built a usable circuit (stuck
  // bootstrap / network blocking Tor). Do NOT route through a daemon that
  // can't carry traffic — tear it down and keep the browser on a direct
  // connection so the user isn't stranded behind a dead proxy.
  molt_ai::tor::TorManager::Get()->Stop();
  if (profile) {
    molt_ai::tor::MoltNetRouting::Disable(profile, local_state);
  }
}

void MoltNetHandler::RegisterMessages() {
  web_ui()->RegisterMessageCallback(
      "moltnet.getStatus",
      base::BindRepeating(&MoltNetHandler::HandleGetStatus,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "moltnet.getExitCountries",
      base::BindRepeating(&MoltNetHandler::HandleGetExitCountries,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "moltnet.setExitCountry",
      base::BindRepeating(&MoltNetHandler::HandleSetExitCountry,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "moltnet.toggle",
      base::BindRepeating(&MoltNetHandler::HandleToggle,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "moltnet.setMode",
      base::BindRepeating(&MoltNetHandler::HandleSetMode,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "moltnet.newIdentity",
      base::BindRepeating(&MoltNetHandler::HandleNewIdentity,
                          base::Unretained(this)));
}

void MoltNetHandler::HandleGetStatus(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 1u);
  const std::string callback_id = args[0].GetString();
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(StatusDict()));
}

void MoltNetHandler::HandleGetExitCountries(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 1u);
  const std::string callback_id = args[0].GetString();

  molt_ai::tor::TorManager* mgr = molt_ai::tor::TorManager::Get();
  base::DictValue out;
  out.Set("selected", mgr->GetExitCountry());
  base::ListValue available;
  for (const std::string& cc : mgr->GetAvailableExitCountries()) {
    base::DictValue entry;
    entry.Set("code", cc);
    entry.Set("name", ExitCountryDisplayName(cc));
    available.Append(std::move(entry));
  }
  out.Set("available", std::move(available));
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}

void MoltNetHandler::HandleSetExitCountry(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 1u);
  const std::string callback_id = args[0].GetString();
  // Arg 1 is a lowercase ISO alpha-2 code, or "" for Auto/any.
  std::string cc = (args.size() > 1 && args[1].is_string())
                       ? base::ToLowerASCII(args[1].GetString())
                       : std::string();
  molt_ai::tor::TorManager::Get()->SetExitCountry(cc);
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(StatusDict()));
}

void MoltNetHandler::HandleToggle(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 1u);
  const std::string callback_id = args[0].GetString();
  molt_ai::tor::TorManager* mgr = molt_ai::tor::TorManager::Get();
  if (mgr->IsRunning()) {
    mgr->Stop();
    // Tor is down now -> restore direct networking before we hand control
    // back, so no window is left pointing at a stopped proxy.
    ApplyRoutingForCurrentTorState();
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(StatusDict()));
    return;
  }
  // Launch is async; its callback fires only once Tor has a usable circuit
  // (result.success) or the bootstrap timed out. Route only on success.
  mgr->Launch(base::BindOnce(
      [](base::WeakPtr<MoltNetHandler> self, std::string cb,
         molt_ai::tor::TorLaunchResult result) {
        if (!self) {
          return;
        }
        self->ApplyRoutingForLaunchResult(result.success);
        self->ResolveJavascriptCallback(base::Value(cb),
                                        base::Value(StatusDict()));
      },
      weak_ptr_factory_.GetWeakPtr(), callback_id));
}

void MoltNetHandler::HandleSetMode(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 1u);
  const std::string callback_id = args[0].GetString();
  std::string mode = (args.size() > 1 && args[1].is_string())
                         ? args[1].GetString()
                         : std::string("multi_hop");
  molt_ai::tor::TorManager* mgr = molt_ai::tor::TorManager::Get();

  // "direct" = no privacy routing → stop any managed Tor (mirrors the settings
  // page). "proxy" and "multi_hop" both route through Tor; we remember which
  // the user picked so the UI shows it, but the backend path is identical.
  if (mode == "direct") {
    mgr->Stop();
    ApplyRoutingForCurrentTorState();  // Tor down -> restore direct.
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(StatusDict()));
    return;
  }
  OnMode() = (mode == "proxy") ? "proxy" : "multi_hop";
  // Always go through Launch (even if a child is already running): its
  // callback re-confirms Tor has a usable circuit before we apply routing,
  // so a mode change during bootstrap can't enable a dead proxy.
  mgr->Launch(base::BindOnce(
      [](base::WeakPtr<MoltNetHandler> self, std::string cb,
         molt_ai::tor::TorLaunchResult result) {
        if (!self) {
          return;
        }
        self->ApplyRoutingForLaunchResult(result.success);
        self->ResolveJavascriptCallback(base::Value(cb),
                                        base::Value(StatusDict()));
      },
      weak_ptr_factory_.GetWeakPtr(), callback_id));
}

void MoltNetHandler::HandleNewIdentity(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 1u);
  const std::string callback_id = args[0].GetString();
  molt_ai::tor::TorManager* mgr = molt_ai::tor::TorManager::Get();
  // Re-applying the current exit country rewrites the torrc and, if Tor is
  // running, reloads it in place to rebuild circuits — the sanctioned NEWNYM
  // path the toolbar menu uses.
  mgr->SetExitCountry(mgr->GetExitCountry());
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(StatusDict()));
}
