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
// budget key:
//  * the renderer process that registered the blob (its BlobRegistry is
//    bound per process; MoltNet's processes each serve one BrowsingInstance,
//    content/browser/renderer_host/molt_browsing_instance_processes_policy.h,
//    so a key is one site in one tab);
//  * for a blob the browser makes for a site (a Background Fetch's
//    response, which the in-memory download service holds as a blob), that
//    site's own key (MoltBlobSiteKeys);
//  * the browser's other blobs (kMoltBlobBrowserKey), which no site's page
//    can make in MoltNet.
// Memory the browser holds for a site before it becomes a blob (a
// Background Fetch's response while it downloads, which the in-memory
// download service keeps in a buffer) counts to that site's key the same
// way, as it arrives (MoltBlobHoldFor()); it then becomes the blob.
//
// A key may use its share (`per_key`), whatever the other keys use; that is
// the only limit a blob meets. There is no total all keys share that a
// group of sites could fill and another find full, and the machine's memory
// is never asked when a blob is stored, so whether one fits says nothing
// about other sites or the machine. The memory it all takes is watched
// instead: once all keys together hold more than the floor (`floor`,
// max_blob_in_memory_space), each blob stored tells the embedder
// (BlobStorageLimits::molt_on_growth_beyond_floor), which checks the
// machine's memory apart from any blob and ends the whole identity if the
// machine is short of memory (chrome/browser/molt_ai/tor/
// molt_net_memory_valve_policy.h). Nothing another site holds is removed.

#ifndef STORAGE_BROWSER_BLOB_MOLT_BLOB_BUDGET_POLICY_H_
#define STORAGE_BROWSER_BLOB_MOLT_BLOB_BUDGET_POLICY_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace storage {

// The browser's own blobs that serve no site.
inline constexpr uint64_t kMoltBlobBrowserKey = 0;

// Keys of the sites the browser makes blobs for have this bit set; renderer
// process ids (positive, 31 bits) never do.
inline constexpr uint64_t kMoltBlobSiteKeyBit = uint64_t{1} << 63;

// The budget keys of the sites the browser makes blobs for: one per site (a
// serialized SchemefulSite), handed out in order the first time the site
// needs one, so no two sites ever share a key (a hash of the site could be
// made to collide by a site that picks its domain), and none is a renderer
// process's. The browser's key for no site. Not thread-safe: its user keeps
// it on one sequence, or locks.
class MoltBlobSiteKeys {
 public:
  uint64_t KeyFor(std::string_view site) {
    if (site.empty()) {
      return kMoltBlobBrowserKey;
    }
    if (auto it = keys_.find(site); it != keys_.end()) {
      return it->second;
    }
    const uint64_t key = kMoltBlobSiteKeyBit | ++last_;
    keys_.emplace(std::string(site), key);
    return key;
  }
  size_t size() const { return keys_.size(); }

 private:
  std::map<std::string, uint64_t, std::less<>> keys_;
  uint64_t last_ = 0;
};

// Whether blob memory is counted in shares.
constexpr bool MoltUsesBlobBudget(uint64_t per_key) {
  return per_key > 0;
}

// The memory `key` may still get: what is left of its share. Nothing else:
// not the other keys' blobs, not the machine.
constexpr uint64_t MoltBlobRoom(uint64_t per_key, uint64_t key_used) {
  return key_used >= per_key ? 0 : per_key - key_used;
}

// What a hold of `bytes` more for a key that uses `key_used` comes to: with
// shares, counted against the key's room like a blob, or refused past it
// (the hold's owner then gives up what it holds, as a blob that doesn't fit
// fails); without shares (not MoltNet), not counted, as in Chromium.
enum class MoltBlobHold {
  kCounted,
  kNotCounted,
  kRefused,
};
constexpr MoltBlobHold MoltBlobHoldFor(uint64_t per_key,
                                       uint64_t key_used,
                                       uint64_t bytes) {
  if (!MoltUsesBlobBudget(per_key)) {
    return MoltBlobHold::kNotCounted;
  }
  return bytes <= MoltBlobRoom(per_key, key_used) ? MoltBlobHold::kCounted
                                                   : MoltBlobHold::kRefused;
}

// Whether all keys' blobs (`total`) are past the floor, so the embedder is
// told to watch the machine's memory
// (BlobStorageLimits::molt_on_growth_beyond_floor).
constexpr bool MoltBlobBeyondFloor(uint64_t per_key,
                                   uint64_t total,
                                   uint64_t floor) {
  return MoltUsesBlobBudget(per_key) && total > floor;
}

}  // namespace storage

#endif  // STORAGE_BROWSER_BLOB_MOLT_BLOB_BUDGET_POLICY_H_
