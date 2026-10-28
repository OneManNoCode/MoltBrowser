// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet's Background Fetch scheduled per site
// (content/browser/background_fetch/molt_background_fetch_site_policy.h)
// and the in-memory download service's limits that match it
// (components/download/internal/background_service/
// molt_in_memory_download_limits.h), both std-only.

#include "content/browser/background_fetch/molt_background_fetch_site_policy.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "components/download/internal/background_service/molt_in_memory_download_limits.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai::tor {
namespace {

using content::kMoltBackgroundFetchActivePerSite;
using content::kMoltBackgroundFetchRunningFloor;
using content::kMoltBackgroundFetchRunningPerSite;
using content::MoltBackgroundFetchBeyondFloor;
using content::MoltBackgroundFetchSites;

std::string Site(int i) {
  return "https://site" + std::to_string(i) + ".test";
}

// Requests left per registration, as the scheduler's controllers say.
struct Requests {
  std::map<std::string, int> left;
  std::function<bool(const std::string&)> HasMore() {
    return [this](const std::string& id) {
      auto it = left.find(id);
      return it != left.end() && it->second > 0;
    };
  }
};

// Starts what `site` may start now, as BackgroundFetchScheduler::
// MoltSchedule() does; returns the registrations whose request started.
std::vector<std::string> Schedule(MoltBackgroundFetchSites& sites,
                                  const std::string& site,
                                  Requests& requests,
                                  int* valve_heard = nullptr) {
  sites.Activate(site);
  std::vector<std::string> started;
  while (std::optional<std::string> id =
             sites.NextToStart(site, requests.HasMore())) {
    --requests.left[*id];
    if (sites.Started(site) && valve_heard) {
      ++*valve_heard;
    }
    started.push_back(*id);
  }
  return started;
}

// Each site has Chromium's limits for a whole partition; the valve's floor
// is 64 sites' whole shares.
TEST(MoltNetBackgroundFetchSchedulingTest, Limits) {
  EXPECT_EQ(kMoltBackgroundFetchActivePerSite, 2);
  EXPECT_EQ(kMoltBackgroundFetchRunningPerSite, 2);
  EXPECT_EQ(kMoltBackgroundFetchRunningFloor, 128);
  EXPECT_FALSE(MoltBackgroundFetchBeyondFloor(128));
  EXPECT_TRUE(MoltBackgroundFetchBeyondFloor(129));
}

// A site's registrations wait only for its own: two active (one per storage
// key), the others wait in order; another site's become active whatever
// the first runs.
TEST(MoltNetBackgroundFetchSchedulingTest, ActivePerSiteAndKey) {
  MoltBackgroundFetchSites sites;
  const std::string a = "https://a.test";
  const std::string b = "https://b.test";
  sites.AddPending(a, "a1", "https://a.test^a");
  sites.AddPending(a, "a2", "https://a.test^a");  // Same key as a1.
  sites.AddPending(a, "a3", "https://x.a.test^a");
  sites.AddPending(a, "a4", "https://y.a.test^a");
  sites.AddPending(b, "b1", "https://b.test^b");
  EXPECT_EQ(sites.Activate(a), (std::vector<std::string>{"a1", "a3"}));
  EXPECT_EQ(sites.active(a), 2);
  EXPECT_EQ(sites.pending(a), 2);
  EXPECT_EQ(sites.Activate(b), (std::vector<std::string>{"b1"}));
  // a1 finishes: the next of its key comes first, then a4 waits for room.
  sites.Remove("a1");
  EXPECT_EQ(sites.Activate(a), (std::vector<std::string>{"a2"}));
  sites.Remove("a3");
  EXPECT_EQ(sites.Activate(a), (std::vector<std::string>{"a4"}));
  EXPECT_EQ(sites.pending(a), 0);
}

// Chromium 156 runs two downloads for all sites: a greedy site's two slow
// ones keep a polite site's from starting. Per site, the polite site's
// requests start at once, whatever the greedy site runs.
TEST(MoltNetBackgroundFetchSchedulingTest, AGreedySiteDoesntDelayAnother) {
  MoltBackgroundFetchSites sites;
  Requests requests;
  const std::string greedy = "https://greedy.test";
  const std::string polite = "https://polite.test";
  sites.AddPending(greedy, "g1", "https://greedy.test^g");
  sites.AddPending(greedy, "g2", "https://x.greedy.test^g");
  requests.left["g1"] = 1000;
  requests.left["g2"] = 1000;
  EXPECT_EQ(Schedule(sites, greedy, requests).size(), 2u);
  // The greedy site's share is full; its downloads never finish.
  EXPECT_TRUE(Schedule(sites, greedy, requests).empty());
  sites.AddPending(polite, "p1", "https://polite.test^p");
  requests.left["p1"] = 2;
  EXPECT_EQ(Schedule(sites, polite, requests),
            (std::vector<std::string>{"p1", "p1"}));
  EXPECT_EQ(sites.running(polite), 2);
  EXPECT_EQ(sites.running(greedy), 2);
}

// No total a group of sites can fill: with 64 sites (a third party embedded
// on each) downloading their whole shares through slow or paused fetches,
// a 65th, and a 1000th, start theirs at once. The valve hears of each
// request past the floor; nothing waits or is refused there.
TEST(MoltNetBackgroundFetchSchedulingTest, NoTotalToFill) {
  MoltBackgroundFetchSites sites;
  Requests requests;
  int valve_heard = 0;
  for (int i = 0; i < 64; ++i) {
    const std::string id = "r" + std::to_string(i);
    sites.AddPending(Site(i), id, Site(i) + "^tracker");
    requests.left[id] = 1000;
    ASSERT_EQ(Schedule(sites, Site(i), requests, &valve_heard).size(), 2u);
  }
  EXPECT_EQ(sites.total_running(), kMoltBackgroundFetchRunningFloor);
  EXPECT_EQ(valve_heard, 0);
  for (int i = 64; i < 1000; ++i) {
    const std::string id = "r" + std::to_string(i);
    sites.AddPending(Site(i), id, Site(i) + "^k");
    requests.left[id] = 1;
    ASSERT_EQ(Schedule(sites, Site(i), requests, &valve_heard),
              (std::vector<std::string>{id}))
        << i;
  }
  EXPECT_EQ(valve_heard, 1000 - 64);
  EXPECT_EQ(sites.total_running(), kMoltBackgroundFetchRunningFloor + 936);
}

// Within a site, its active registrations take turns: the one served least
// recently starts next, as Chromium takes them for a partition.
TEST(MoltNetBackgroundFetchSchedulingTest, TurnsWithinASite) {
  MoltBackgroundFetchSites sites;
  Requests requests;
  const std::string a = "https://a.test";
  sites.AddPending(a, "a1", "k1");
  sites.AddPending(a, "a2", "k2");
  requests.left["a1"] = 10;
  requests.left["a2"] = 10;
  EXPECT_EQ(Schedule(sites, a, requests),
            (std::vector<std::string>{"a1", "a2"}));
  sites.Finished(a);
  EXPECT_EQ(Schedule(sites, a, requests), (std::vector<std::string>{"a1"}));
  sites.Finished(a);
  EXPECT_EQ(Schedule(sites, a, requests), (std::vector<std::string>{"a2"}));
  // One with nothing left is passed over.
  requests.left["a1"] = 0;
  sites.Finished(a);
  EXPECT_EQ(Schedule(sites, a, requests), (std::vector<std::string>{"a2"}));
}

// A site with nothing pending, active or running isn't kept, whatever order
// its registrations and requests end in; counts never go below zero.
TEST(MoltNetBackgroundFetchSchedulingTest, SitesAreForgotten) {
  MoltBackgroundFetchSites sites;
  Requests requests;
  const std::string a = "https://a.test";
  sites.AddPending(a, "a1", "k1");
  sites.AddPending(a, "a2", "k1");
  requests.left["a1"] = 2;
  Schedule(sites, a, requests);
  EXPECT_EQ(sites.running(a), 2);
  // The pending one is given up, the active one finishes with its requests
  // still downloading.
  sites.Remove("a2");
  sites.Finished(a, 2);
  sites.Remove("a1");
  EXPECT_EQ(sites.site_count(), 0u);
  EXPECT_EQ(sites.registration_count(), 0u);
  EXPECT_EQ(sites.total_running(), 0);
  sites.Finished(a, 5);
  sites.Started(a, 0);
  sites.Finished(a, -1);
  sites.Remove("unknown");
  EXPECT_EQ(sites.site_count(), 0u);
  EXPECT_FALSE(sites.SiteOf("a1").has_value());
}

// A registration resumed at startup is active at once, and its key counts.
TEST(MoltNetBackgroundFetchSchedulingTest, ResumedAtStartup) {
  MoltBackgroundFetchSites sites;
  const std::string a = "https://a.test";
  sites.AddActive(a, "a1", "k1");
  sites.AddPending(a, "a2", "k1");
  sites.AddPending(a, "a3", "k2");
  EXPECT_EQ(sites.Activate(a), (std::vector<std::string>{"a3"}));
  EXPECT_EQ(sites.SiteOf("a2"), a);
  sites.Remove("a1");
  sites.Remove("a3");
  EXPECT_EQ(sites.Activate(a), (std::vector<std::string>{"a2"}));
}

// The in-memory download service never queues or refuses what Background
// Fetch starts: it has no limit of its own, whatever a field trial sets.
TEST(MoltNetBackgroundFetchSchedulingTest, DownloadServiceHasNoLimit) {
  EXPECT_EQ(download::kMoltInMemoryDownloadsLimit,
            std::numeric_limits<uint32_t>::max());
  EXPECT_EQ(download::MoltInMemoryDownloadLimit(2),
            download::kMoltInMemoryDownloadsLimit);
  EXPECT_EQ(download::MoltInMemoryDownloadLimit(15),
            download::kMoltInMemoryDownloadsLimit);
  EXPECT_EQ(download::MoltInMemoryDownloadLimit(1000),
            download::kMoltInMemoryDownloadsLimit);
}

}  // namespace
}  // namespace molt_ai::tor
