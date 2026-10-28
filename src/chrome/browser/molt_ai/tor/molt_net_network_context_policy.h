// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// How MoltNet's network context differs from every other profile's in the
// settings Chromium keeps per network context rather than in prefs.
//
// ProfileNetworkContextService::ConfigureNetworkContextParamsInternal()
// calls ApplyMoltNetNetworkContextSettings() for every network context it
// configures; the network service, the certificate verifier and the SOCKS
// connect job act on the fields it sets (hooks marked "MoltBrowser:" in
// services/network/network_context.cc, services/cert_verifier/,
// net/http/http_network_session.*, net/http/transport_security_state.*,
// net/socket/ (the SOCKS connect job, ssl_client_socket.*) and
// services/network/pervasive_resources/). Three settings have their own
// hooks in ProfileNetworkContextService (split_http_auth_cache,
// BlockPrivateStateTokensFor() and DeviceBoundSessionsEnabledFor()). The
// only input is whether the profile is MoltNet's (IsMoltNetProfile()):
// every other profile keeps Chromium's behaviour.
//
// Most of these keep per-site state per site: within one MoltNet identity,
// what one site causes in the network context must not be observable by
// another site (docs/MOLTNET_COMPLIANCE.md section 2, "Shared network
// state"). What the process-wide switches decide is checked separately,
// before a MoltNet window opens (molt_net_partitioning_policy.h).
//
// Header-only (the standard library, base::Token and net's std-only
// molt_per_site_socket_pool_policy.h), so molt_ai_unittests tests it and the
// upstream hook needs no new link dependency.

#ifndef CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_NETWORK_CONTEXT_POLICY_H_
#define CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_NETWORK_CONTEXT_POLICY_H_

#include <cstdint>
#include <optional>
#include <string>

#include "base/token.h"
#include "net/socket/molt_per_site_socket_pool_policy.h"

namespace molt_ai::tor {

struct MoltNetNetworkContextSettings {
  // Certificate fetches: the missing intermediate certificates (AIA), OCSP
  // responses and CRLs the certificate verifier downloads while it checks
  // a site's certificate. Chromium keys each one by the fetched URL's own
  // site, whichever site's certificate needed it, so the same fetch made
  // for different sites shares one HTTP cache entry and, in MoltNet, one
  // Tor circuit (the SOCKS username is the key's top-level site). When
  // true, each fetch gets a one-off key of its own: nothing is cached, it
  // has its own connection and its own Tor circuit, and two fetches are
  // never merged into one request. The key's top-level site is the site
  // whose connection needed the fetch, which MoltNet's verifications carry
  // (uncached_cert_verification below; net/cert/molt_cert_fetch_site.h),
  // so the fetch counts against that site's connection limits; a fetch no
  // site is named for gets a key derived from nothing
  // (net::IsolationInfo::CreateTransient()).
  bool isolate_each_cert_fetch = false;

  // HTTP authentication: when true, sites are offered Basic and Digest
  // only, never NTLM or Negotiate (Kerberos), whatever the browser-wide
  // settings and policies allow. Those two schemes can carry details of
  // the computer, and Negotiate can look the site's name up with the
  // system's resolver, outside the proxy. Tor Browser offers neither.
  bool basic_and_digest_auth_only = false;

  // Tor circuits: the SOCKS5 username of each connection is its top-level
  // site (net/socket/socks_connect_job.cc), and Tor keeps streams with
  // different SOCKS credentials on different circuits (IsolateSOCKSAuth).
  // When true, the network context also gets a SOCKS5 password of its own,
  // random and new each time the context is created
  // (NewSocksIsolationPassword()). Every MoltNet profile, so every MoltNet
  // identity (a new one after New Identity or once the last MoltNet window
  // has closed), has a network context of its own, so it never shares a
  // circuit or an exit with an earlier identity, also for a site both
  // visit, and however long Tor would keep using the earlier circuits.
  bool fresh_socks_isolation_password = false;

