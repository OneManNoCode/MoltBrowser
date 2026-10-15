// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Pure .onion helpers for MoltNet: onion host detection and Onion-Location
// header validation. No //url, //net or //content dependency, so they are
// unit-tested standalone (onion_util_unittest.cc).
//
// Two different questions, on purpose:
//   * IsOnionHost() is the BROAD check used to keep onion names away from
//     DNS and the network outside MoltNet. It matches every name in the
//     special-use ".onion" domain (RFC 7686), valid onion address or not.
//   * IsV3OnionServiceHost() is the NARROW check used before MoltBrowser
//     offers to open an onion address it was told about (Onion-Location).

#ifndef CHROME_BROWSER_MOLT_AI_TOR_ONION_UTIL_H_
#define CHROME_BROWSER_MOLT_AI_TOR_ONION_UTIL_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace molt_ai {
namespace tor {

// True iff |host| is in the special-use ".onion" domain: "onion" itself or
// any name ending in ".onion", ASCII case-insensitively. Trailing dots are
// ignored ("x.onion." is the same DNS name as "x.onion"). Pass a canonical
// URL host (GURL::host_piece()).
bool IsOnionHost(std::string_view host);

// True iff |host| is a Tor v3 onion service name: "<address>.onion" or
// "<labels>.<address>.onion", where <address> is 56 lowercase base32
// characters ([a-z2-7]) ending in 'd' (the encoded version byte 3) and every
// extra label is 1-63 characters of [a-z0-9-]. Lowercase, no trailing dot
// (the canonical GURL form). Version 2 addresses stopped working in Tor
// 0.4.6 and are rejected.
bool IsV3OnionServiceHost(std::string_view host);

// An Onion-Location target that passed the syntax checks below.
struct OnionLocation {
  std::string scheme;  // "http" or "https"
  std::string host;    // lowercase v3 onion service host
};

// The Onion-Location rules (Tor Browser's, plus stricter syntax):
//   * The header must appear exactly once (|values| holds every occurrence).
//   * The value is an absolute http:// or https:// URL of at most 2048
//     printable ASCII characters (no spaces, controls, backslashes or
//     non-ASCII; surrounding spaces/tabs are trimmed).
//   * Its authority has no user info ('@'), no IP literal ('['), no
//     percent-escapes and at most a numeric port (1-65535).
//   * Its host is a v3 onion service host (IsV3OnionServiceHost, compared
//     lowercase).
// Returns the parsed scheme and host, or nullopt. The caller must still
// parse the value with GURL and require the same scheme and host (so no
// URL-parser differential can smuggle in another host), and must check
// ShouldHonorOnionLocation() for the page that sent it.
std::optional<OnionLocation> ParseOnionLocationHeaderValues(
    const std::vector<std::string>& values);

// Onion-Location is only honored on https pages that are not already onion
// services (Tor Browser's rule). |page_scheme| and |page_host| are the
// canonical (lowercase) URL parts of the committed top-level document.
bool ShouldHonorOnionLocation(std::string_view page_scheme,
                              std::string_view page_host);

}  // namespace tor
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_TOR_ONION_UTIL_H_
