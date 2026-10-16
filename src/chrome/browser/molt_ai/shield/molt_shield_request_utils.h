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
#include <set>
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

// ---- Tracking-parameter stripping ($removeparam) ---------------------------

// True if `stripped` (the engine's rewrite of `original`) may be loaded
// instead of `original`: both valid http(s), same origin, path and
// fragment, and `stripped` is shorter -- only query parameters were
// dropped. Guards every place MoltShield acts on a rewrite.
bool IsSafeParamStrip(const GURL& original, const GURL& stripped);

// Subresource requests whose URL the proxying factory may strip before the
// request starts. Not worker / shared-worker / service-worker scripts (the
// script URL is the worker's identity, e.g. for service-worker
// registration and update checks) and not keepalive requests (beacons are
// also seen by the keepalive throttle, which can't rewrite, so leave both
// paths consistent).
bool CanStripParamsFromSubresource(
    network::mojom::RequestDestination destination,
    bool keepalive);

// Navigations that MoltShieldNavigationInterceptor may redirect to a
// stripped URL: GET (an internal 307 would re-send a POST body) to an
// http(s) URL that has a query.
bool CanStripParamsFromNavigation(std::string_view method, const GURL& url);

// Upper bound on internal redirects one navigation gets from parameter
// stripping (a server that keeps re-adding parameters gives up sooner, see
// MoltShieldNavigationInterceptor).
inline constexpr int kMaxParamStripsPerNavigation = 4;

// Server-redirect legs of a subresource request whose new URL the proxying
// factory may strip (as well as CanStripParamsFromSubresource()): the
// request follows redirects itself (a "manual" redirect is handed to the
// page as an opaque-redirect response, an "error" one fails) and the next
// leg is a GET. The strip reaches the network service as FollowRedirect's
// `new_url`, which it turns into an internal 307 that would carry a request
// body along; like navigations, only GETs are stripped.
bool CanStripParamsOnRedirect(network::mojom::RedirectMode redirect_mode,
                              std::string_view new_method);

// Upper bound on parameter strips for one subresource request, its initial
// URL and all its redirect legs together.
inline constexpr int kMaxParamStripsPerSubresource = 4;

// Loop guard for one subresource request's parameter stripping, kept for
// the request's whole life (initial URL and every server-redirect leg). The
// navigation interceptor's rules: a request is moved to a given stripped
// URL at most once; if the server sends it back to a URL whose stripped
// form it was already moved to, the server insists on the parameters, so
// that URL is loaded as is and stripping stops for the rest of the request;
// at most kMaxParamStripsPerSubresource strips overall.
class ParamStripGuard {
 public:
  ParamStripGuard();
  ParamStripGuard(const ParamStripGuard&) = delete;
  ParamStripGuard& operator=(const ParamStripGuard&) = delete;
  ~ParamStripGuard();

  // True if `stripped` (the engine's rewrite of `url`) may be loaded instead
  // of `url` on this leg: IsSafeParamStrip() holds and the guard allows it.
  // Counts the strip when true.
  bool Allow(const GURL& url, const GURL& stripped);

  int strips() const { return strips_; }

 private:
  std::set<GURL> stripped_to_;
  int strips_ = 0;
};

// Parameter stripping for one subresource request in
// MoltShieldProxyingURLLoaderFactory, as a state machine over the
// request's events so the proxy only forwards them:
//
//  * Initial URL: AllowInitialStrip() -- the request starts with the
//    stripped URL and the renderer is not told.
//  * Server redirect to a decorated URL: OnRedirectChecked() remembers the
//    stripped URL; the renderer gets the server's redirect unchanged. When
//    it follows, OnFollowRedirect() returns the stripped URL as
//    FollowRedirect's `new_url`, which the network service applies before
//    the decorated URL is requested, as an internal 307 to the stripped
//    URL. OnNetworkRedirect() recognizes that redirect (the strip took
//    effect: count it); the renderer gets it like any redirect and ends on
//    the URL that was fetched.
//  * Every leg goes through one ParamStripGuard; a `new_url` is only ever
//    an IsSafeParamStrip() rewrite of the redirect target (same origin,
//    path and fragment), as the network service requires.
class SubresourceParamStripper {
 public:
  explicit SubresourceParamStripper(network::mojom::RedirectMode redirect_mode);
  SubresourceParamStripper(const SubresourceParamStripper&) = delete;
  SubresourceParamStripper& operator=(const SubresourceParamStripper&) = delete;
  ~SubresourceParamStripper();

  // The request is about to start with `url`; `stripped` is the engine's
  // rewrite (invalid if none, or stripping is off for this request). True
  // if it should start with `stripped` instead.
  bool AllowInitialStrip(const GURL& url, const GURL& stripped);

  // A server redirect to `new_url` passed the block check; the next request
  // will use `new_method`. `stripped` as above, for `new_url`.
  void OnRedirectChecked(const GURL& new_url,
                         std::string_view new_method,
                         const GURL& stripped);

  // The renderer follows the redirect, with a URL of its own or none. The
  // URL to pass on as `new_url`: the renderer's if it gave one (its leg is
  // then checked and stripped when the network service redirects to it),
  // else the stripped URL, else none.
  std::optional<GURL> OnFollowRedirect(
      const std::optional<GURL>& renderer_new_url);

  // A redirect from the network service, before it is checked. True if it
  // is the internal redirect to the stripped URL OnFollowRedirect() asked
  // for: the decorated URL was never requested.
  bool OnNetworkRedirect(const GURL& new_url);

  // The response arrived. A strip still awaited didn't happen (the network
  // service re-issues some CORS requests on a redirect without `new_url`;
  // the URL is then fetched as the server sent it).
  void OnResponse();

 private:
  const network::mojom::RedirectMode redirect_mode_;
  ParamStripGuard guard_;
  // Stripped form of the redirect the renderer was just sent.
  std::optional<GURL> pending_;
  // The stripped URL the network service was asked to switch to.
  std::optional<GURL> awaiting_;
};

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
