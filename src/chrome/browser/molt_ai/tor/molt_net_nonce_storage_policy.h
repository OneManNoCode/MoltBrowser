// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet: storage keyed by a page's nonce goes with the page, and a
// top-level site holds a bounded number of such groups at once.
//
// A credentialless iframe's storage key (blink::StorageKey::
// CreateWithNonce()) carries its page's nonce, one for each page (PageImpl::
// credentialless_iframes_nonce()); a fenced frame's carries one for each of
// its configurations, which its own navigations keep. MoltNet counts the
// storage of such keys with their nonce
// (components/services/storage/dom_storage/molt_dom_storage_site.h,
// storage/browser/quota/molt_storage_budget.h), so a page that reloads
// itself got a fresh group each time: its own DOM storage total and write
// allowance, its own quota share. At Chromium 156 such storage outlives its
// page until the profile ends off the record (on disk, until a sweep of
// storage keyed by a nonce or under an opaque top-level site not used for a
// day, once a session: QuotaDatabase::GetExpiredBuckets(),
// LocalStorageImpl::OnGotMetaDataToDeleteStaleStorageAreas()), though
// nothing can reach it once its page is gone: a nonce is never used again.
// MoltNet keeps no database, so areas holding items were never dropped and
// the per-group records grew with every reload, bounded only by the memory
// valve ending the identity.
//
// So, for MoltNet's pages (MoltNetNonceStorageTabHelper):
//  * when a page goes (its PageImpl, after it leaves the back/forward cache),
//    the storage of its nonce goes with it: the localStorage and
//    sessionStorage areas of its storage keys in every tab (copies a tab it
//    opened made included), their per-group records, its storage keys'
//    buckets (IndexedDB, Cache Storage, the origin-private file system) and
//    the quota counts of its group. Nothing that could still use the storage
//    is left: only the page's frames and their workers have its nonce. A
//    nonce that successive pages share (a fenced frame navigating itself
//    keeps its nonce) goes when the last of them goes
//    (MoltNetNoncePageCounts);
//  * a top-level site holds at most kMoltNetNonceGroupsPerSite such groups
//    at once (the pages of the site, in its tabs and their back/forward
//    caches, whose frames use storage keyed by their nonce). Past it, the
//    site's oldest such page's group is closed: what it holds is dropped as
//    if the page had gone, and from then on its frames can't make any of it
//    again until the page goes: their DOM storage writes are refused (as a
//    write past a quota is), their quota-managed storage gets no bucket and
//    no room (IndexedDB, Cache Storage and the origin-private file system
//    fail to open or to grow with a quota error, a service worker or a
//    Storage Bucket isn't made), and the cookie store refuses their cookies
//    (as cookies blocked by settings are; net/cookies/
//    molt_nonce_cookie_policy.h). The page itself and its frames keep
//    working, statelessly. What it depends on: the site's own pages only,
//    never another site's (a page served sandboxed counts to the site it
//    came from: MoltNetNonceGroupSite());
//  * a group's records are bounded too: the storage keys its frames use
//    (one per origin), kMoltNetNonceStorageKeysPerGroup at most; a group
//    whose frames reach more is closed the same way.
//
// What else is kept under a page's nonce, at Chromium 156, and goes the same
// way, at the same moment (MoltNetNonceStorageTabHelper):
//  * cookies: a frame with a nonce keeps its cookies in partitions of its
//    own (net::CookiePartitionKey with the nonce: one for each site a frame
//    of the page, or one of its navigations, reached;
//    CookiePartitionKey::FromNetworkIsolationKey() uses the frame's site for
//    a nonce). 156 never deletes them: the network service keeps them in
//    memory until the profile ends, and it has no total for partitioned
//    cookies (only 180 cookies, 10 KiB, per partition and domain). MoltNet's
//    cookie store holds at most what molt_nonce_cookie_policy.h says under
//    each nonce and finds a nonce's partitions itself; the network context
//    deletes them by the nonce (network::mojom::NetworkContext::
//    MoltDropNonce()) and refuses further ones until, a while after the
//    page went (kMoltNetNonceCookieSweepSeconds, past the longest a request
//    of the page outlives it), it deletes them once more and forgets the
//    nonce (MoltNetNonceSweepsCookiesLater());
//  * the network state kept under the nonce's network keys: idle
//    connections and HTTP/2 sessions, TLS sessions, server properties, CORS
//    preflight results and HTTP authentication entries, dropped by the
//    network context with the cookies, and again with them later
//    (services/network/molt_nonce_drop_policy.h says how each is keyed and
//    dropped);
//  * media-license (CDM) data, kept per storage key: deleted for each of
//    the nonce's storage keys (StoragePartition::GetCdmStorageDataModel());
//    MoltNet pages have no key system to make any (NotSupportedError);
//  * the clipboard's salt for each storage key (StoragePartitionImpl::
//    GetPartitionUUIDPerStorageKey(): a table entry per key, for the
//    partition's life): in MoltNet's partition a key with a nonce gets a
//    salt derived from the key instead, so the table keeps none for it (one
//    with an opaque origin keeps Chromium's entry, made only when a frame
//    that may read the clipboard asks);
//  * and what round 30 drops: DOM storage, and the storage keys' buckets
//    (IndexedDB, Cache Storage, the origin-private file system, service
//    worker registrations and their Background Fetches).
// Kept by 156 for no nonce at all: HTTP cache entries (a request with a
// nonce is never cached: HttpCache::GenerateCacheKey()), shared
// dictionaries (none for a nonce: SharedDictionaryIsolationKey::
// MaybeCreate()), host-cache entries (MoltNet's names are resolved by Tor),
// Reporting endpoints and Network Error Logging policies (not built into
// MoltBrowser); blob URLs go with the document that made them
// (BlobURLStoreImpl), Web Locks and BroadcastChannels with their frames.
// Kept under the nonce and not dropped, as no page can use them again once
// the nonce's page is gone: marks of broken alternative services, which
// expire on their own.
//
// Std-only, so molt_ai_unittests tests it. Not thread-safe: the browser uses
// it on the UI thread.

