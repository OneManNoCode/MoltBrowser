// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// GURL / net glue around onion_util.h, split out of the content-layer code
// so molt_ai_unittests can test it with a real GURL and
// net::HttpResponseHeaders:
//   * GetOnionLocation — the validated Onion-Location of a top-level
//     response (rules in onion_util.h, plus the check that GURL parses the
//     header to the same scheme and host as the strict parser did);
//   * IsOnionRequestURL — whether a request URL names an onion host
//     (onion_request_blocker.h).

#ifndef CHROME_BROWSER_MOLT_AI_TOR_ONION_LOCATION_H_
#define CHROME_BROWSER_MOLT_AI_TOR_ONION_LOCATION_H_

#include "url/gurl.h"

namespace net {
class HttpResponseHeaders;
}  // namespace net

namespace molt_ai {
namespace tor {

// The onion URL that |headers| (the response for the committed top-level
// document at |page_url|) validly offers, or an empty GURL.
GURL GetOnionLocation(const GURL& page_url,
                      const net::HttpResponseHeaders* headers);

// True iff |url| is valid and its host is in the special-use .onion domain
// (IsOnionHost: "onion" or "*.onion", trailing dots ignored), whatever the
// scheme: http(s), ws(s) or anything else that could reach the network.
bool IsOnionRequestURL(const GURL& url);

}  // namespace tor
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_TOR_ONION_LOCATION_H_
