// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Whether a profile's proxy settings are MoltNet's own.
//
// MoltNet writes one proxy setting into its profile: a fixed socks5://
// proxy, its own Tor, with no bypass and no direct fallback
// (molt_net_routing.cc). Two profile settings decide where a request goes:
//
//  * "proxy" (proxy_config::prefs::kProxy) itself, which a policy, an
//    extension or a command-line flag outranks; and
//  * "proxy_override_rules" (proxy_config::prefs::kProxyOverrideRules), set
//    by the ProxyOverrideRules policy: rules the network service evaluates
//    before the proxy, each of which can send the destinations it matches
//    through another proxy or DIRECT, and whose conditions can look names
//    up with the system resolver. PrefProxyConfigTrackerImpl puts them on
//    top of every proxy configuration, a fixed one included
//    (components/proxy_config/pref_proxy_config_tracker_impl.cc).
//
// Either one, set by anything but MoltNet, could send a MoltNet page's
// requests around Tor, so MoltNet doesn't open while it is (the same error
// as for a policy proxy). Chromium 156's enterprise provisioning-domain
// routes (EnableDynamicRouteFetching) only reach a regular profile's proxy
// configuration: EnterpriseProxyServiceFactory has no instance for an
// off-the-record profile.
//
// Header-only and std-only, so molt_ai_unittests tests it.

#ifndef CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_PROXY_POLICY_H_
#define CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_PROXY_POLICY_H_

#include <cstddef>

namespace molt_ai::tor {

// A profile's proxy settings, as its PrefService reports them.
struct ProfileProxyPrefs {
  // "proxy" has its registered default value (use the system's settings).
  bool proxy_is_default = true;
  // "proxy"'s effective value comes from the profile's own (user) store,
  // which is where MoltNet writes it; a policy, an extension or a
  // command-line flag would outrank it.
  bool proxy_is_user_controlled = false;
  // The number of rules in "proxy_override_rules"' effective value.
  size_t override_rule_count = 0;
};

// True if something other than the profile's own store decides its proxy,
// or override rules apply on top of it.
constexpr bool ProxyPrefsOverrideMoltNet(const ProfileProxyPrefs& prefs) {
  return (!prefs.proxy_is_default && !prefs.proxy_is_user_controlled) ||
         prefs.override_rule_count > 0;
}

}  // namespace molt_ai::tor

#endif  // CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_PROXY_POLICY_H_