#ifndef CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_NONCE_STORAGE_POLICY_H_
#define CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_NONCE_STORAGE_POLICY_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace molt_ai::tor {

// How many of a top-level site's pages may hold storage keyed by their nonce
// at once: far more than the pages of one site with credentialless frames a
// person keeps open (a tab and its back/forward cache hold seven at most).
inline constexpr size_t kMoltNetNonceGroupsPerSite = 32;

// How many storage keys (one per origin) a group's frames may use before the
// group is closed: far more than the origins a page's credentialless frames
// load, so the browser's record of a page is bounded.
inline constexpr size_t kMoltNetNonceStorageKeysPerGroup = 256;

// Whether a group that holds `keys` storage keys is closed for another one.
constexpr bool MoltNetNonceGroupFull(size_t keys) {
  return keys >= kMoltNetNonceStorageKeysPerGroup;
}

// How long a request of a page may outlive it at Chromium 156: a keepalive
// request (fetch() with keepalive, sendBeacon(), fetchLater()) is cancelled
// this long after its page's renderer disconnects (KeepAliveURLLoader's
// disconnected-loader timeout), and it is retried only while a page of the
// same network key is active, which no page is once its nonce's page is gone
// (KeepAliveURLLoader::AttemptRetryIfAllowed()).
inline constexpr int64_t kMoltNetKeepaliveOutlivesPageSeconds = 30;

// When the network context drops a gone page's nonce once more and forgets
// it (its cookies deleted again and no longer refused, its network state
// dropped again): past the longest one of its requests may outlive it.
inline constexpr int64_t kMoltNetNonceCookieSweepSeconds = 60;
static_assert(kMoltNetNonceCookieSweepSeconds >
              kMoltNetKeepaliveOutlivesPageSeconds);

// Whether a nonce group is dropped once more and forgotten
// kMoltNetNonceCookieSweepSeconds later: when its page has gone; a group
// closed past its site's bound belongs to a page that lives on, whose nonce
// stays refused until it goes.
constexpr bool MoltNetNonceSweepsCookiesLater(bool closed) {
  return !closed;
}

// The site whose bound a page's nonce groups count to, from the origin of
// the page's main document (an outer page's, for a fenced frame): its
// top-level site, serialized (`site`, net::SchemefulSite::Serialize()). An
// opaque origin (`opaque`: a page its server sandboxed) serializes as
// "null" for every such page of every site, so it counts to the site it
// came from instead (`precursor_site`, the serialized site of its precursor;
// empty if it has none): a site's sandboxed pages count with its other
// pages, and never close another site's. One with no precursor counts on
// its own (`page`, its nonce).
inline std::string MoltNetNonceGroupSite(bool opaque,
                                         std::string_view site,
                                         std::string_view precursor_site,
                                         std::string_view page) {
  if (!opaque) {
    return std::string(site);
  }
  if (!precursor_site.empty()) {
    return std::string(precursor_site);
  }
  // Never a serialized site, which has a scheme ("https://...", "null").
  return "page " + std::string(page);
}

