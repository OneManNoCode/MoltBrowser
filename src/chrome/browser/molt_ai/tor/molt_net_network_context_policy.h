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
// services/network/pervasive_resources/). Two settings
// the browser also updates later have their own hooks in
// ProfileNetworkContextService (split_http_auth_cache and
// BlockPrivateStateTokensFor()). The only input is whether the profile is
// MoltNet's (IsMoltNetProfile()): every other profile keeps Chromium's
// behaviour.
//
// Most of these keep per-site state per site: within one MoltNet identity,
// what one site causes in the network context must not be observable by
// another site (docs/MOLTNET_COMPLIANCE.md section 2, "Shared network
// state"). What the process-wide switches decide is checked separately,
// before a MoltNet window opens (molt_net_partitioning_policy.h).
//
// Header-only (the standard library and base::Token), so molt_ai_unittests
// tests it and the upstream hook needs no new link dependency.

#ifndef CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_NETWORK_CONTEXT_POLICY_H_
#define CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_NETWORK_CONTEXT_POLICY_H_

#include <optional>
#include <string>

#include "base/token.h"

namespace molt_ai::tor {

struct MoltNetNetworkContextSettings {
  // Certificate fetches: the missing intermediate certificates (AIA), OCSP
  // responses and CRLs the certificate verifier downloads while it checks
  // a site's certificate. Chromium keys each one by the fetched URL's own
  // site, whichever site's certificate needed it, so the same fetch made
  // for different sites shares one HTTP cache entry and, in MoltNet, one
  // Tor circuit (the SOCKS username is the key's top-level site). When
  // true, each fetch gets a one-off key of its own
  // (net::IsolationInfo::CreateTransient()): nothing is cached, it has its
  // own connection and its own Tor circuit, and two fetches are never
  // merged into one request.
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
  // takes).
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
  return settings;
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
// own; other contexts get none. (split_http_auth_cache and
// block_private_state_tokens are written by their own hooks, see above.)
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
}

}  // namespace molt_ai::tor

#endif  // CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_NETWORK_CONTEXT_POLICY_H_