  // Certificate verification: Chromium wraps each network context's
  // certificate verifier in net::CachingCertVerifier, which keeps results
  // for up to 30 minutes keyed by certificate, hostname and flags, and
  // net::CoalescingCertVerifier, which merges identical verifications in
  // flight. Neither is keyed by top-level site, so how long a verification
  // takes, and whether a missing intermediate is downloaded again, would
  // depend on what other sites (or a third party embedded on several)
  // loaded before in this identity. When true, the context has neither:
  // every TLS connection, resumed ones included, is verified on its own (a
  // few milliseconds, next to the hundreds a connection through Tor
  // takes). Each verification then names the site of its connection, for
  // the certificate fetches it needs (isolate_each_cert_fetch).
  bool uncached_cert_verification = false;

  // HTTP cache: Chromium 152 files requests for a built-in list of
  // well-known scripts and styles under one shared entry instead of one per
  // top-level site, where cookie settings allow full cookie access
  // (network::features::kCacheSharingForPervasiveResources). When true,
  // none of this context's requests uses those shared entries, whatever the
  // feature and cookie exceptions say.
  bool no_shared_cache_entries = false;

  // HSTS: the context's store of hosts that asked for HTTPS only is keyed
  // by host, whichever site's page loaded them. When true, it upgrades
  // top-level navigations only (net::features::kHstsTopLevelNavigationsOnly
  // for this context), so a site's subresource loads can't read what other
  // sites stored there; MoltNet's HTTPS-Only mode upgrades every plain-HTTP
  // navigation anyway, so for those the entries make no observable
  // difference either.
  bool hsts_top_level_navigations_only = false;

  // HTTP authentication: server credentials are cached for the context's
  // lifetime. Chromium keys them by top-level site only when
  // network::features::kSplitAuthCacheByNetworkIsolationKey is on (off by
  // default at 152) and the GloballyScopeHTTPAuthCacheEnabled policy isn't
  // set. When true, always keyed by top-level site: the hook in
  // ProfileNetworkContextService::ShouldSplitAuthCacheByNetworkIsolationKey()
  // decides both the value a network context is created with and the
  // updates sent when the policy changes.
  bool split_http_auth_cache = false;

  // Network quality: the network service keeps one estimate for the whole
  // browser (net::NetworkQualityEstimator), fed by every network context's
  // requests. A context's request scheduling (ResourceScheduler) holds back
  // low-priority requests while the estimate says 3g or slower, so a page's
  // load timing would depend on it, and other sites could move it by
  // loading heavily; MoltNet's own requests would move what other windows
  // read. When true, the context has no estimator: its requests don't feed
  // the estimate and it schedules as on a network of unknown quality, the
  // same for every MoltNet user (MoltNet's renderers report fixed values,
  // third_party/blink/renderer/core/molt_fingerprinting/moltnet_network.h).
  bool no_network_quality_estimator = false;

  // Private State Tokens: tokens an issuer gives on one site can be
  // detected and redeemed on any other (document.hasPrivateToken()), by
  // design. When true, issuance and redemption are blocked for the context
  // whatever the anti-abuse setting says (BlockPrivateStateTokensFor()).
  bool block_private_state_tokens = false;

  // TLS client certificates: Chromium keeps the certificate a user picks
  // for a server (net::SSLClientAuthCache, inside the context's
  // SSLClientContext) by host and port, for the context's lifetime, not by
  // top-level site. That includes "no certificate", which the browser
  // answers without asking when the user has none. A choice made while
  // site A was open would then apply to that server under site B too, and
  // skip the aborted handshake, the browser round trip and the second
  // connection the first request costs, which the server can time. When
  // true, the context answers every server's certificate request with no
  // certificate, at once, the same way each time: nothing is remembered,
  // nothing is asked, no certificate (and so no name in it) is ever sent,
  // and an administrator's AutoSelectCertificateForUrls doesn't apply. Tor
  // Browser keeps its certificate store in memory (security.nocertdb), so
  // it has no personal certificates to offer either.
  bool send_no_client_certificates = false;