// The open nonce groups of each top-level site, oldest first. `Nonce` is
// base::UnguessableToken in the browser.
template <typename Nonce>
class MoltNetNonceGroups {
 public:
  // A page of top-level site `site` uses storage keyed by `nonce`. Returns
  // the nonce to close, if `site` now holds more than its bound: the oldest
  // of its other open ones.
  std::optional<Nonce> Added(const std::string& site, const Nonce& nonce) {
    if (site_of_.contains(nonce)) {
      return std::nullopt;
    }
    site_of_.emplace(nonce, site);
    std::deque<Nonce>& open = open_[site];
    open.push_back(nonce);
    if (open.size() <= kMoltNetNonceGroupsPerSite) {
      return std::nullopt;
    }
    const Nonce oldest = open.front();
    open.pop_front();
    return oldest;
  }

  // The page that owns `nonce` is gone; its group with it, open or closed.
  void Gone(const Nonce& nonce) {
    auto owner = site_of_.find(nonce);
    if (owner == site_of_.end()) {
      return;
    }
    auto site = open_.find(owner->second);
    site_of_.erase(owner);
    if (site == open_.end()) {
      return;
    }
    std::deque<Nonce>& open = site->second;
    for (auto it = open.begin(); it != open.end(); ++it) {
      if (*it == nonce) {
        open.erase(it);
        break;
      }
    }
    if (open.empty()) {
      open_.erase(site);
    }
  }

  // Closes `nonce`'s group though its site isn't past its bound (its page's
  // frames used more than kMoltNetNonceStorageKeysPerGroup storage keys): it
  // no longer counts to its site's bound, and stays its page's until the
  // page goes. Returns whether it was open.
  bool Close(const Nonce& nonce) {
    auto owner = site_of_.find(nonce);
    if (owner == site_of_.end()) {
      return false;
    }
    auto site = open_.find(owner->second);
    if (site == open_.end()) {
      return false;
    }
    std::deque<Nonce>& open = site->second;
    for (auto it = open.begin(); it != open.end(); ++it) {
      if (*it == nonce) {
        open.erase(it);
        if (open.empty()) {
          open_.erase(site);
        }
        return true;
      }
    }
    return false;
  }

  bool IsTracked(const Nonce& nonce) const { return site_of_.contains(nonce); }
  size_t open_count(const std::string& site) const {
    auto it = open_.find(site);
    return it == open_.end() ? 0 : it->second.size();
  }
  size_t site_count() const { return open_.size(); }
  size_t nonce_count() const { return site_of_.size(); }

 private:
  std::map<std::string, std::deque<Nonce>> open_;
  std::map<Nonce, std::string> site_of_;
};

// How many pages use each nonce. A credentialless frame's nonce is its
// page's alone, but a fenced frame keeps its nonce when it navigates itself,
// so successive pages (the frame's documents) can share one: its group goes
// only when the last of them goes. (At Chromium 156 no page can make a
// fenced frame: FencedFrameConfig has no public constructor, and content has
// neither shared storage nor interest groups. This keeps the tracking right
// for when a rebase brings them back.)
template <typename Nonce>
class MoltNetNoncePageCounts {
 public:
  // A page uses `nonce` (once per page).
  void Uses(const Nonce& nonce) { ++counts_[nonce]; }

  // A page that used `nonce` went. Returns whether it was the last.
  bool Left(const Nonce& nonce) {
    auto it = counts_.find(nonce);
    if (it == counts_.end()) {
      return true;
    }
    if (--it->second > 0) {
      return false;
    }
    counts_.erase(it);
    return true;
  }

  size_t PagesOf(const Nonce& nonce) const {
    auto it = counts_.find(nonce);
    return it == counts_.end() ? 0 : it->second;
  }
  size_t size() const { return counts_.size(); }

 private:
  std::map<Nonce, size_t> counts_;
};

}  // namespace molt_ai::tor

#endif  // CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_NONCE_STORAGE_POLICY_H_
