// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// When MoltNet ends an identity for the machine's memory. Std-only, so
// molt_ai_unittests tests it; molt_net_memory_valve.cc applies it.
//
// MoltNet's sites store in memory: storage (IndexedDB, Cache Storage, the
// origin-private file system, ...) and blobs. Each site, or each renderer
// process for blobs, has a share of its own it can always use, whatever the
// others do (storage/browser/quota/molt_storage_budget_policy.h,
// storage/browser/blob/molt_blob_budget_policy.h), and that is the only
// limit a write meets: no write fails because of a total other sites could
// fill, or because of the machine's memory. Otherwise a group of sites
// could fill a total, or the machine, and another site, or the group
// itself, would read from its own writes failing that it had, and roughly
// how much memory the machine had free.
//
// Memory still has to be bounded. Below the floors (600 MiB of storage, 800
// MiB of blobs, sized for the smallest machine MoltNet runs on) nothing is
// checked. Beyond a floor, storage and blobs tell this valve as they grow;
// the valve reads the machine's available memory on a worker, apart from
// any write, at most every kMoltNetMemoryValveIntervalMs while they grow,
// and if it is below the reserve, it ends the whole identity: every page
// of it closes, at once, the same way New Identity does, and the user is
// told why. That is what an out-of-memory crash would otherwise do,
// uncontrolled (and to the regular windows too). What a group of sites can
// learn this way: roughly how much memory the machine had free, once, at
// the cost of the identity they track the user in; no other site's write
// ever fails, and nothing can be learnt without ending the identity.
//
// A reading of 0 means the system couldn't say: nothing is ended for it.

#ifndef CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_MEMORY_VALVE_POLICY_H_
#define CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_MEMORY_VALVE_POLICY_H_

#include <cstdint>

namespace molt_ai::tor {

// The least time between two readings of the machine's memory while
// MoltNet's sites store more than a floor.
inline constexpr int64_t kMoltNetMemoryValveIntervalMs = 200;

// Whether the valve reads the machine's memory now, after a site stored
// more beyond a floor: not once it has ended the identity, not while a
// reading is under way, and not within the interval of the last one.
constexpr bool MoltNetMemoryValveReads(bool ended,
                                       bool reading,
                                       int64_t ms_since_last_reading) {
  return !ended && !reading &&
         ms_since_last_reading >= kMoltNetMemoryValveIntervalMs;
}

// Whether a reading of the machine's available memory (`available`, 0 if
// the system couldn't say) ends the identity: below `reserve`.
constexpr bool MoltNetMemoryValveEnds(uint64_t available, uint64_t reserve) {
  return available > 0 && available < reserve;
}

}  // namespace molt_ai::tor

#endif  // CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_MEMORY_VALVE_POLICY_H_
