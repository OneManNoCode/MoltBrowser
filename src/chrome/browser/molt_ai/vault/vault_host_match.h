// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Deciding whether a saved vault credential may be filled into a page.
//
// The vault used to match a saved site_host against a page host by plain
// suffix ("github.com" matched "gist.github.com", and also any subdomain an
// attacker can host content on), and /vault fill <id> filled any entry into
// whatever tab was active. The rules now are:
//
//   * the page must be https:// (http:// only on a loopback host);
//   * kExact      — same canonical host: fill;
//   * kSameSite   — same registrable domain (eTLD+1 from the Public Suffix
//                   List, *including* private registries, so
//                   alice.github.io and bob.github.io are different sites):
//                   fill only after the user confirms;
//   * kNone       — never fill.

#ifndef CHROME_BROWSER_MOLT_AI_VAULT_VAULT_HOST_MATCH_H_
#define CHROME_BROWSER_MOLT_AI_VAULT_VAULT_HOST_MATCH_H_

#include <string>
#include <string_view>

namespace url {
class Origin;
}  // namespace url

namespace molt_ai {
namespace vault {

enum class HostMatch {
  kNone,
  kSameSite,
  kExact,
};

// Canonical host for a user-entered site_host. Accepts a bare host
// ("GitHub.com"), host:port, or a full URL ("https://github.com/login").
// Returns "" if it can't be parsed into a host.
std::string CanonicalizeSiteHost(std::string_view site_host);

// Pure comparison of two canonical hosts, given each one's registrable
// domain ("" for IP addresses, single-label hosts and bare registries).
// No Public Suffix List lookups happen here; see MatchSiteHost().
HostMatch ClassifyHostMatch(std::string_view entry_host,
                            std::string_view entry_site,
                            std::string_view page_host,
                            std::string_view page_site);

// Match a stored site_host against a page's canonical host (for example
// url::Origin::host()).
HostMatch MatchSiteHost(std::string_view site_host,
                        std::string_view page_host);

// Whether credentials may be filled into a document with this origin at
// all: https, or http on localhost / 127.0.0.0/8 / [::1].
bool IsFillableOrigin(const url::Origin& origin);

}  // namespace vault
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VAULT_VAULT_HOST_MATCH_H_
