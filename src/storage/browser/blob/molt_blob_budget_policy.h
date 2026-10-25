// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: blob memory in shares, for embedders that set one
// (BlobStorageLimits::molt_per_key_memory: MoltNet). Std-only, so
// molt_ai_unittests tests it; BlobMemoryController applies it.
//
// Chromium 156 holds all of a profile's blobs in one memory limit
// (max_blob_in_memory_space, 800 MiB in MoltNet, where nothing pages to
// disk): a page that fills it makes every other site's blob creation fail
// (Blob, Response.blob(), canvas.toBlob(), MediaRecorder, ...), and that
// failure is a signal another site can read. Here blob memory is counted per
// budget key: the renderer process that registered the blob (its
// BlobRegistry is bound per process; MoltNet's processes each serve one
// BrowsingInstance, content/browser/renderer_host/
// molt_browsing_instance_processes_policy.h, so a key is one site in one
// tab), or the browser's own blobs (kMoltBlobBrowserKey).
//
// A key may use its share (`per_key`) whatever the other keys use, as long
// as all keys together stay below the floor (`floor`, the old total, which
// is always available). Beyond the floor a blob is still stored while the
// machine has more memory available than `reserve`: the limit there is the
// machine's own memory, which any program on it can use up as well, not a
// limit of MoltBrowser's that a group of sites could reach and another site
// see. A blob that doesn't fit fails for the page that made it; nothing
// another site stored is removed.

#ifndef STORAGE_BROWSER_BLOB_MOLT_BLOB_BUDGET_POLICY_H_
#define STORAGE_BROWSER_BLOB_MOLT_BLOB_BUDGET_POLICY_H_

#include <algorithm>
#include <cstdint>
#include <optional>

namespace storage {

// The browser's own blobs (not registered through a renderer's
// BlobRegistry).
inline constexpr uint64_t kMoltBlobBrowserKey = 0;

// Whether blob memory is counted in shares.
constexpr bool MoltUsesBlobBudget(uint64_t per_key) {
  return per_key > 0;
}

// What a key may still use of its share.
constexpr uint64_t MoltBlobKeyRoom(uint64_t per_key, uint64_t key_used) {
  return key_used >= per_key ? 0 : per_key - key_used;
}

// Whether the machine's available memory is needed: the key's room reaches
// past the floor. Below the floor nothing depends on the machine.
constexpr bool MoltBlobNeedsMemoryCheck(uint64_t per_key,
                                        uint64_t key_used,
                                        uint64_t total,
                                        uint64_t floor) {
  const uint64_t key_room = MoltBlobKeyRoom(per_key, key_used);
  return key_room > 0 && (total >= floor || floor - total < key_room);
}

// What the machine allows beyond the floor: its available memory above
// `reserve`; nothing if it couldn't be read.
constexpr uint64_t MoltBlobMachineRoom(std::optional<uint64_t> available,
                                       uint64_t reserve) {
  return available && *available > reserve ? *available - reserve : 0;
}

// The memory `key` may still get: its share's room, within what is left of
// the floor or, beyond it, what the machine allows. `total`: every key's
// blobs; `available`: the machine's available memory, needed only where
// MoltBlobNeedsMemoryCheck().
constexpr uint64_t MoltBlobRoom(uint64_t per_key,
                                uint64_t key_used,
                                uint64_t total,
                                uint64_t floor,
                                std::optional<uint64_t> available,
                                uint64_t reserve) {
  const uint64_t key_room = MoltBlobKeyRoom(per_key, key_used);
  const uint64_t below_floor = total >= floor ? 0 : floor - total;
  return std::min(key_room,
                  std::max(below_floor, MoltBlobMachineRoom(available, reserve)));
}

}  // namespace storage

#endif  // STORAGE_BROWSER_BLOB_MOLT_BLOB_BUDGET_POLICY_H_
