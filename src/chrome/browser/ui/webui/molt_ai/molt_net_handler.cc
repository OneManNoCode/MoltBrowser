// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/webui/molt_ai/molt_net_handler.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/molt_ai/tor/exit_country_names.h"
#include "chrome/browser/molt_ai/tor/molt_net_profile_id.h"
#include "chrome/browser/molt_ai/tor/molt_net_routing.h"
#include "chrome/browser/molt_ai/tor/molt_net_window.h"
#include "chrome/browser/molt_ai/tor/tor_manager.h"
#include "chrome/browser/molt_ai/tor/tor_service.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/webui/molt_ai/molt_ai_chat_handler_helpers.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "net/base/load_flags.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "ui/base/l10n/l10n_util.h"
#include "url/gurl.h"

namespace {

using molt_ai::chat_handler_helpers::HasCallbackArgs;
using molt_ai::tor::MoltNetRouting;

net::NetworkTrafficAnnotationTag ExitCheckAnnotation() {
  return net::DefineNetworkTrafficAnnotation("moltnet_exit_check", R"(
      semantics {
        sender: "MoltNet private-routing panel"
        description:
          "Fetches check.torproject.org/api/ip THROUGH the MoltNet (Tor) "
          "profile, so the panel can show the Tor exit IP that websites see "
          "and confirm the traffic really went through Tor. Never sent on "
          "the user's direct connection."
        trigger: "The user clicks Verify in the MoltNet route panel."
        data: "None. A plain HTTPS GET via Tor."
        destination: WEBSITE
      }
      policy {
        cookies_allowed: NO
        setting: "Only fires on an explicit Verify click."
      })");
}

std::string RoutingString(MoltNetRouting::RoutingState state) {
  switch (state) {
    case MoltNetRouting::RoutingState::kNotRouted:
      return "not_routed";
    case MoltNetRouting::RoutingState::kRoutedViaTor:
      return "tor";
    case MoltNetRouting::RoutingState::kBlockedTorDown:
      return "blocked";
    case MoltNetRouting::RoutingState::kProxyOverridden:
      return "overridden";
  }
  return "not_routed";
}

// The single status shape every call resolves with:
// { running, starting, selected, mode, routing, in_moltnet_window,
//   proxy_externally_controlled[, error] }.
// |routing| describes the window the popover is shown in, not the process:
// a regular window is "not_routed" even while Tor runs for MoltNet windows.
// |error| is set up front when there is no tor MoltNet can run, so the
// popover says what to do ("Install the tor package" on Linux) before the
// user tries.
base::DictValue StatusDict(Profile* profile) {
  molt_ai::tor::TorManager* mgr = molt_ai::tor::TorManager::Get();
  const MoltNetRouting::Status status = MoltNetRouting::GetStatus(profile);
  base::DictValue d;
  d.Set("running", mgr->IsRunning());
  d.Set("starting", mgr->IsStarting());
  d.Set("selected", mgr->GetExitCountry());
  d.Set("mode", mgr->IsRunning() ? "multi_hop" : "direct");
  d.Set("routing", RoutingString(status.state));
  d.Set("in_moltnet_window", molt_ai::tor::IsMoltNetProfile(profile));
  d.Set("proxy_externally_controlled", status.proxy_externally_controlled);
  if (!mgr->IsRunning() && !mgr->IsStarting()) {
    const std::string missing = mgr->FindTorBinary().error;
    if (!missing.empty()) {
      d.Set("error", missing);
    }
  }
  return d;
}

}  // namespace

MoltNetHandler::MoltNetHandler() = default;
MoltNetHandler::~MoltNetHandler() = default;

Profile* MoltNetHandler::GetProfile() {
  content::WebContents* wc = web_ui()->GetWebContents();
  return wc ? Profile::FromBrowserContext(wc->GetBrowserContext()) : nullptr;
}