  // Device-bound sessions (DBSC; on by default on Windows and macOS): a
  // site can bind its sign-in cookies to a signing key the operating
  // system's secure hardware makes (a TPM, the Secure Enclave), and the
  // network context then signs refresh requests with it. Whether a site
  // can set one up depends on the machine's hardware, which differs between
  // MoltNet users, and the key is made and held by the operating system,
  // not in MoltNet's in-memory profile. When true, the context has no
  // session service (DeviceBoundSessionsEnabledFor()): sites fall back to
  // their ordinary cookies, as in Tor Browser, which has no such sessions.
  bool no_device_bound_sessions = false;

  // DNS-over-HTTPS probes: when secure DNS has servers (the user's or an
  // administrator's choice, or the automatic upgrade of a system resolver
  // that offers DoH), the network service probes them for every network
  // context it registers and again after each network change. The probes
  // are DoH requests made through the context itself but, like every DoH
  // request, with the proxy bypassed (net::LOAD_BYPASS_PROXY) and the DoH
  // server's name looked up with the system resolver: from a MoltNet
  // context, a connection from the user's own address, outside Tor, each
  // time a MoltNet identity starts. When true, the context starts none.
  // MoltNet's pages' names are resolved by Tor inside SOCKS5, so it never
  // needs DoH.
  bool no_doh_probes = false;

  // Connection limits: Chromium keeps one socket pool per proxy chain in a
  // network context, with a limit for the whole pool (128 for a proxy,
  // or what the MaxConnectionsPerProxy policy says). All of MoltNet's
  // traffic goes through Tor's SOCKS5 port, one proxy chain, so every site
  // shared that limit: one site holding its connections open made every
  // other site's new connections wait, which a site open at the same time
  // can measure. When not 0, each site gets socket pools of its own with
  // fixed limits derived from this one, so MaxConnectionsPerProxy doesn't
  // show either, all of them within one budget that Tor can always hold
  // (net/socket/molt_per_site_socket_pool_policy.h).
  uint32_t per_site_socket_pool_size = 0;

  // Keepalive request limits: the network service counts requests that may
  // outlive their page for all profiles and sites together (2048 in
  // flight), so one site using them up shows to another, and to regular
  // windows. When true, the context's requests count against limits of
  // their own site (services/network/molt_keepalive_partitions.h).
  bool per_site_keepalive_limits = false;

  // HTTP cache: an off-the-record context keeps it in memory, with one size
  // limit and one least-recently-used list for every site, so one site's
  // writes evicted another's entries, which the other could time, and the
  // machine's memory decided the size and evicted entries under pressure.
  // When true, each site's entries are evicted only for its own share or for
  // going unused, all sites stay within a fixed total, and memory pressure
  // evicts nothing (net/disk_cache/memory/molt_mem_backend_budget_policy.h).
  bool per_site_http_cache = false;

  // Ending an identity: the browser ends one by sending its network
  // contexts a configuration that refuses every request; requests already
  // under way would carry on (a body being sent, a redirect followed on an
  // open connection). When true, that refusal also cancels every request of
  // the context and closes its connections, in the same task, so nothing
  // the identity started is sent after its context has the refusal
  // (services/network/molt_ended_identity_policy.h).
  bool stop_all_when_refusing = false;

  // A page's nonce (its credentialless frames'): the cookie store keeps no
  // total for the partitions such frames use, and nothing drops their
  // cookies, connections or network caches by the nonce. When true, each
  // nonce partition and each nonce's partitions together have limits of
  // their own, and the browser's tab helper can have the context delete and
  // refuse a nonce's cookies and drop what its caches keep under it
  // (NetworkContext::MoltDropNonce(): net/cookies/molt_nonce_cookie_policy.h,
  // services/network/molt_nonce_drop_policy.h, molt_net_nonce_storage.h).
  bool nonce_limits = false;

