// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/onion_location.h"

#include <optional>
#include <string>
#include <vector>

#include "base/values.h"
#include "chrome/browser/molt_ai/tor/onion_util.h"
#include "net/http/http_response_headers.h"

namespace molt_ai {
namespace tor {

namespace {

constexpr char kOnionLocationHeader[] = "Onion-Location";

// OnionLocationMetaScript() returns at most this many contents, each cut to
// 4096 UTF-16 code units (at most 3 UTF-8 bytes each).
constexpr size_t kMaxMetaElements = 2;
constexpr size_t kMaxMetaContentBytes = 3 * 4096;

// Every Onion-Location value the committed top-level document at |page_url|
// declared (header or <meta>) -> the onion URL, if the rules allow it.
GURL ValidateOnionLocation(const GURL& page_url,
                           const std::vector<std::string>& values) {
  if (!page_url.is_valid() ||
      !ShouldHonorOnionLocation(page_url.scheme(), page_url.host())) {
    return GURL();
  }
  // Exactly one value, strict syntax, v3 onion host.
  std::optional<OnionLocation> parsed = ParseOnionLocationHeaderValues(values);
  if (!parsed) {
    return GURL();
  }
  // GURL must agree with the strict parser, so no parser difference can
  // change which host the user is sent to.
  GURL target(values.front());
  if (!target.is_valid() || target.scheme() != parsed->scheme ||
      target.host() != parsed->host || target.has_username() ||
      target.has_password()) {
    return GURL();
  }
  return target;
}

}  // namespace

GURL GetOnionLocation(const GURL& page_url,
                      const net::HttpResponseHeaders* headers) {
  if (!headers) {
    return GURL();
  }
  // net joins repeated headers with ", " and the parser refuses spaces, so a
  // repeated Onion-Location header is ignored as a whole.
  std::optional<std::string> value =
      headers->GetNormalizedHeader(kOnionLocationHeader);
  if (!value) {
    return GURL();
  }
  return ValidateOnionLocation(page_url, std::vector<std::string>{*value});
}

bool HasOnionLocationHeader(const net::HttpResponseHeaders* headers) {
  return headers && headers->HasHeader(kOnionLocationHeader);
}

GURL GetOnionLocationFromMeta(const GURL& page_url,
                              const std::vector<std::string>& meta_contents) {
  return ValidateOnionLocation(page_url, meta_contents);
}

const char16_t* OnionLocationMetaScript() {
  // Built-ins are taken from the isolated world's own prototypes, so neither
  // page script nor DOM clobbering (e.g. <form name="querySelectorAll">) can
  // stand in for them; at worst the page makes this throw (no infobar).
  return uR"JS((function() {
  var out = [];
  var metas = Document.prototype.querySelectorAll.call(
      document, 'head meta[http-equiv]');
  var getAttribute = Element.prototype.getAttribute;
  for (var i = 0; i < metas.length && out.length < 2; i++) {
    var equiv = getAttribute.call(metas[i], 'http-equiv');
    var content = getAttribute.call(metas[i], 'content');
    if (equiv === null || content === null || equiv.length !== 14) {
      continue;
    }
    var lower = '';
    for (var j = 0; j < equiv.length; j++) {
      var c = equiv.charCodeAt(j);
      lower += String.fromCharCode(c >= 65 && c <= 90 ? c + 32 : c);
    }
    if (lower === 'onion-location') {
      out.push(content.slice(0, 4096));
    }
  }
  return out;
})())JS";
}

std::optional<std::vector<std::string>> OnionLocationMetaContents(
    const base::Value& script_result) {
  if (!script_result.is_list()) {
    return std::nullopt;
  }
  const base::ListValue& list = script_result.GetList();
  if (list.size() > kMaxMetaElements) {
    return std::nullopt;
  }
  std::vector<std::string> contents;
  for (const base::Value& item : list) {
    if (!item.is_string() || item.GetString().size() > kMaxMetaContentBytes) {
      return std::nullopt;
    }
    contents.push_back(item.GetString());
  }
  return contents;
}

bool IsOnionRequestURL(const GURL& url) {
  return url.is_valid() && IsOnionHost(url.host());
}

}  // namespace tor
}  // namespace molt_ai
