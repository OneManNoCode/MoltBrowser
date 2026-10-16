// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// GURL / net glue around onion_util.h, split out of the content-layer code
// so molt_ai_unittests can test it with a real GURL, base::Value and
// net::HttpResponseHeaders:
//   * GetOnionLocation — the validated Onion-Location of a top-level
//     response (rules in onion_util.h, plus the check that GURL parses the
//     header to the same scheme and host as the strict parser did);
//   * GetOnionLocationFromMeta — the same for <meta http-equiv=
//     "onion-location" content="..."> in the document's <head> (Tor
//     Browser honours both), read by OnionLocationTabHelper;
//   * IsOnionRequestURL — whether a request URL names an onion host
//     (onion_request_blocker.h).

#ifndef CHROME_BROWSER_MOLT_AI_TOR_ONION_LOCATION_H_
#define CHROME_BROWSER_MOLT_AI_TOR_ONION_LOCATION_H_

#include <optional>
#include <string>
#include <vector>

#include "url/gurl.h"

namespace base {
class Value;
}  // namespace base

namespace net {
class HttpResponseHeaders;
}  // namespace net

namespace molt_ai {
namespace tor {

// The onion URL that |headers| (the response for the committed top-level
// document at |page_url|) validly offers, or an empty GURL.
GURL GetOnionLocation(const GURL& page_url,
                      const net::HttpResponseHeaders* headers);

// True iff |headers| has any Onion-Location header, valid or not. Such a
// response's <meta> elements are ignored: the header is authoritative.
bool HasOnionLocationHeader(const net::HttpResponseHeaders* headers);

// The onion URL offered by the document at |page_url| through <meta
// http-equiv="onion-location"> elements in its <head>; |meta_contents| holds
// the content attribute of each such element. The header's rules apply:
// exactly one element, an https page that isn't an onion service, and a
// strict absolute http(s) URL with a v3 onion host that GURL parses to the
// same scheme and host. Empty GURL otherwise.
GURL GetOnionLocationFromMeta(const GURL& page_url,
                              const std::vector<std::string>& meta_contents);

// JavaScript, run by OnionLocationTabHelper in an isolated world of the
// committed top-level document at DOMContentLoaded, that returns the content
// attribute of the first two <meta> elements inside a <head> whose
// http-equiv is ASCII-case-insensitively "onion-location" (two are enough
// to tell "exactly one"), each cut to 4096 characters. Blink applies a
// <meta http-equiv> wherever in the document it is inserted (by the parser
// or by script); like Blink's own rule for Content-Security-Policy, only
// those in <head> count. Page scripts can't change what the isolated world
// sees of the DOM API, but they can add or remove <meta> elements: a script
// running in the page can navigate anywhere anyway, and the infobar never
// opens anything without a click.
const char16_t* OnionLocationMetaScript();

// The strings in |script_result| (OnionLocationMetaScript()'s result, from
// the renderer, so untrusted): nullopt unless it is a list of at most two
// strings of at most 4096 characters each.
std::optional<std::vector<std::string>> OnionLocationMetaContents(
    const base::Value& script_result);

// True iff |url| is valid and its host is in the special-use .onion domain
// (IsOnionHost: "onion" or "*.onion", trailing dots ignored), whatever the
// scheme: http(s), ws(s) or anything else that could reach the network.
bool IsOnionRequestURL(const GURL& url);

}  // namespace tor
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_TOR_ONION_LOCATION_H_
