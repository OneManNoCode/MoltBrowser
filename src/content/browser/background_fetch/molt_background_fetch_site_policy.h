// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: Background Fetch scheduled per site, for storage partitions
// whose embedder asks (ContentBrowserClient::
// ShouldScheduleBackgroundFetchesPerSite(): MoltNet's).
//
// At Chromium 156 each partition has one Background Fetch scheduler
// (BackgroundFetchScheduler): at most 2 registrations are active at a time
// (from different storage keys), and at most 2 of their requests download
// at a time, all sites together. An active registration stays active until
// it finishes, however slowly its server sends. So one site's slow fetches
// (two registrations, from its own origin and a subdomain, against a server
// that trickles bytes) kept every other site's fetches from starting, for
// as long as it liked, and the other site could measure how long.
//
// Scheduled per site (by the registration's storage key's top-level site, so
// a frame's service worker counts to the site embedding it):
//  * each site has the limits Chromium gives a whole partition:
//    kMoltBackgroundFetchActivePerSite registrations active (one per storage
//    key, as in Chromium) and kMoltBackgroundFetchRunningPerSite requests
//    downloading; a site's own registrations wait only for its own;
//  * a pending registration becomes active as soon as its site has room,
//    whatever other sites run;
//  * all sites together download at most kMoltBackgroundFetchRunningTotal
//    requests: 64 sites' whole shares, so it is reached only when 64 sites
//    download as much as they may at the same time. Then the next free slot
//    goes to the site with a request waiting that downloads the fewest, the
//    one served least recently among equals (round robin), so no site can
//    keep one from another.
// What a download holds in memory counts already against its site's blob
// share and the memory valve (storage/browser/blob/
// molt_blob_budget_policy.h, BlobTaskProxy::MoltHoldMemory()); the total
// bounds the rest (requests, loaders, the download service's entries).
// The in-memory download service that makes these requests queues none of
// them (components/download/internal/background_service/
// molt_in_memory_download_limits.h).
//
// Std-only, so molt_ai_unittests tests it. Not thread-safe: the scheduler
// uses it on the UI thread.

#ifndef CONTENT_BROWSER_BACKGROUND_FETCH_MOLT_BACKGROUND_FETCH_SITE_POLICY_H_
#define CONTENT_BROWSER_BACKGROUND_FETCH_MOLT_BACKGROUND_FETCH_SITE_POLICY_H_

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace content {

// Chromium's limits for a whole partition (BackgroundFetchScheduler's
// max_active_registrations and max_running_downloads defaults), for each
// site.
inline constexpr int kMoltBackgroundFetchActivePerSite = 2;
inline constexpr int kMoltBackgroundFetchRunningPerSite = 2;

// What all sites together download at a time: 64 sites' whole shares.
inline constexpr int kMoltBackgroundFetchRunningTotal =
    64 * kMoltBackgroundFetchRunningPerSite;

// Each site's active registrations and downloads, and the order in which
// sites waiting for the total are served. Sites are top-level sites,
// serialized.
class MoltBackgroundFetchSites {
 public:
  // Whether a pending registration of `site` may become active.
  bool MayActivate(std::string_view site) const {
    return Find(site).active < kMoltBackgroundFetchActivePerSite;
  }
  void Activated(std::string_view site) { ++Get(site).active; }
  // An active registration of `site` finished.
  void Deactivated(std::string_view site) {
    Site& counts = Get(site);
    if (counts.active > 0) {
      --counts.active;
    }
    Drop(site);
  }

  // Whether a request of `site` may start downloading now: within its own
  // share and the total.
  bool MayStart(std::string_view site) const {
    return total_running_ < kMoltBackgroundFetchRunningTotal &&
           Find(site).running < kMoltBackgroundFetchRunningPerSite;
  }

  // The site whose request starts next among `ready` (the sites of the
  // active registrations with a request to start, repeats allowed), or
  // none: of those that may start, the one downloading the fewest, then the
  // one served least recently, then the first in `ready`.
  std::optional<std::string> PickNext(
      const std::vector<std::string>& ready) const {
    std::optional<std::string> best;
    int best_running = 0;
    uint64_t best_served = 0;
    for (const std::string& site : ready) {
      if (!MayStart(site)) {
        continue;
      }
      const Site& counts = Find(site);
      if (!best || counts.running < best_running ||
          (counts.running == best_running && counts.served < best_served)) {
        best = site;
        best_running = counts.running;
        best_served = counts.served;
      }
    }
    return best;
  }

  // `count` requests of `site` started downloading (served now).
  void Started(std::string_view site, int count = 1) {
    if (count <= 0) {
      return;
    }
    Site& counts = Get(site);
    counts.running += count;
    counts.served = ++serves_;
    total_running_ += count;
  }
  // `count` requests of `site` stopped downloading.
  void Finished(std::string_view site, int count = 1) {
    if (count <= 0) {
      return;
    }
    Site& counts = Get(site);
    const int stopped = count < counts.running ? count : counts.running;
    counts.running -= stopped;
    total_running_ -= stopped;
    Drop(site);
  }

  int active(std::string_view site) const { return Find(site).active; }
  int running(std::string_view site) const { return Find(site).running; }
  int total_running() const { return total_running_; }
  size_t site_count() const { return sites_.size(); }

 private:
  struct Site {
    int active = 0;
    int running = 0;
    // When the site was last served (0: never), for the round robin.
    uint64_t served = 0;
  };

  const Site& Find(std::string_view site) const {
    static constexpr Site kNone{};
    auto it = sites_.find(site);
    return it == sites_.end() ? kNone : it->second;
  }
  Site& Get(std::string_view site) {
    auto it = sites_.find(site);
    if (it == sites_.end()) {
      it = sites_.emplace(std::string(site), Site()).first;
    }
    return it->second;
  }
  // A site with nothing active or running isn't kept (its place in the
  // round robin restarts: never served, so first among equals).
  void Drop(std::string_view site) {
    auto it = sites_.find(site);
    if (it != sites_.end() && it->second.active == 0 &&
        it->second.running == 0) {
      sites_.erase(it);
    }
  }

  std::map<std::string, Site, std::less<>> sites_;
  int total_running_ = 0;
  uint64_t serves_ = 0;
};

}  // namespace content

#endif  // CONTENT_BROWSER_BACKGROUND_FETCH_MOLT_BACKGROUND_FETCH_SITE_POLICY_H_
