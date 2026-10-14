// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/vault/vault_host_match.h"

#include "base/strings/string_util.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "net/base/url_util.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "url/url_constants.h"

namespace molt_ai {
namespace vault {

namespace {

// eTLD+1 including the private section of the PSL: github.io, appspot.com,
// blogspot.com etc. are registries, so two users' subdomains never share
// a "site".
std::string RegistrableDomain(std::string_view canonical_host) {
  return net::registry_controlled_domains::GetDomainAndRegistry(
      canonical_host,
      net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);
}

}  // namespace

std::string CanonicalizeSiteHost(std::string_view site_host) {
  std::string s(base::TrimWhitespaceASCII(site_host, base::TRIM_ALL));
  if (s.empty()) {
    return std::string();
  }
  if (s.find("://") == std::string::npos) {
    s = "https://" + s;
  }
  const GURL url(s);
  if (!url.is_valid() || !url.has_host()) {
    return std::string();
  }
  return std::string(url.host());
}

HostMatch ClassifyHostMatch(std::string_view entry_host,
                            std::string_view entry_site,
                            std::string_view page_host,
                            std::string_view page_site) {
  if (entry_host.empty() || page_host.empty()) {
    return HostMatch::kNone;
  }
  if (entry_host == page_host) {
    return HostMatch::kExact;
  }
  if (!entry_site.empty() && entry_site == page_site) {
    return HostMatch::kSameSite;
  }
  return HostMatch::kNone;
}

HostMatch MatchSiteHost(std::string_view site_host,
                        std::string_view page_host) {
  const std::string entry = CanonicalizeSiteHost(site_host);
  const std::string page = CanonicalizeSiteHost(page_host);
  return ClassifyHostMatch(entry, RegistrableDomain(entry), page,
                           RegistrableDomain(page));
}

bool IsFillableOrigin(const url::Origin& origin) {
  if (origin.opaque()) {
    return false;
  }
  if (origin.scheme() == url::kHttpsScheme) {
    return true;
  }
  return origin.scheme() == url::kHttpScheme &&
         net::IsLocalhost(origin.GetURL());
}

}  // namespace vault
}  // namespace molt_ai
