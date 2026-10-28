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
// a frame's service worker counts to the site embedding it, grouped as DOM
// storage groups keys: components/services/storage/dom_storage/
// molt_dom_storage_site.h), each site has the limits Chromium gives a whole
// partition, and nothing else:
//  * kMoltBackgroundFetchActivePerSite registrations active (one per storage
//    key, as in Chromium), the others waiting in the order they were made;
//  * kMoltBackgroundFetchRunningPerSite requests downloading, taken from its
//    active registrations in turn (the one served least recently first, as
//    Chromium takes them);
//  * no total for all sites. A total that sites share can be filled by a
//    group of them (a third party embedded on many sites counts as each of
//    those sites), and another site would then wait and learn that. So a
//    site's registration becomes active, and its request starts, as soon as
//    its own site has room, whatever every other site runs: what a site's
//    fetches do depends on its own registrations only. Scheduling a site
//    looks at that site's own registrations only, so what it costs doesn't
//    depend on other sites either.
// What the requests hold is bounded per site (a download's response counts
// against its site's blob share from its first byte: storage/browser/blob/
// molt_blob_budget_policy.h, BlobTaskProxy::MoltHoldMemory()), and all of it
// by the memory valve, as storage and blobs are: once all sites together
// download more than kMoltBackgroundFetchRunningFloor requests (64 sites'
// whole shares), each further request that starts tells the embedder
// (ContentBrowserClient::GetBackgroundFetchGrowthBeyondFloorCallback()),
// which checks the machine's memory apart from any request and ends the
// whole identity if the machine is short of memory
// (chrome/browser/molt_ai/tor/molt_net_memory_valve_policy.h). No request
// waits because of other sites, and none is refused.
// The in-memory download service that makes these requests queues none of
// them (components/download/internal/background_service/
// molt_in_memory_download_limits.h).
//
// Std-only, so molt_ai_unittests tests it. Not thread-safe: the scheduler
// uses it on the UI thread.

#ifndef CONTENT_BROWSER_BACKGROUND_FETCH_MOLT_BACKGROUND_FETCH_SITE_POLICY_H_
#define CONTENT_BROWSER_BACKGROUND_FETCH_MOLT_BACKGROUND_FETCH_SITE_POLICY_H_

#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace content {

// Chromium's limits for a whole partition (BackgroundFetchScheduler's
// max_active_registrations and max_running_downloads defaults), for each
// site.
inline constexpr int kMoltBackgroundFetchActivePerSite = 2;
inline constexpr int kMoltBackgroundFetchRunningPerSite = 2;

// Past this many requests downloading for all sites together (64 sites'
// whole shares), each request that starts tells the memory valve. A floor
// for the valve only: nothing waits or is refused there.
inline constexpr int kMoltBackgroundFetchRunningFloor =
    64 * kMoltBackgroundFetchRunningPerSite;

// Whether a request starting when all sites together download
// `total_running` (counting it) tells the memory valve.
constexpr bool MoltBackgroundFetchBeyondFloor(int total_running) {
  return total_running > kMoltBackgroundFetchRunningFloor;
}

// Each site's registrations and downloads. Sites are top-level sites,
// serialized; registrations are named by their unique id, and carry their
// storage key, serialized.
class MoltBackgroundFetchSites {
 public:
  MoltBackgroundFetchSites();
  MoltBackgroundFetchSites(const MoltBackgroundFetchSites&) = delete;
  MoltBackgroundFetchSites& operator=(const MoltBackgroundFetchSites&) =
      delete;
  ~MoltBackgroundFetchSites();

  // A registration `id` of `site`, for storage key `key`, was made: it
  // waits until Activate() makes it active.
  void AddPending(std::string_view site,
                  std::string_view id,
                  std::string_view key) {
    if (site_of_.contains(id)) {
      return;
    }
    Get(site).pending.push_back({std::string(id), std::string(key)});
    site_of_.emplace(std::string(id), std::string(site));
  }

  // A registration resumed at startup is active at once.
  void AddActive(std::string_view site,
                 std::string_view id,
                 std::string_view key) {
    if (site_of_.contains(id)) {
      return;
    }
    Site& state = Get(site);
    state.active.push_back({std::string(id), std::string(key)});
    state.active_keys.insert(std::string(key));
    site_of_.emplace(std::string(id), std::string(site));
  }

  // Makes `site`'s pending registrations active while it has room, in the
  // order they were made, one per storage key (as in Chromium), and returns
  // their ids. Looks at `site`'s registrations only.
  std::vector<std::string> Activate(std::string_view site) {
    std::vector<std::string> activated;
    auto it = sites_.find(site);
    if (it == sites_.end()) {
      return activated;
    }
    Site& state = it->second;
    for (auto pending = state.pending.begin();
         pending != state.pending.end() &&
         static_cast<int>(state.active.size()) <
             kMoltBackgroundFetchActivePerSite;) {
      if (state.active_keys.contains(pending->key)) {
        ++pending;
        continue;
      }
      state.active_keys.insert(pending->key);
      activated.push_back(pending->id);
      state.active.push_back(std::move(*pending));
      pending = state.pending.erase(pending);
    }
    return activated;
  }

