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
// Here a change notified to more than one observer that carries a big value
// is split: each observer's notification is a task of its own in the area's
// site's turn, in the order of the changes (StorageAreaImpl::
// MoltNotifyObservers(), MoltDomStorageSplitsNotifications()); and a big map
// is freed a chunk a task there (MoltDomStorageFreeChunk()). What one
// message still does in one task: its own decoding and its change (for a
// split write, one more copy of its value, which its notifications share),
// with one observer's notification, or its reply (a read of the whole
// area): each bounded by one area's limit, whatever the site does otherwise.
//
// Walks over the whole partition are split too. Listing what a partition
// stores (GetUsage(): what Clear-Site-Data and the browser's settings read
// before they delete) walked every area of every site in one task at
// Chromium 156, and so did the memory dump provider. Here the listing walks
// the partition's groups (sites) kMoltDomStorageGroupsPerTask a task in the
// control's turn, and lists each group's areas in that group's own turn,
// kMoltDomStorageAreasPerTask a task (MoltDomStorageNextChunk()), so it is
// also in order with each group's own messages: a group's areas are listed
// after the messages it queued before the listing reached it, and a clear
// that follows deletes what they stored. The memory dump reports the
// partition's totals, which are kept as they change, and names no area.
//
// Std-only, so molt_ai_unittests and a standalone harness test it.

#ifndef COMPONENTS_SERVICES_STORAGE_DOM_STORAGE_MOLT_DOM_STORAGE_SPLIT_WORK_H_
#define COMPONENTS_SERVICES_STORAGE_DOM_STORAGE_MOLT_DOM_STORAGE_SPLIT_WORK_H_

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace storage {

// A change's notifications are split, each observer's in a task of its own,
// when they carry at least this many bytes (the key and the values) to more
// than one observer; and, to keep every observer's notifications in order,
// while any of the area's split notifications are still to be sent. A
// smaller change, or one with a single observer, is notified at once, as
// Chromium does: splitting it would only add tasks (and a copy of the value).
inline constexpr size_t kMoltDomStorageSplitNotificationBytes = 64 * 1024;

constexpr bool MoltDomStorageSplitsNotifications(size_t observers,
                                                 size_t payload_bytes,
                                                 size_t pending_split) {
  return pending_split > 0 ||
         (observers >= 2 &&
          payload_bytes >= kMoltDomStorageSplitNotificationBytes);
}

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

// ---- Walks over a partition ----

// How many groups (sites) a walk's step reaches, and how many of one
// group's areas a step lists, in one task. A step's groups are ready at
// once, each with its first chunk, so a read sent between two steps by a
// site with nothing queued waits for at most kMoltDomStorageGroupsPerTask
// chunks: about a thousand areas listed, however many the partition holds.
inline constexpr size_t kMoltDomStorageGroupsPerTask = 16;
inline constexpr size_t kMoltDomStorageAreasPerTask = 64;

// The key of a sorted container's element: a set's element, or a map's key.
template <typename Key>
const Key& MoltDomStorageKeyOf(const Key& key) {
  return key;
}
template <typename Key, typename Value>
const Key& MoltDomStorageKeyOf(const std::pair<const Key, Value>& item) {
  return item.first;
}

// Of `sorted` (a std::set or std::map), the next at most `max` keys after
// `after` (from the first if null): one step of a walk that resumes after
// the last key it reached, however the container changed meanwhile. `more`
// says whether keys follow.
template <typename Sorted>
std::vector<typename Sorted::key_type> MoltDomStorageNextChunk(
    const Sorted& sorted,
    const typename Sorted::key_type* after,
    size_t max,
    bool* more) {
  std::vector<typename Sorted::key_type> chunk;
  auto it = after ? sorted.upper_bound(*after) : sorted.begin();
  for (; it != sorted.end() && chunk.size() < max; ++it) {
    chunk.push_back(MoltDomStorageKeyOf(*it));
  }
  *more = it != sorted.end();
  return chunk;
}

}  // namespace storage

#endif  // COMPONENTS_SERVICES_STORAGE_DOM_STORAGE_MOLT_DOM_STORAGE_SPLIT_WORK_H_
