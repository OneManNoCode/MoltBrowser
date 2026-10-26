// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: an area's work split into short tasks, where the storage
// sequence is taken in turn per site (MoltNet's partition:
// molt_dom_storage_fair_scheduler.h).
//
// The sites take turns one task at a time, so the longest task of one site
// is how long another site's read may wait. At Chromium 156 one message
// could do a lot in one task:
//  * a change notified every observer of the area (each renderer process
//    with it open: in MoltNet one per tab), each notification carrying the
//    new and the old value, so a 10 MiB write to an area open in eight tabs
//    serialized 160 MiB in one task;
//  * a clear(), or an area going (its tab closing), freed every item of the
//    area in one task: up to about 94,000 items (a site's whole total in
//    tiny items, molt_dom_storage_site_policy.h).
// Here each observer's notification is a task of its own in the area's
// site's turn, in the order of the changes (StorageAreaImpl::
// MoltNotifyObservers()), and a big map is freed a chunk a task there
// (MoltDomStorageFreeChunk()). What one message still does in one task: its
// own decoding and its change (for a write, one copy of its value into the
// area, one for the notifications), or its reply (a read of the whole area):
// each bounded by one area's limit, whatever the site does otherwise.
//
// Std-only, so molt_ai_unittests and a standalone harness test it.

#ifndef COMPONENTS_SERVICES_STORAGE_DOM_STORAGE_MOLT_DOM_STORAGE_SPLIT_WORK_H_
#define COMPONENTS_SERVICES_STORAGE_DOM_STORAGE_MOLT_DOM_STORAGE_SPLIT_WORK_H_

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace storage {

// At most this many items, or past this many bytes, are freed in one task.
inline constexpr size_t kMoltDomStorageFreeItemsPerTask = 1024;
inline constexpr size_t kMoltDomStorageFreeBytesPerTask = 1024 * 1024;

// The bytes an item of an area's map holds: its key's and its value's (or,
// for a map of keys only, its key's).
inline size_t MoltDomStorageItemBytes(
    const std::pair<const std::vector<uint8_t>, std::vector<uint8_t>>& item) {
  return item.first.size() + item.second.size();
}
inline size_t MoltDomStorageItemBytes(
    const std::pair<const std::vector<uint8_t>, size_t>& item) {
  return item.first.size();
}

// Whether a map of `items` items holding `bytes` is freed a chunk a task;
// else at once (what one chunk is anyway).
constexpr bool MoltDomStorageFreesInChunks(size_t items, size_t bytes) {
  return items > kMoltDomStorageFreeItemsPerTask ||
         bytes > kMoltDomStorageFreeBytesPerTask;
}

// Frees `map`'s first items, a chunk: at most
// kMoltDomStorageFreeItemsPerTask, stopping once
// kMoltDomStorageFreeBytesPerTask are freed (one item at least). Whether
// any are left.
template <typename Map>
bool MoltDomStorageFreeChunk(Map& map) {
  size_t items = 0;
  size_t bytes = 0;
  while (!map.empty() && items < kMoltDomStorageFreeItemsPerTask &&
         bytes < kMoltDomStorageFreeBytesPerTask) {
    auto first = map.begin();
    bytes += MoltDomStorageItemBytes(*first);
    map.erase(first);
    ++items;
  }
  return !map.empty();
}

}  // namespace storage

#endif  // COMPONENTS_SERVICES_STORAGE_DOM_STORAGE_MOLT_DOM_STORAGE_SPLIT_WORK_H_