void MoltNetHandler::ResolveWithStatus(const std::string& callback_id,
                                       const std::string& error) {
  base::DictValue d = StatusDict(GetProfile());
  if (!error.empty()) {
    d.Set("error", error);
  }
  ResolveJavascriptCallback(base::Value(callback_id), base::Value(std::move(d)));
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
      "moltnet.openWindow",
      base::BindRepeating(&MoltNetHandler::HandleOpenWindow,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "moltnet.newIdentity",
      base::BindRepeating(&MoltNetHandler::HandleNewIdentity,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "moltnet.getCircuit",
      base::BindRepeating(&MoltNetHandler::HandleGetCircuit,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "moltnet.verifyExit",
      base::BindRepeating(&MoltNetHandler::HandleVerifyExit,
                          base::Unretained(this)));
}

void MoltNetHandler::HandleGetStatus(const base::ListValue& args) {
  if (!HasCallbackArgs(args, 1u, __func__)) return;
  AllowJavascript();
  ResolveWithStatus(args[0].GetString(), std::string());
}

void MoltNetHandler::HandleGetExitCountries(const base::ListValue& args) {
  if (!HasCallbackArgs(args, 1u, __func__)) return;
  AllowJavascript();
  const std::string callback_id = args[0].GetString();

  molt_ai::tor::TorManager* mgr = molt_ai::tor::TorManager::Get();
  base::DictValue out;
  out.Set("selected", mgr->GetExitCountry());
  base::ListValue available;
  for (const std::string& cc : mgr->GetAvailableExitCountries()) {
    base::DictValue entry;
    entry.Set("code", cc);
    entry.Set("name", molt_ai::tor::ExitCountryDisplayName(cc));
    available.Append(std::move(entry));
  }
  out.Set("available", std::move(available));
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}

void MoltNetHandler::HandleSetExitCountry(const base::ListValue& args) {
  if (!HasCallbackArgs(args, 1u, __func__)) return;
  AllowJavascript();
  const std::string callback_id = args[0].GetString();
  // Arg 1 is a lowercase ISO alpha-2 code, or "" for Auto/any. Session-only.
  std::string cc = (args.size() > 1 && args[1].is_string())
                       ? base::ToLowerASCII(args[1].GetString())
                       : std::string();
  molt_ai::tor::TorManager::Get()->SetExitCountry(
      cc, base::BindOnce(
              [](base::WeakPtr<MoltNetHandler> self, std::string cb, bool ok,
                 std::string error) {
                if (self) {
                  self->ResolveWithStatus(cb, ok ? std::string() : error);
                }
              },
              weak_ptr_factory_.GetWeakPtr(), callback_id));
}

void MoltNetHandler::HandleToggle(const base::ListValue& args) {
  if (!HasCallbackArgs(args, 1u, __func__)) return;
  AllowJavascript();
  const std::string callback_id = args[0].GetString();
  molt_ai::tor::TorManager* mgr = molt_ai::tor::TorManager::Get();
  if (mgr->IsRunning() || mgr->IsStarting()) {
    // Closes MoltNet windows (possibly the one hosting this popover; window
    // closing is asynchronous) and stops Tor. Regular windows were never
    // proxied, so there is nothing to restore.
    molt_ai::tor::MoltNetWindow::TurnOff();
    ResolveWithStatus(callback_id, std::string());
    return;
  }
  HandleOpenWindow(args);
}

void MoltNetHandler::HandleSetMode(const base::ListValue& args) {
  if (!HasCallbackArgs(args, 1u, __func__)) return;
  AllowJavascript();
  const std::string mode = (args.size() > 1 && args[1].is_string())
                               ? args[1].GetString()
                               : std::string("multi_hop");
  if (mode == "direct") {
    molt_ai::tor::MoltNetWindow::TurnOff();
    ResolveWithStatus(args[0].GetString(), std::string());
    return;
  }
  HandleOpenWindow(args);
}

void MoltNetHandler::HandleOpenWindow(const base::ListValue& args) {
  if (!HasCallbackArgs(args, 1u, __func__)) return;
  AllowJavascript();
  const std::string callback_id = args[0].GetString();
  // Bootstrap can take a while; the page shows "Connecting…" meanwhile.
  molt_ai::tor::MoltNetWindow::Open(
      GetProfile(), GURL(),
      base::BindOnce(
          [](base::WeakPtr<MoltNetHandler> self, std::string cb, bool ok,
             std::string error) {
            if (self) {
              self->ResolveWithStatus(cb, ok ? std::string() : error);
            }
          },
          weak_ptr_factory_.GetWeakPtr(), callback_id));
}

void MoltNetHandler::HandleNewIdentity(const base::ListValue& args) {
  if (!HasCallbackArgs(args, 1u, __func__)) return;
  AllowJavascript();
  const std::string callback_id = args[0].GetString();
  // Closes the MoltNet windows (this popover may die with its window — the
  // weak pointer handles that), discards the MoltNet profile, NEWNYM, and
  // reopens a fresh MoltNet window.
  molt_ai::tor::MoltNetWindow::NewIdentity(
      GetProfile(),
      base::BindOnce(
          [](base::WeakPtr<MoltNetHandler> self, std::string cb, bool ok,
             std::string error) {
            if (self) {
              self->ResolveWithStatus(cb, ok ? std::string() : error);
            }
          },
          weak_ptr_factory_.GetWeakPtr(), callback_id));
}

void MoltNetHandler::HandleGetCircuit(const base::ListValue& args) {
  if (!HasCallbackArgs(args, 1u, __func__)) return;
  AllowJavascript();
  const std::string callback_id = args[0].GetString();

  if (!molt_ai::tor::TorManager::Get()->IsRunning()) {
    base::DictValue out;
    out.Set("connected", false);
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  molt_ai::tor::TorService::Get()->GetCircuitsEnriched(base::BindOnce(
      [](base::WeakPtr<MoltNetHandler> self, std::string cb,
         std::vector<molt_ai::tor::TorCircuit> circuits) {
        if (!self) {
          return;
        }
        // Prefer a BUILT general circuit; otherwise any built one.
        const molt_ai::tor::TorCircuit* active = nullptr;
        for (const auto& c : circuits) {
          if (c.state == "BUILT" && !c.hops.empty()) {
            active = &c;
            if (c.purpose == "GENERAL") {
              break;
            }
          }
        }
        base::DictValue out;
        out.Set("connected", true);
        base::ListValue hops;
        if (active) {
          const size_t n = active->hops.size();
          for (size_t i = 0; i < n; ++i) {
            const auto& h = active->hops[i];
            base::DictValue hop;
            hop.Set("role", i == 0 ? "guard"
                                   : (i + 1 == n ? "exit" : "middle"));
            hop.Set("country", base::ToUpperASCII(h.country));
            if (!h.country.empty()) {
              hop.Set("country_name",
                      base::UTF16ToUTF8(l10n_util::GetDisplayNameForCountry(
                          h.country, g_browser_process->GetApplicationLocale())));
            }
            hop.Set("ip", h.ip);
            hop.Set("nickname", h.nickname);
            // Public relay identity: anyone can look it up in the Tor
            // consensus (metrics.torproject.org / onionoo).
            hop.Set("fingerprint", h.fingerprint);
            hops.Append(std::move(hop));
          }
        }
        out.Set("hops", std::move(hops));
        self->ResolveJavascriptCallback(base::Value(cb),
                                        base::Value(std::move(out)));
      },
      weak_ptr_factory_.GetWeakPtr(), callback_id));
}

void MoltNetHandler::HandleVerifyExit(const base::ListValue& args) {
  if (!HasCallbackArgs(args, 1u, __func__)) return;
  AllowJavascript();
  const std::string callback_id = args[0].GetString();

  // Only ever through the MoltNet profile, and only once it is verifiably
  // routed via Tor — never on the user's direct connection.
  Profile* moltnet = MoltNetRouting::GetMoltNetProfile(GetProfile());
  if (!moltnet || verify_loader_ ||
      MoltNetRouting::GetStatus(moltnet).state !=
          MoltNetRouting::RoutingState::kRoutedViaTor) {
    base::DictValue out;
    out.Set("ok", false);
    out.Set("error", "Open a MoltNet window first.");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  scoped_refptr<network::SharedURLLoaderFactory> factory =
      moltnet->GetDefaultStoragePartition()
          ->GetURLLoaderFactoryForBrowserProcess();
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = GURL("https://check.torproject.org/api/ip");
  request->method = "GET";
  request->load_flags = net::LOAD_DO_NOT_SAVE_COOKIES;
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  verify_loader_ = network::SimpleURLLoader::Create(std::move(request),
                                                    ExitCheckAnnotation());
  verify_loader_->DownloadToString(
      factory.get(),
      base::BindOnce(&MoltNetHandler::OnVerifyExitDone,
                     weak_ptr_factory_.GetWeakPtr(), callback_id),
      /*max_body_size=*/4096);
}

void MoltNetHandler::OnVerifyExitDone(std::string callback_id,
                                      std::optional<std::string> body) {
  verify_loader_.reset();
  base::DictValue out;
  std::optional<base::Value> parsed =
      body ? base::JSONReader::Read(*body, base::JSON_PARSE_RFC) : std::nullopt;
  if (parsed && parsed->is_dict()) {
    const base::DictValue& d = parsed->GetDict();
    out.Set("ok", true);
    out.Set("is_tor", d.FindBool("IsTor").value_or(false));
    if (const std::string* ip = d.FindString("IP")) {
      out.Set("ip", *ip);
    }
  } else {
    out.Set("ok", false);
  }
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}
