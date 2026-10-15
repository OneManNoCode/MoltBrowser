// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/onion_location.h"

#include <optional>
#include <string>
#include <vector>

#include "chrome/browser/molt_ai/tor/onion_util.h"
#include "net/http/http_response_headers.h"

namespace molt_ai {
namespace tor {

namespace {
constexpr char kOnionLocationHeader[] = "Onion-Location";
}  // namespace

GURL GetOnionLocation(const GURL& page_url,
                      const net::HttpResponseHeaders* headers) {
  if (!headers || !page_url.is_valid() ||
      !ShouldHonorOnionLocation(page_url.scheme(), page_url.host())) {
    return GURL();
  }
  // net joins repeated headers with ", " and the parser refuses spaces, so a
  // repeated Onion-Location header is ignored as a whole.
  std::optional<std::string> value =
      headers->GetNormalizedHeader(kOnionLocationHeader);
  if (!value) {
    return GURL();
  }
  std::optional<OnionLocation> parsed =
      ParseOnionLocationHeaderValues(std::vector<std::string>{*value});
  if (!parsed) {
    return GURL();
  }
  // GURL must agree with the strict parser, so no parser difference can
  // change which host the user is sent to.
  GURL target(*value);
  if (!target.is_valid() || target.scheme() != parsed->scheme ||
      target.host() != parsed->host || target.has_username() ||
      target.has_password()) {
    return GURL();
  }
  return target;
}

}  // namespace tor
}  // namespace molt_ai
