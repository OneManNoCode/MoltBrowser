// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/molt_net_routing.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/supports_user_data.h"
#include "base/values.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/molt_ai/tor/molt_net_profile_id.h"
#include "chrome/browser/molt_ai/tor/tor_manager.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/common/molt_ai/fingerprinting/molt_fingerprinting_features.h"
#include "chrome/common/pref_names.h"
#include "components/language/core/browser/pref_names.h"
#include "components/prefs/pref_service.h"
#include "components/proxy_config/proxy_config_dictionary.h"
#include "components/proxy_config/proxy_config_pref_names.h"
#include "components/proxy_config/proxy_prefs.h"
#include "content/public/browser/storage_partition.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "net/base/net_errors.h"
#include "net/base/network_anonymization_key.h"
#include "net/base/proxy_chain.h"
#include "net/base/proxy_server.h"
#include "net/base/schemeful_site.h"
#include "net/proxy_resolution/proxy_info.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "services/network/public/mojom/proxy_lookup_client.mojom.h"
#include "url/gurl.h"

namespace molt_ai {
namespace tor {

namespace {

// blink::kWebRTCIPHandlingDisableNonProxiedUdp (web-exposed, stable value).
// WebRTC may only use the proxy, so STUN/UDP can't reveal the real IP.
constexpr char kWebRtcDisableNonProxiedUdp[] = "disable_non_proxied_udp";

// prefetch::NetworkPredictionOptions::kDisabled. Preconnect / DNS prefetch
// resolve via the host resolver directly, i.e. OUTSIDE the SOCKS proxy.
constexpr int kNetworkPredictionDisabled = 2;

// What pre-isolation builds wrote to the regular profile + local state.
constexpr char kLegacyTorProxy[] = "socks5://127.0.0.1:9050";
constexpr char kLegacyDohModeOff[] = "off";

// Any http(s) URL works for the routing self-check; the lookup never leaves
// the network service (no request is made).
constexpr char kRoutingProbeUrl[] = "https://check.torproject.org/";

// Uniform language for every MoltNet window (fingerprinting protection). Off-
// the-record profiles only send the first entry, so pages see "en-US" in both
// Accept-Language and navigator.language(s).
constexpr char kMoltNetAcceptLanguages[] = "en-US,en";

// The current MoltNet OTR profile id, attached to the ORIGINAL profile.
const void* const kMoltNetProfileDataKey = &kMoltNetProfileDataKey;

struct MoltNetProfileData : public base::SupportsUserData::Data {
  explicit MoltNetProfileData(const Profile::OTRProfileID& otr_id)
      : id(otr_id) {}
  const Profile::OTRProfileID id;
};

std::string SocksProxyUri(uint16_t port) {
  return "socks5://127.0.0.1:" + base::NumberToString(port);
}

std::optional<std::string> FixedProxyServer(const base::Value* value) {
  if (!value || !value->is_dict()) {
    return std::nullopt;
  }
  ProxyConfigDictionary dict(value->GetDict().Clone());
  ProxyPrefs::ProxyMode mode;
  std::string server;
  if (!dict.GetMode(&mode) || mode != ProxyPrefs::MODE_FIXED_SERVERS ||
      !dict.GetProxyServer(&server)) {
    return std::nullopt;
  }
  return server;
}

// True iff |name|'s effective value comes from the user store (for an OTR
// profile: the in-memory overlay MoltNet wrote), i.e. no policy, extension,
// supervision or command-line flag outranks it.
bool IsUserControlled(PrefService* prefs, const char* name) {
  const PrefService::Preference* pref = prefs->FindPreference(name);
  return pref && pref->IsUserControlled();
}

// True if the proxy of |prefs| is decided by something other than the user
// store or the default (policy, extension, command line, supervision).
bool ProxyExternallyControlled(PrefService* prefs) {
  const PrefService::Preference* pref =
      prefs->FindPreference(proxy_config::prefs::kProxy);
  return pref && !pref->IsDefaultValue() && !pref->IsUserControlled();
}

bool LeakPrefsInPlace(PrefService* prefs) {
  return IsUserControlled(prefs, prefs::kWebRTCIPHandlingPolicy) &&
         prefs->GetString(prefs::kWebRTCIPHandlingPolicy) ==
             kWebRtcDisableNonProxiedUdp &&
         IsUserControlled(prefs, prefs::kNetworkPredictionOptions) &&
         prefs->GetInteger(prefs::kNetworkPredictionOptions) ==
             kNetworkPredictionDisabled &&
         IsUserControlled(prefs, prefs::kHttpsOnlyModeEnabled) &&
         prefs->GetBoolean(prefs::kHttpsOnlyModeEnabled);
}

bool ProxyPointsAtTor(PrefService* prefs, uint16_t socks_port) {
  if (!socks_port ||
      !IsUserControlled(prefs, proxy_config::prefs::kProxy)) {
    return false;
  }
  std::optional<std::string> server = FixedProxyServer(
      prefs->FindPreference(proxy_config::prefs::kProxy)->GetValue());
  return server && *server == SocksProxyUri(socks_port);
}

// Self-owned ProxyLookupClient: asks a NetworkContext how it would route
// kRoutingProbeUrl and reports whether that is exactly our Tor SOCKS proxy.
class RoutingCheck : public network::mojom::ProxyLookupClient {
 public:
  static void Start(network::mojom::NetworkContext* network_context,
                    uint16_t socks_port,
                    MoltNetRouting::DoneCallback done) {
    // Deletes itself in Finish().
    auto* check = new RoutingCheck(socks_port, std::move(done));
    const GURL url(kRoutingProbeUrl);
    network_context->LookUpProxyForURL(
        url,
        net::NetworkAnonymizationKey::CreateSameSite(net::SchemefulSite(url)),
        check->receiver_.BindNewPipeAndPassRemote());
    check->receiver_.set_disconnect_handler(base::BindOnce(
        &RoutingCheck::Finish, base::Unretained(check), false,
        std::string("MoltNet's network context went away")));
  }

