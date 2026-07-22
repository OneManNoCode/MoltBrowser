// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/webui/molt_ai/molt_net_handler.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/check_op.h"
#include "base/no_destructor.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_reader.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/molt_ai/tor/molt_net_routing.h"
#include "chrome/browser/molt_ai/tor/tor_manager.h"
#include "chrome/browser/molt_ai/tor/tor_service.h"
#include "chrome/browser/net/system_network_context_manager.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/storage_partition.h"
#include "services/network/public/mojom/network_context.mojom.h"
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

// The user's real public IP, captured via a DIRECT fetch at the moment they
// start connecting — before the Tor proxy is installed — so the route panel can
// reveal it on demand without ever phoning out once traffic is on Tor. This
// pre-Tor request is indistinguishable from any IP-check and leaks nothing
// about Tor usage. Process-global (MoltNet is process-global).
std::string& CachedOriginIp() {
  static base::NoDestructor<std::string> ip;
  return *ip;
}

net::NetworkTrafficAnnotationTag IpCheckAnnotation() {
  return net::DefineNetworkTrafficAnnotation("moltnet_ip_check", R"(
      semantics {
        sender: "MoltNet private-routing panel"
        description:
          "Fetches the browser's public IP so the MoltNet route panel can "
          "show the user their own IP (before routing) and the Tor exit IP "
          "(through Tor). No user data is sent beyond the plain GET."
        trigger:
          "The user turns on MoltNet (real-IP capture, direct) or clicks "
          "Verify in the route panel (exit check, through Tor)."
        data: "None. A plain HTTPS GET; the response is the caller's own IP."
        destination: WEBSITE
      }
      policy {
        cookies_allowed: NO
        setting: "Only fires on MoltNet connect / an explicit Verify click."
      })");
}

// Kick a direct fetch of the user's public IP and cache it. Self-owning loader
// (kept alive by its own reply callback) so it survives the popover closing.
// Must be called while the profile is still on a DIRECT connection (i.e.
// before routing is applied) for the result to be the real origin IP.
void FetchOriginIp(Profile* profile) {
  if (!profile) {
    return;
  }
  scoped_refptr<network::SharedURLLoaderFactory> factory =
      profile->GetDefaultStoragePartition()
          ->GetURLLoaderFactoryForBrowserProcess();
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = GURL("https://api.ipify.org?format=json");
  request->method = "GET";
  request->load_flags = net::LOAD_DO_NOT_SAVE_COOKIES;
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  auto loader =
      network::SimpleURLLoader::Create(std::move(request), IpCheckAnnotation());
  network::SimpleURLLoader* raw = loader.get();
  raw->DownloadToString(
      factory.get(),
      base::BindOnce(
          [](std::unique_ptr<network::SimpleURLLoader> owned,
             std::optional<std::string> body) {
            if (!body) {
              return;
            }
            std::optional<base::Value> v = base::JSONReader::Read(*body, base::JSON_PARSE_RFC);
            if (v && v->is_dict()) {
              if (const std::string* ip = v->GetDict().FindString("ip")) {
                CachedOriginIp() = *ip;
              }
            }
          },
          std::move(loader)),
      /*max_body_size=*/4096);
}

// Close every existing socket on the profile AND system network contexts, so
// connections opened DIRECT before routing turned on don't linger off-Tor —
// they re-establish through the SOCKS proxy. Without this there's a transient
// window where a packet capture right after Connect would still see pre-enable
// keep-alive connections egressing from the real IP.
void CloseDirectConnections(Profile* profile) {
  if (profile) {
    if (network::mojom::NetworkContext* nc =
            profile->GetDefaultStoragePartition()->GetNetworkContext()) {
      nc->CloseAllConnections(base::DoNothing());
    }
  }
  if (SystemNetworkContextManager* sys =
          g_browser_process->system_network_context_manager()) {
    if (network::mojom::NetworkContext* nc = sys->GetContext()) {
      nc->CloseAllConnections(base::DoNothing());
    }
  }
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
      // Drop any sockets opened before routing so nothing lingers off-Tor.
      CloseDirectConnections(profile);
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
  // Capture the real origin IP now, while still on a direct connection (the
  // proxy is installed only in the Launch success callback below), so the
  // route panel can reveal it without phoning out once we're on Tor.
  FetchOriginIp(GetProfile());
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
  // Capture the real origin IP while still direct (only when we're actually
  // starting Tor — if it's already running the connection is already proxied,
  // which would capture the exit IP instead of the real one).
  if (!mgr->IsRunning()) {
    FetchOriginIp(GetProfile());
  }
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

void MoltNetHandler::HandleGetCircuit(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 1u);
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
        out.Set("origin_ip", CachedOriginIp());
        base::ListValue hops;
        if (active) {
          const size_t n = active->hops.size();
          for (size_t i = 0; i < n; ++i) {
            const auto& h = active->hops[i];
            base::DictValue hop;
            hop.Set("role", i == 0 ? "guard"
                                   : (i + 1 == n ? "exit" : "middle"));
            hop.Set("country", base::ToUpperASCII(h.country));
            // Full localized country name (e.g. "DK" -> "Denmark") for every
            // hop, not just a flag + ISO code, so the route reads clearly.
            if (!h.country.empty()) {
              hop.Set("country_name",
                      base::UTF16ToUTF8(l10n_util::GetDisplayNameForCountry(
                          h.country, g_browser_process->GetApplicationLocale())));
            }
            hop.Set("ip", h.ip);
            hop.Set("nickname", h.nickname);
            // Relay fingerprint = the public identity anyone can look up in the
            // Tor consensus (metrics.torproject.org / onionoo) to independently
            // confirm this is a real relay in the country/role we claim.
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
  AllowJavascript();
  CHECK_GE(args.size(), 1u);
  const std::string callback_id = args[0].GetString();

  Profile* profile = GetProfile();
  if (!profile || verify_loader_) {
    base::DictValue out;
    out.Set("ok", false);
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  // Fetched through the PROFILE factory — which is proxied through Tor while
  // MoltNet is on — so the reported IP is exactly what websites see, and IsTor
  // independently confirms the traffic went through the Tor network.
  scoped_refptr<network::SharedURLLoaderFactory> factory =
      profile->GetDefaultStoragePartition()
          ->GetURLLoaderFactoryForBrowserProcess();
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = GURL("https://check.torproject.org/api/ip");
  request->method = "GET";
  request->load_flags = net::LOAD_DO_NOT_SAVE_COOKIES;
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  verify_loader_ =
      network::SimpleURLLoader::Create(std::move(request), IpCheckAnnotation());
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
