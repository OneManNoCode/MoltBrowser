// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// GetOnionLocation — the validated Onion-Location of a top-level response.
// The rules are in onion_util.h; this adds the //url and //net glue and the
// check that GURL parses the header to the same scheme and host as the strict
// parser did. Split out of OnionLocationTabHelper so molt_ai_unittests can
// test it with a real GURL and net::HttpResponseHeaders.

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

}  // namespace tor
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_TOR_ONION_LOCATION_H_
