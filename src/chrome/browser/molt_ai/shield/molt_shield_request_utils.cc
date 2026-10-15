// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/shield/molt_shield_request_utils.h"

#include <algorithm>

#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "url/url_constants.h"

namespace molt_ai::shield {

using network::mojom::RequestDestination;

std::string_view RequestTypeForDestination(RequestDestination destination,
                                           bool keepalive) {
  switch (destination) {
    case RequestDestination::kDocument:
      return kTypeDocument;
    case RequestDestination::kIframe:
    case RequestDestination::kFrame:
    case RequestDestination::kFencedframe:
      return kTypeSubdocument;
    case RequestDestination::kScript:
    case RequestDestination::kWorker:
    case RequestDestination::kSharedWorker:
    case RequestDestination::kServiceWorker:
    case RequestDestination::kAudioWorklet:
    case RequestDestination::kPaintWorklet:
    case RequestDestination::kSharedStorageWorklet:
    case RequestDestination::kSpeculationRules:
      return "script";
    case RequestDestination::kStyle:
    case RequestDestination::kXslt:
      return "stylesheet";
    case RequestDestination::kImage:
      return "image";
    case RequestDestination::kFont:
      return "font";
    case RequestDestination::kAudio:
    case RequestDestination::kVideo:
    case RequestDestination::kTrack:
      return "media";
    case RequestDestination::kObject:
    case RequestDestination::kEmbed:
      return "object";
    case RequestDestination::kReport:
      return "csp_report";
    case RequestDestination::kEmpty:
    case RequestDestination::kJson:
      // fetch()/XHR. Beacons and keepalive fetches are pings.
      return keepalive ? "ping" : "xmlhttprequest";
    case RequestDestination::kManifest:
    case RequestDestination::kWebBundle:
    case RequestDestination::kWebIdentity:
    case RequestDestination::kDictionary:
    case RequestDestination::kEmailVerification:
      return "other";
  }
  return "other";
}

bool IsFilterableUrl(const GURL& url) {
  return url.is_valid() &&
         (url.SchemeIsHTTPOrHTTPS() || url.SchemeIsWSOrWSS());
}

bool IsThirdParty(const GURL& url,
                  const std::optional<url::Origin>& initiator) {
  if (!initiator.has_value() || initiator->opaque()) {
    return true;
  }
  return !net::registry_controlled_domains::SameDomainOrHost(
      url, *initiator,
      net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);
}

std::optional<CheckInput> MakeCheckInput(
    const GURL& url,
    const std::optional<url::Origin>& initiator,
    std::string_view request_type,
    std::string_view method) {
  if (!IsFilterableUrl(url)) {
    return std::nullopt;
  }
  CheckInput input;
  input.url = url.spec();
  input.host = std::string(url.host());
  if (initiator.has_value() && !initiator->opaque()) {
    input.source_host = initiator->host();
  }
  input.type = std::string(request_type);
  input.third_party = IsThirdParty(url, initiator);
  input.method = method.empty() ? std::string("GET") : std::string(method);
  return input;
}

bool IsSafeParamStrip(const GURL& original, const GURL& stripped) {
  return original.is_valid() && stripped.is_valid() &&
         original.SchemeIsHTTPOrHTTPS() && original.has_query() &&
         stripped.spec().size() < original.spec().size() &&
         url::Origin::Create(original).IsSameOriginWith(
             url::Origin::Create(stripped)) &&
         original.path() == stripped.path() &&
         original.has_ref() == stripped.has_ref() &&
         original.ref() == stripped.ref();
}

bool CanStripParamsFromSubresource(RequestDestination destination,
                                   bool keepalive) {
  if (keepalive) {
    return false;
  }
  switch (destination) {
    case RequestDestination::kWorker:
    case RequestDestination::kSharedWorker:
    case RequestDestination::kServiceWorker:
    // Documents and frames are navigations (MoltShieldNavigationInterceptor).
    case RequestDestination::kDocument:
    case RequestDestination::kIframe:
    case RequestDestination::kFrame:
    case RequestDestination::kFencedframe:
      return false;
    default:
      return true;
  }
}

bool CanStripParamsFromNavigation(std::string_view method, const GURL& url) {
  return method == "GET" && url.is_valid() && url.SchemeIsHTTPOrHTTPS() &&
         url.has_query();
}

bool LooksLikeFilterList(std::string_view text) {
  if (text.empty() || static_cast<int64_t>(text.size()) > kMaxListBytes) {
    return false;
  }
  if (!base::IsStringUTF8(text)) {
    return false;
  }
  std::string_view head = base::TrimWhitespaceASCII(
      text.substr(0, std::min<size_t>(text.size(), 512)), base::TRIM_LEADING);
  if (base::StartsWith(head, "<", base::CompareCase::SENSITIVE)) {
    return false;  // HTML: captive portal, error page, CDN interstitial.
  }
  size_t filters = 0;
  for (std::string_view line : base::SplitStringPiece(
           text, "\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    if (line[0] == '!' || line[0] == '[') {
      continue;
    }
    if (++filters >= kMinFilterLines) {
      return true;
    }
  }
  return false;
}

ListMetadata ParseListMetadata(std::string_view text) {
  ListMetadata meta;
  std::string last_modified;
  // Headers live at the top; uBO only scans the first 1 KB too.
  std::string_view head = text.substr(0, std::min<size_t>(text.size(), 2048));
  for (std::string_view line : base::SplitStringPiece(
           head, "\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    if (line[0] == '[') {
      continue;
    }
    if (line[0] != '!') {
      break;
    }
    std::string_view body =
        base::TrimWhitespaceASCII(line.substr(1), base::TRIM_ALL);
    size_t colon = body.find(": ");
    if (colon == std::string_view::npos) {
      continue;
    }
    std::string_view key = body.substr(0, colon);
    std::string value(
        base::TrimWhitespaceASCII(body.substr(colon + 2), base::TRIM_ALL));
    // uAssets' raw sources carry an unexpanded placeholder.
    if (value == "%timestamp%") {
      continue;
    }
    if (key == "Title" && meta.title.empty()) {
      meta.title = value;
    } else if (key == "Version" && meta.version.empty()) {
      meta.version = value;
    } else if (key == "Last modified" && last_modified.empty()) {
      last_modified = value;
    }
  }
  if (meta.version.empty()) {
    meta.version = last_modified;
  }
  return meta;
}

base::TimeDelta NextUpdateDelay(base::Time last_check,
                                base::Time now,
                                base::TimeDelta jitter) {
  jitter = std::clamp(jitter, -kMaxUpdateJitter, kMaxUpdateJitter);
  if (last_check.is_null() || last_check > now) {
    return kMinStartupDelay;
  }
  base::TimeDelta delay = (last_check + kUpdateInterval + jitter) - now;
  return std::max(delay, kMinStartupDelay);
}

std::string BuildBreakageReport(const GURL& page_url,
                                bool shields_enabled_for_site,
                                int64_t blocked_on_page,
                                const std::vector<ListStatus>& lists,
                                std::string_view browser_version,
                                std::string_view engine_version) {
  std::string site;
  if (page_url.is_valid() && page_url.has_host()) {
    site = net::registry_controlled_domains::GetDomainAndRegistry(
        page_url,
        net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);
    if (site.empty()) {
      site = page_url.HostIsIPAddress() ? "(IP address)"
                                        : std::string(page_url.host());
    }
  } else {
    site = "(unknown)";
  }

  std::string report = base::StrCat(
      {"MoltShield breakage report\n", "Site: ", site, "\n",
       "Shields: ", shields_enabled_for_site ? "on" : "off", "\n",
       "Blocked on this page: ", base::NumberToString(blocked_on_page), "\n",
       "MoltBrowser: ", browser_version, "\n", "Engine: ", engine_version,
       "\n", "Filter lists:\n"});
  for (const ListStatus& list : lists) {
    if (!list.enabled) {
      continue;
    }
    base::StrAppend(
        &report,
        {"  - ", list.id, ": ",
         list.version.empty() ? std::string("(no version)") : list.version,
         list.sha256_hex.empty()
             ? std::string(list.cached ? "" : " (not downloaded)")
             : base::StrCat({" sha256:", list.sha256_hex.substr(0, 12)}),
         "\n"});
  }
  return report;
}

}  // namespace molt_ai::shield