  // The active registration of `site` whose next request starts now, or
  // none: none while the site downloads its share; else, of its active
  // registrations with a request left (`has_more`), the one served least
  // recently, which then goes last. Looks at `site`'s registrations only.
  std::optional<std::string> NextToStart(
      std::string_view site,
      const std::function<bool(const std::string&)>& has_more) {
    auto it = sites_.find(site);
    if (it == sites_.end() ||
        it->second.running >= kMoltBackgroundFetchRunningPerSite) {
      return std::nullopt;
    }
    std::deque<Registration>& active = it->second.active;
    for (auto registration = active.begin(); registration != active.end();
         ++registration) {
      if (!has_more(registration->id)) {
        continue;
      }
      Registration next = std::move(*registration);
      active.erase(registration);
      active.push_back(std::move(next));
      return active.back().id;
    }
    return std::nullopt;
  }

  // `count` requests of `site` started downloading. Returns whether the
  // memory valve hears of it (MoltBackgroundFetchBeyondFloor()).
  bool Started(std::string_view site, int count = 1) {
    if (count <= 0) {
      return false;
    }
    Get(site).running += count;
    total_running_ += count;
    return MoltBackgroundFetchBeyondFloor(total_running_);
  }

  // `count` requests of `site` stopped downloading.
  void Finished(std::string_view site, int count = 1) {
    auto it = sites_.find(site);
    if (count <= 0 || it == sites_.end()) {
      return;
    }
    Site& state = it->second;
    const int stopped = count < state.running ? count : state.running;
    state.running -= stopped;
    total_running_ -= stopped;
    DropIfEmpty(it);
  }

  // Registration `id` finished or was given up, active or pending. Its
  // requests still downloading are Finished() apart.
  void Remove(std::string_view id) {
    auto owner = site_of_.find(id);
    if (owner == site_of_.end()) {
      return;
    }
    auto it = sites_.find(owner->second);
    site_of_.erase(owner);
    if (it == sites_.end()) {
      return;
    }
    Site& state = it->second;
    for (auto pending = state.pending.begin(); pending != state.pending.end();
         ++pending) {
      if (pending->id == id) {
        state.pending.erase(pending);
        DropIfEmpty(it);
        return;
      }
    }
    for (auto active = state.active.begin(); active != state.active.end();
         ++active) {
      if (active->id == id) {
        if (auto key = state.active_keys.find(active->key);
            key != state.active_keys.end()) {
          state.active_keys.erase(key);
        }
        state.active.erase(active);
        DropIfEmpty(it);
        return;
      }
    }
  }

  // The site registration `id` belongs to, if it is pending or active.
  std::optional<std::string> SiteOf(std::string_view id) const {
    auto it = site_of_.find(id);
    if (it == site_of_.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  int active(std::string_view site) const {
    auto it = sites_.find(site);
    return it == sites_.end() ? 0 : static_cast<int>(it->second.active.size());
  }
  int pending(std::string_view site) const {
    auto it = sites_.find(site);
    return it == sites_.end() ? 0
                              : static_cast<int>(it->second.pending.size());
  }
  int running(std::string_view site) const {
    auto it = sites_.find(site);
    return it == sites_.end() ? 0 : it->second.running;
  }
  int total_running() const { return total_running_; }
  size_t site_count() const { return sites_.size(); }
  size_t registration_count() const { return site_of_.size(); }

 private:
  struct Registration {
    std::string id;
    std::string key;
  };
  struct Site {
    std::deque<Registration> pending;
    // In the order their next request starts (least recently served first).
    std::deque<Registration> active;
    // The active registrations' storage keys (a key twice only for
    // registrations resumed at startup).
    std::multiset<std::string> active_keys;
    int running = 0;
  };
  using Sites = std::map<std::string, Site, std::less<>>;

  Site& Get(std::string_view site) {
    auto it = sites_.find(site);
    if (it == sites_.end()) {
      it = sites_.emplace(std::string(site), Site()).first;
    }
    return it->second;
  }
  // A site with nothing pending, active or running isn't kept.
  void DropIfEmpty(Sites::iterator it) {
    if (it->second.pending.empty() && it->second.active.empty() &&
        it->second.running == 0) {
      sites_.erase(it);
    }
  }

  Sites sites_;
  std::map<std::string, std::string, std::less<>> site_of_;
  int total_running_ = 0;
};

inline MoltBackgroundFetchSites::MoltBackgroundFetchSites() = default;
inline MoltBackgroundFetchSites::~MoltBackgroundFetchSites() = default;

}  // namespace content

#endif  // CONTENT_BROWSER_BACKGROUND_FETCH_MOLT_BACKGROUND_FETCH_SITE_POLICY_H_