  // network::mojom::ProxyLookupClient:
  void OnProxyLookupComplete(
      int32_t net_error,
      const std::optional<net::ProxyInfo>& proxy_info) override {
    if (net_error != net::OK || !proxy_info) {
      Finish(false, "MoltNet couldn't confirm its proxy settings");
      return;
    }
    // Exactly one entry (no DIRECT or other fallback), a single SOCKS5 hop
    // to our Tor on loopback.
    const net::ProxyInfo& info = *proxy_info;
    bool ok = !info.is_empty() && info.proxy_list().size() == 1 &&
              info.proxy_chain().is_single_proxy();
    if (ok) {
      const net::ProxyServer& server = info.proxy_chain().First();
      ok = server.scheme() == net::ProxyServer::SCHEME_SOCKS5 &&
           server.host_port_pair().host() == "127.0.0.1" &&
           server.host_port_pair().port() == socks_port_;
    }
    Finish(ok, ok ? std::string()
                  : std::string("MoltNet's window would not be routed "
                                "through Tor; refusing to open it"));
  }

 private:
  RoutingCheck(uint16_t socks_port, MoltNetRouting::DoneCallback done)
      : socks_port_(socks_port), done_(std::move(done)) {}
  ~RoutingCheck() override = default;

  void Finish(bool ok, std::string error) {
    receiver_.reset();
    MoltNetRouting::DoneCallback done = std::move(done_);
    delete this;
    if (done) {
      std::move(done).Run(ok, std::move(error));
    }
  }

