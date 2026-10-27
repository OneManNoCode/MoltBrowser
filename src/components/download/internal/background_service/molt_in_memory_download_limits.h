// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: the in-memory download service never queues what Background
// Fetch starts.
//
// Off the record, Background Fetch's requests are made by the profile's
// in-memory download service (BuildInMemoryDownloadService()), its only
// client there. At Chromium 156 the service runs at most 2 downloads, 4
// active or paused, and 15 live for each client
// (Configuration: max_running_downloads, max_concurrent_downloads,
// max_scheduled_downloads), one queue for all sites, and refuses a download
// past the last (StartResult::BACKOFF). Background Fetch's own scheduler
// starts at most 2 at a time, so those limits didn't bind; in MoltNet it
// schedules per site with no total for all sites (content/browser/
// background_fetch/molt_background_fetch_site_policy.h), and any limit of
// the service's would be a total again, one every site's downloads wait in
// or are refused by, and that a group of sites could fill. So the in-memory
// service has no limit of its own (kMoltInMemoryDownloadsLimit, more than it
// can count), and what decides when a download starts is Background Fetch's
// scheduler alone; what the downloads hold is bounded per site and by the
// memory valve there. In an off-the-record profile other than MoltNet's,
// Background Fetch still starts at most 2, so nothing changes there.
//
// Std-only, so molt_ai_unittests tests it.

#ifndef COMPONENTS_DOWNLOAD_INTERNAL_BACKGROUND_SERVICE_MOLT_IN_MEMORY_DOWNLOAD_LIMITS_H_
#define COMPONENTS_DOWNLOAD_INTERNAL_BACKGROUND_SERVICE_MOLT_IN_MEMORY_DOWNLOAD_LIMITS_H_

#include <cstdint>
#include <limits>

namespace download {

// No limit: the service compares its counts with its limits (a count at the
// limit stops it), and none of its counts reaches this.
inline constexpr uint32_t kMoltInMemoryDownloadsLimit =
    std::numeric_limits<uint32_t>::max();

// One of the in-memory service's limits, in place of Chromium's (which
// field trials may set): none.
constexpr uint32_t MoltInMemoryDownloadLimit(uint32_t /*chromiums*/) {
  return kMoltInMemoryDownloadsLimit;
}

}  // namespace download

#endif  // COMPONENTS_DOWNLOAD_INTERNAL_BACKGROUND_SERVICE_MOLT_IN_MEMORY_DOWNLOAD_LIMITS_H_
