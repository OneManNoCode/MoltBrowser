// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// How MoltNet's network context differs from every other profile's in the
// settings Chromium keeps per network context rather than in prefs.
//
// ProfileNetworkContextService::ConfigureNetworkContextParamsInternal()
// calls ApplyMoltNetNetworkContextSettings() for every network context it
// configures; the network service and the certificate verifier act on the
// fields it sets (hooks marked "MoltBrowser:" in
// services/network/network_context.cc and services/cert_verifier/). The
// only input is whether the profile is MoltNet's (IsMoltNetProfile()):
// every other profile keeps Chromium's behaviour.
//
// Standard library only and header-only, so molt_ai_unittests tests it and
// the upstream hook needs no new link dependency.

#ifndef CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_NETWORK_CONTEXT_POLICY_H_
#define CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_NETWORK_CONTEXT_POLICY_H_

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

  friend constexpr bool operator==(const MoltNetNetworkContextSettings&,
                                   const MoltNetNetworkContextSettings&) =
      default;
};

constexpr MoltNetNetworkContextSettings MoltNetNetworkContextSettingsFor(
    bool is_moltnet_profile) {
  MoltNetNetworkContextSettings settings;
  settings.isolate_each_cert_fetch = is_moltnet_profile;
  settings.basic_and_digest_auth_only = is_moltnet_profile;
  return settings;
}

// Writes MoltNetNetworkContextSettingsFor(is_moltnet_profile) into a
// network context's parameters: `network_context_params` is a
// network::mojom::NetworkContextParams whose
// http_auth_static_network_context_params is already set, and
// `cert_verifier_creation_params` a
// cert_verifier::mojom::CertVerifierCreationParams. A template, so this
// header needs neither mojom.
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
}

}  // namespace molt_ai::tor

#endif  // CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_NETWORK_CONTEXT_POLICY_H_