  friend constexpr bool operator==(const MoltNetNetworkContextSettings&,
                                   const MoltNetNetworkContextSettings&) =
      default;
};

constexpr MoltNetNetworkContextSettings MoltNetNetworkContextSettingsFor(
    bool is_moltnet_profile) {
  MoltNetNetworkContextSettings settings;
  settings.isolate_each_cert_fetch = is_moltnet_profile;
  settings.basic_and_digest_auth_only = is_moltnet_profile;
  settings.fresh_socks_isolation_password = is_moltnet_profile;
  settings.uncached_cert_verification = is_moltnet_profile;
  settings.no_shared_cache_entries = is_moltnet_profile;
  settings.hsts_top_level_navigations_only = is_moltnet_profile;
  settings.split_http_auth_cache = is_moltnet_profile;
  settings.no_network_quality_estimator = is_moltnet_profile;
  settings.block_private_state_tokens = is_moltnet_profile;
  settings.send_no_client_certificates = is_moltnet_profile;
  settings.no_doh_probes = is_moltnet_profile;
  settings.no_device_bound_sessions = is_moltnet_profile;
  settings.per_site_socket_pool_size =
      is_moltnet_profile ? static_cast<uint32_t>(net::kMoltPerSitePoolSize)
                         : 0;
  settings.per_site_keepalive_limits = is_moltnet_profile;
  settings.per_site_http_cache = is_moltnet_profile;
  settings.stop_all_when_refusing = is_moltnet_profile;
  settings.nonce_limits = is_moltnet_profile;
  return settings;
}

// Whether the browser tells a profile's renderer processes fixed values
// instead of its network-quality estimate and the operating system's
// connection type (ChromeContentBrowserClient::
// ShouldSendUniformNetworkState(), content/browser/network/
// molt_uniform_network_state.h). The browser keeps one estimate for all
// profiles and sends it to every renderer; MoltNet's renderers report fixed
// values to pages anyway, so this only keeps the real ones from a
// compromised MoltNet renderer. Decided by the profile alone (as WebRTC is,
// molt_net_webrtc.h), not by the fingerprinting switch.
constexpr bool SendUniformNetworkStateToRenderers(bool is_moltnet_profile) {
  return is_moltnet_profile;
}

// Whether the download requests the browser makes for a profile's pages
// carry the network isolation of the frame or download they come from
// (ChromeContentBrowserClient::ShouldKeepDownloadsWithTheirSite(),
// content/browser/download/molt_download_isolation_policy.h). Chromium
// files a download a page starts without navigating (a link with
// `download`, "Save link as", "Save image as"), a resumed download and Save
// Page As's resources under their destination, as top-level navigations
// there: their cache partition, connections and cookies, and in MoltNet the
// destination's Tor circuit. Background Fetch, in every off-the-record
// profile, carries the site's isolation through the download service
// (components/download/.../molt_registering_site_isolation.h).
constexpr bool KeepDownloadsWithTheirSite(bool is_moltnet_profile) {
  return is_moltnet_profile;
}

// Whether the storage service keeps a profile's localStorage and
// sessionStorage caches per top-level site
// (ChromeContentBrowserClient::ShouldKeepDomStorageCachesPerSite(),
// components/services/storage/dom_storage/molt_dom_storage_site_policy.h).
// Chromium purges every site's unused areas when all areas together pass
// one limit, so one site could push another's out and time the reloads.
constexpr bool KeepDomStorageCachesPerSite(bool is_moltnet_profile) {
  return is_moltnet_profile;
}

// Whether a profile's Background Fetch is scheduled per top-level site
// (ChromeContentBrowserClient::ShouldScheduleBackgroundFetchesPerSite(),
// content/browser/background_fetch/molt_background_fetch_site_policy.h).
// Chromium runs 2 registrations and 2 downloads at a time for a whole
// partition, so one site's slow fetches could keep every other site's from
// starting, and the other site could time it.
constexpr bool ScheduleBackgroundFetchesPerSite(bool is_moltnet_profile) {
  return is_moltnet_profile;
}

// ProfileNetworkContextService's block_trust_tokens, when a network context
// is created and when the anti-abuse content setting changes:
// `chromium_value` is Chromium's (the setting blocks them).
constexpr bool BlockPrivateStateTokensFor(bool is_moltnet_profile,
                                          bool chromium_value) {
  return MoltNetNetworkContextSettingsFor(is_moltnet_profile)
             .block_private_state_tokens ||
         chromium_value;
}

// ProfileNetworkContextService's device_bound_sessions_enabled, when a
// network context is created: `chromium_value` is Chromium's (the
// DeviceBoundSessions feature).
constexpr bool DeviceBoundSessionsEnabledFor(bool is_moltnet_profile,
                                             bool chromium_value) {
  return chromium_value && !MoltNetNetworkContextSettingsFor(is_moltnet_profile)
                                .no_device_bound_sessions;
}

// A SOCKS5 password for one network context: 128 bits from a
// cryptographically secure source, as 32 hexadecimal digits (RFC 1929
// allows 1 to 255 bytes).
inline std::string NewSocksIsolationPassword() {
  return base::Token::CreateRandom().ToString();
}

// Writes MoltNetNetworkContextSettingsFor(is_moltnet_profile) into a
// network context's parameters: `network_context_params` is a
// network::mojom::NetworkContextParams whose
// http_auth_static_network_context_params is already set, and
// `cert_verifier_creation_params` a
// cert_verifier::mojom::CertVerifierCreationParams. Called once for each
// network context created, so each MoltNet context gets a password of its
// own; other contexts get none. (split_http_auth_cache,
// block_private_state_tokens and no_device_bound_sessions are written by
// their own hooks, see above.)
// A template, so this header needs neither mojom.
template <typename NetworkContextParams, typename CertVerifierCreationParams>
void ApplyMoltNetNetworkContextSettings(
    bool is_moltnet_profile,
    NetworkContextParams& network_context_params,
    CertVerifierCreationParams& cert_verifier_creation_params) {
  const MoltNetNetworkContextSettings settings =
      MoltNetNetworkContextSettingsFor(is_moltnet_profile);
  cert_verifier_creation_params.molt_isolate_each_cert_fetch =
      settings.isolate_each_cert_fetch;
  network_context_params.http_auth_static_network_context_params
      ->molt_basic_and_digest_auth_only = settings.basic_and_digest_auth_only;
  network_context_params.molt_socks_isolation_password =
      settings.fresh_socks_isolation_password
          ? std::optional<std::string>(NewSocksIsolationPassword())
          : std::nullopt;
  network_context_params.molt_uncached_cert_verification =
      settings.uncached_cert_verification;
  network_context_params.molt_no_shared_cache_entries =
      settings.no_shared_cache_entries;
  network_context_params.molt_hsts_top_level_navigations_only =
      settings.hsts_top_level_navigations_only;
  network_context_params.molt_no_network_quality_estimator =
      settings.no_network_quality_estimator;
  network_context_params.molt_send_no_client_certificates =
      settings.send_no_client_certificates;
  network_context_params.molt_no_doh_probes = settings.no_doh_probes;
  network_context_params.molt_per_site_socket_pool_size =
      settings.per_site_socket_pool_size;
  network_context_params.molt_per_site_keepalive_limits =
      settings.per_site_keepalive_limits;
  network_context_params.molt_per_site_http_cache =
      settings.per_site_http_cache;
  network_context_params.molt_stop_all_when_refusing =
      settings.stop_all_when_refusing;
  network_context_params.molt_nonce_limits = settings.nonce_limits;
}

}  // namespace molt_ai::tor

#endif  // CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_NETWORK_CONTEXT_POLICY_H_