  const uint16_t socks_port_;
  MoltNetRouting::DoneCallback done_;
  mojo::Receiver<network::mojom::ProxyLookupClient> receiver_{this};
};

void OnTorStateChanged() {
  TorManager* manager = TorManager::Get();
  if (!manager->IsRunning() || !g_browser_process ||
      !g_browser_process->profile_manager()) {
    // Tor stopped: leave the MoltNet profile pointed at the dead port, so
    // its requests keep failing instead of going direct.
    return;
  }
  for (Profile* profile :
       g_browser_process->profile_manager()->GetLoadedProfiles()) {
    if (Profile* moltnet = MoltNetRouting::GetMoltNetProfile(profile)) {
      std::string error;
      if (!MoltNetRouting::ApplyRoutingPrefs(moltnet, manager->socks_port(),
                                             &error)) {
        LOG(ERROR) << "[MoltNet] could not re-point MoltNet profile: "
                   << error;
      }
    }
  }
}

}  // namespace

// static
Profile* MoltNetRouting::GetMoltNetProfile(Profile* profile) {
  if (!profile) {
    return nullptr;
  }
  Profile* original = profile->GetOriginalProfile();
  auto* data = static_cast<MoltNetProfileData*>(
      original->GetUserData(kMoltNetProfileDataKey));
  if (!data) {
    return nullptr;
  }
  if (!original->HasOffTheRecordProfile(data->id)) {
    original->RemoveUserData(kMoltNetProfileDataKey);
    return nullptr;
  }
  return original->GetOffTheRecordProfile(data->id,
                                          /*create_if_needed=*/false);
}

// static
Profile* MoltNetRouting::GetOrCreateMoltNetProfile(Profile* profile,
                                                   uint16_t socks_port,
                                                   std::string* error) {
  if (!profile) {
    *error = "No profile";
    return nullptr;
  }
  Profile* original = profile->GetOriginalProfile();
  if (original->IsGuestSession() || original->IsSystemProfile()) {
    *error = "MoltNet isn't available in Guest or system profiles";
    return nullptr;
  }
  Profile* moltnet = GetMoltNetProfile(original);
  if (!moltnet) {
    // A fresh unique id per MoltNet session: a new identity is a brand-new
    // profile, not a wiped old one.
    const Profile::OTRProfileID id =
        Profile::OTRProfileID::CreateUnique(kMoltNetOtrProfilePrefix);
    moltnet = original->GetOffTheRecordProfile(id, /*create_if_needed=*/true);
    if (!moltnet) {
      *error = "Could not create the MoltNet profile";
      return nullptr;
    }
    original->SetUserData(kMoltNetProfileDataKey,
                          std::make_unique<MoltNetProfileData>(id));
  }
  EnsureTorStateObserver();
  if (!ApplyRoutingPrefs(moltnet, socks_port, error)) {
    return nullptr;
  }
  return moltnet;
}

// static
bool MoltNetRouting::ApplyRoutingPrefs(Profile* moltnet_profile,
                                       uint16_t socks_port,
                                       std::string* error) {
  if (!IsMoltNetProfile(moltnet_profile)) {
    // Hard rule: never proxy (or touch the network prefs of) any other
    // profile.
    *error = "Refusing to route a non-MoltNet profile";
    return false;
  }
  if (!socks_port) {
    *error = "MoltNet's Tor isn't running";
    return false;
  }
  // These writes land in the OTR profile's in-memory overlay store: none of
  // these prefs is on the incognito "persistent" allowlist, so nothing
  // reaches the regular profile or disk.
  PrefService* prefs = moltnet_profile->GetPrefs();
  // socks5:// = remote DNS (hostnames go to Tor, never the local resolver)
  // and no DIRECT fallback = fail-closed when Tor is unreachable.
  prefs->Set(proxy_config::prefs::kProxy,
             base::Value(ProxyConfigDictionary::CreateFixedServers(
                 SocksProxyUri(socks_port), /*bypass_list=*/std::string())));
  prefs->SetString(prefs::kWebRTCIPHandlingPolicy,
                   kWebRtcDisableNonProxiedUdp);
  prefs->SetInteger(prefs::kNetworkPredictionOptions,
                    kNetworkPredictionDisabled);
  // HTTPS-Only (strict): the exit relay could otherwise read/modify
  // plain-HTTP traffic. Users get a warning interstitial before any HTTP.
  prefs->SetBoolean(prefs::kHttpsOnlyModeEnabled, true);
  // Uniformity: the same languages for everyone. (The renderer side -- UTC,
  // the en-US Intl locale, letterboxed screen metrics -- is configured per
  // renderer process; see chrome/browser/molt_ai/fingerprinting.)
  if (fingerprinting::IsFingerprintingProtectionEnabled()) {
    prefs->SetString(language::prefs::kAcceptLanguages,
                     kMoltNetAcceptLanguages);
  }

  if (ProxyExternallyControlled(prefs) || !ProxyPointsAtTor(prefs, socks_port)) {
    *error =
        // Extension-controlled settings never reach MoltNet's profile
        // (molt_net_isolation.h: CreateMoltNetExtensionPrefStore).
        "A policy or command-line flag controls the proxy, so MoltNet "
        "can't guarantee Tor routing";
    return false;
  }
  if (!LeakPrefsInPlace(prefs)) {
    *error =
        "A policy overrides MoltNet's WebRTC / prediction / HTTPS-Only "
        "settings";
    return false;
  }
  return true;
}

// static
void MoltNetRouting::VerifyRouting(Profile* moltnet_profile,
                                   uint16_t socks_port,
                                   DoneCallback done) {
  if (!IsMoltNetProfile(moltnet_profile)) {
    std::move(done).Run(false, "Not a MoltNet profile");
    return;
  }
  network::mojom::NetworkContext* network_context =
      moltnet_profile->GetDefaultStoragePartition()->GetNetworkContext();
  if (!network_context) {
    std::move(done).Run(false, "MoltNet has no network context");
    return;
  }
  RoutingCheck::Start(network_context, socks_port, std::move(done));
}

// static
void MoltNetRouting::FlushNetworkState(Profile* moltnet_profile) {
  if (!IsMoltNetProfile(moltnet_profile)) {
    return;
  }
  network::mojom::NetworkContext* network_context =
      moltnet_profile->GetDefaultStoragePartition()->GetNetworkContext();
  if (!network_context) {
    return;
  }
  network_context->CloseAllConnections(base::DoNothing());
  network_context->ClearHostCache(network::mojom::ClearDataFilterPtr(),
                                  base::DoNothing());
}

// static
void MoltNetRouting::ForgetMoltNetProfile(Profile* moltnet_profile) {
  if (!IsMoltNetProfile(moltnet_profile)) {
    return;
  }
  Profile* original = moltnet_profile->GetOriginalProfile();
  auto* data = static_cast<MoltNetProfileData*>(
      original->GetUserData(kMoltNetProfileDataKey));
  if (data && data->id == moltnet_profile->GetOTRProfileID()) {
    original->RemoveUserData(kMoltNetProfileDataKey);
  }
}

// static
MoltNetRouting::Status MoltNetRouting::GetStatus(Profile* profile) {
  Status status;
  if (!profile) {
    return status;
  }
  PrefService* prefs = profile->GetPrefs();
  status.proxy_externally_controlled = ProxyExternallyControlled(prefs);
  if (!IsMoltNetProfile(profile)) {
    status.state = RoutingState::kNotRouted;
    return status;
  }
  TorManager* manager = TorManager::Get();
  if (status.proxy_externally_controlled || !LeakPrefsInPlace(prefs)) {
    status.state = RoutingState::kProxyOverridden;
  } else if (!manager->IsRunning() ||
             !ProxyPointsAtTor(prefs, manager->socks_port())) {
    status.state = RoutingState::kBlockedTorDown;
  } else {
    status.state = RoutingState::kRoutedViaTor;
  }
  return status;
}

// static
void MoltNetRouting::MigrateLegacyRoutingPrefs(Profile* profile,
                                               PrefService* local_state) {
  if (profile) {
    PrefService* prefs = profile->GetOriginalProfile()->GetPrefs();
    std::optional<std::string> server =
        FixedProxyServer(prefs->GetUserPrefValue(proxy_config::prefs::kProxy));
    if (server && *server == kLegacyTorProxy) {
      LOG(WARNING) << "[MoltNet] removing legacy Tor proxy from the regular "
                      "profile (MoltNet now uses an isolated profile)";
      prefs->ClearPref(proxy_config::prefs::kProxy);
      const base::Value* webrtc =
          prefs->GetUserPrefValue(prefs::kWebRTCIPHandlingPolicy);
      if (webrtc && webrtc->is_string() &&
          webrtc->GetString() == kWebRtcDisableNonProxiedUdp) {
        prefs->ClearPref(prefs::kWebRTCIPHandlingPolicy);
      }
      const base::Value* prediction =
          prefs->GetUserPrefValue(prefs::kNetworkPredictionOptions);
      if (prediction && prediction->is_int() &&
          prediction->GetInt() == kNetworkPredictionDisabled) {
        prefs->ClearPref(prefs::kNetworkPredictionOptions);
      }
    }
  }
  if (local_state) {
    std::optional<std::string> server = FixedProxyServer(
        local_state->GetUserPrefValue(proxy_config::prefs::kProxy));
    if (server && *server == kLegacyTorProxy) {
      LOG(WARNING) << "[MoltNet] removing legacy Tor proxy from local state";
      local_state->ClearPref(proxy_config::prefs::kProxy);
      const base::Value* doh =
          local_state->GetUserPrefValue(prefs::kDnsOverHttpsMode);
      if (doh && doh->is_string() && doh->GetString() == kLegacyDohModeOff) {
        local_state->ClearPref(prefs::kDnsOverHttpsMode);
      }
    }
  }
}

// static
void MoltNetRouting::EnsureTorStateObserver() {
  [[maybe_unused]] static base::NoDestructor<base::CallbackListSubscription>
      subscription(TorManager::Get()->AddStateChangedCallback(
          base::BindRepeating(&OnTorStateChanged)));
}

}  // namespace tor
}  // namespace molt_ai
