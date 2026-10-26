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
// max_scheduled_downloads), one queue for all sites. Background Fetch's own
// scheduler starts at most 2 at a time, so those limits didn't bind; in
// MoltNet it schedules per site (content/browser/background_fetch/
// molt_background_fetch_site_policy.h) and starts up to 128 at a time, and
// the service's limits would make every site's downloads wait in one queue
// again. So the in-memory service's limits are raised to
// kMoltInMemoryDownloadsLimit, twice what Background Fetch starts at most
// (cancelled downloads leave the service's counts a moment later), and
// what decides when a download starts is Background Fetch's scheduler. In an
// off-the-record profile other than MoltNet's, Background Fetch still starts
// at most 2, so nothing changes there.
//
// Std-only, so molt_ai_unittests tests it against the scheduler's total.

#ifndef COMPONENTS_DOWNLOAD_INTERNAL_BACKGROUND_SERVICE_MOLT_IN_MEMORY_DOWNLOAD_LIMITS_H_
#define COMPONENTS_DOWNLOAD_INTERNAL_BACKGROUND_SERVICE_MOLT_IN_MEMORY_DOWNLOAD_LIMITS_H_

#include <cstdint>

namespace download {

// Twice MoltNet's Background Fetch total (kMoltBackgroundFetchRunningTotal,
// 128).
inline constexpr uint32_t kMoltInMemoryDownloadsLimit = 256;

// One of the in-memory service's limits: Chromium's (`chromiums`, which
// field trials may set), but never below kMoltInMemoryDownloadsLimit.
constexpr uint32_t MoltInMemoryDownloadLimit(uint32_t chromiums) {
  return chromiums > kMoltInMemoryDownloadsLimit ? chromiums
                                                 : kMoltInMemoryDownloadsLimit;
}

}  // namespace download

#endif  // COMPONENTS_DOWNLOAD_INTERNAL_BACKGROUND_SERVICE_MOLT_IN_MEMORY_DOWNLOAD_LIMITS_H_
