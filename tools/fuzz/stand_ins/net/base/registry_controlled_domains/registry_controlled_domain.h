// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// STANDALONE FUZZING STAND-IN for Chromium's
// net/base/registry_controlled_domains/registry_controlled_domain.h (the
// subset MoltBrowser uses), for tools/fuzz/molt_fuzz.py only. It
// approximates the public suffix list with a small table: the registries
// below, and any other last label (an unknown one, as upstream's
// GetDomainAndRegistry() takes it). Hosts are taken as canonical
// (lowercase, no trailing dot).

#ifndef MOLT_FUZZ_STAND_INS_NET_BASE_REGISTRY_CONTROLLED_DOMAINS_REGISTRY_CONTROLLED_DOMAIN_H_
#define MOLT_FUZZ_STAND_INS_NET_BASE_REGISTRY_CONTROLLED_DOMAINS_REGISTRY_CONTROLLED_DOMAIN_H_

#include <string>
#include <string_view>

namespace net {
namespace registry_controlled_domains {

enum PrivateRegistryFilter {
  EXCLUDE_PRIVATE_REGISTRIES = 0,
  INCLUDE_PRIVATE_REGISTRIES,
};

namespace internal {

// The registry |host| ends in: the longest listed one it ends in after a
// '.', else its last label. Empty for a host with no '.'.
inline std::string_view RegistryOf(std::string_view host,
                                   PrivateRegistryFilter filter) {
  static constexpr std::string_view kPublic[] = {"com", "net", "org",
                                                 "io",  "uk",  "co.uk"};
  static constexpr std::string_view kPrivate[] = {"github.io",
                                                  "s3.amazonaws.com"};
  std::string_view best;
  auto consider = [&](std::string_view r) {
    if (host.size() > r.size() && host.ends_with(r) &&
        host[host.size() - r.size() - 1] == '.' && r.size() > best.size()) {
      best = r;
    }
  };
  for (std::string_view r : kPublic) consider(r);
  if (filter == INCLUDE_PRIVATE_REGISTRIES) {
    for (std::string_view r : kPrivate) consider(r);
  }
  if (best.empty()) {
    const size_t dot = host.rfind('.');
    if (dot != std::string_view::npos) best = host.substr(dot + 1);
  }
  return best;
}

}  // namespace internal

// The registrable domain (one label before its registry) of |host|; empty
// for an IP address, a single label, or a host that is itself a registry.
inline std::string GetDomainAndRegistry(std::string_view host,
                                        PrivateRegistryFilter filter) {
  if (host.empty() || host.find(':') != std::string_view::npos ||
      host.find_first_not_of("0123456789.") == std::string_view::npos) {
    return std::string();
  }
  static constexpr std::string_view kListed[] = {
      "com", "net", "org", "io", "uk", "co.uk", "github.io",
      "s3.amazonaws.com"};
  for (std::string_view r : kListed) {
    // A host that is itself a registry has no registrable domain.
    if (host == r) return std::string();
  }
  const std::string_view registry = internal::RegistryOf(host, filter);
  if (registry.empty() || host.size() <= registry.size() + 1) {
    return std::string();
  }
  const std::string_view rest =
      host.substr(0, host.size() - registry.size() - 1);
  const size_t dot = rest.rfind('.');
  return std::string(dot == std::string_view::npos ? host
                                                   : host.substr(dot + 1));
}

}  // namespace registry_controlled_domains
}  // namespace net

#endif  // MOLT_FUZZ_STAND_INS_NET_BASE_REGISTRY_CONTROLLED_DOMAINS_REGISTRY_CONTROLLED_DOMAIN_H_
