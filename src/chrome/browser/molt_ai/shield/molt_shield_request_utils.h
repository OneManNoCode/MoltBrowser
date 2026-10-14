// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Pure helpers for MoltShield: turning Chromium requests into adblock-rust
// inputs, filter-list sanity checks and metadata, update scheduling, and the
// non-identifying breakage report. No threads, no I/O — unit-tested in
// molt_shield_unittest.cc.

#ifndef CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_REQUEST_UTILS_H_
#define CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_REQUEST_UTILS_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/time/time.h"
#include "chrome/browser/molt_ai/shield/molt_shield_types.h"
#include "services/network/public/mojom/fetch_api.mojom-shared.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace molt_ai::shield {

// adblock-rust request-type strings used outside the destination mapping.
inline constexpr char kTypeDocument[] = "document";
inline constexpr char kTypeSubdocument[] = "sub_frame";
inline constexpr char kTypeWebSocket[] = "websocket";

// Maps a fetch destination to adblock-rust's request type. `keepalive`
// requests with no destination (navigator.sendBeacon, fetch keepalive) are
// "ping", which is what `$ping` filters target.
std::string_view RequestTypeForDestination(
    network::mojom::RequestDestination destination,
    bool keepalive);

// MoltShield only filters http(s) and ws(s); everything else (chrome://,
// data:, blob:, extension schemes, file:) passes untouched.
bool IsFilterableUrl(const GURL& url);

// True unless `url` and `initiator` share a registrable domain (eTLD+1,
// private registries included) — the same definition adblock-rust uses for
// `$third-party`. No or opaque initiator counts as third party.
bool IsThirdParty(const GURL& url, const std::optional<url::Origin>& initiator);

// Builds the engine input; nullopt if `url` isn't filterable.
std::optional<CheckInput> MakeCheckInput(
    const GURL& url,
    const std::optional<url::Origin>& initiator,
    std::string_view request_type,
    std::string_view method);

// ---- Filter lists -------------------------------------------------------

// Bounds for a downloaded list.
inline constexpr int64_t kMaxListBytes = 16 * 1024 * 1024;
inline constexpr size_t kMinFilterLines = 20;

// Cheap sanity check that downloaded bytes are a filter list and not a
// captive-portal page, an HTML error page or a truncated file: valid UTF-8,
// not HTML, at least kMinFilterLines filter lines.
bool LooksLikeFilterList(std::string_view text);

struct ListMetadata {
  std::string title;
  std::string version;  // "Version:" if present, else "Last modified:".
};
// Reads `! Key: value` header comments from the start of a list.
ListMetadata ParseListMetadata(std::string_view text);

// ---- Updates -------------------------------------------------------------

inline constexpr base::TimeDelta kUpdateInterval = base::Days(1);
inline constexpr base::TimeDelta kMaxUpdateJitter = base::Hours(2);
inline constexpr base::TimeDelta kMinStartupDelay = base::Minutes(1);
inline constexpr base::TimeDelta kRetryAfterFailure = base::Hours(1);

// Delay until the next update check: kUpdateInterval after `last_check`,
// shifted by `jitter` (expected in [-kMaxUpdateJitter, kMaxUpdateJitter]) so
// installs don't hit list servers in lockstep, never sooner than
// kMinStartupDelay. A null or future `last_check` (clock moved back) means
// "due now".
base::TimeDelta NextUpdateDelay(base::Time last_check,
                                base::Time now,
                                base::TimeDelta jitter);

// ---- Breakage report -----------------------------------------------------

// Text copied to the clipboard by "Report broken site". Deliberately
// non-identifying: only the site's registrable domain (no path, query,
// subdomain or user data), shield state, the per-page block count, and list
// versions/hashes.
std::string BuildBreakageReport(const GURL& page_url,
                                bool shields_enabled_for_site,
                                int64_t blocked_on_page,
                                const std::vector<ListStatus>& lists,
                                std::string_view browser_version,
                                std::string_view engine_version);

}  // namespace molt_ai::shield

#endif  // CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_REQUEST_UTILS_H_
