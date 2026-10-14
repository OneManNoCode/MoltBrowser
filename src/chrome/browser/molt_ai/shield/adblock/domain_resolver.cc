// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/shield/adblock/domain_resolver.h"

#include <string_view>

#include "net/base/registry_controlled_domains/registry_controlled_domain.h"

namespace molt_ai::shield::ffi {

size_t registrable_domain_offset(rust::Str host) {
  const std::string_view host_view(host.data(), host.size());
  if (host_view.empty()) {
    return 0;
  }
  // GetDomainAndRegistry() canonicalizes `host_view` and returns its
  // registrable domain (empty for IP literals and hosts that are themselves
  // a registry). adblock-rust passes canonical (lowercase, punycode) hosts,
  // so the result is a suffix of `host_view`.
  const std::string domain =
      net::registry_controlled_domains::GetDomainAndRegistry(
          host_view,
          net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);
  if (domain.empty() || domain.size() > host_view.size()) {
    return 0;
  }
  const size_t offset = host_view.size() - domain.size();
  if (host_view.substr(offset) != domain) {
    // Not a suffix (non-canonical input); treat the whole host as the
    // domain, like adblock-rust does for unparseable hosts.
    return 0;
  }
  return offset;
}

}  // namespace molt_ai::shield::ffi
