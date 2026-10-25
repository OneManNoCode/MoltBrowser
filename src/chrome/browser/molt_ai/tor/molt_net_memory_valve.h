// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNetMemoryValve — ends a MoltNet identity when the machine runs short
// of memory while its sites store more than MoltNet's floors
// (molt_net_memory_valve_policy.h says why and when). One per MoltNet
// profile; the quota system and the blob storage tell it as they grow past
// a floor (storage::QuotaSettings and storage::BlobStorageLimits'
// molt_on_growth_beyond_floor, which ChromeContentBrowserClient sets to
// MoltNetMemoryValveCallback()), from their own sequences.

#ifndef CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_MEMORY_VALVE_H_
#define CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_MEMORY_VALVE_H_

#include <atomic>
#include <cstdint>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "chrome/browser/profiles/profile.h"

namespace molt_ai::tor {

class MoltNetMemoryValve
    : public base::RefCountedThreadSafe<MoltNetMemoryValve> {
 public:
  // Reads the machine's available memory (blocking allowed); 0 if unknown.
  using ReadAvailableMemory = uint64_t (*)();
  // Ends the identity (the MoltNet profile `id` of the original profile at
  // `path`), on the UI thread.
  using EndIdentity = void (*)(const base::FilePath& path,
                               const Profile::OTRProfileID& id);

  // Made on the UI thread, for `moltnet_profile`; ends it when the machine
  // keeps less than `reserve` available.
  MoltNetMemoryValve(Profile* moltnet_profile,
                     uint64_t reserve,
                     ReadAvailableMemory read = nullptr,
                     EndIdentity end = nullptr);
  MoltNetMemoryValve(const MoltNetMemoryValve&) = delete;
  MoltNetMemoryValve& operator=(const MoltNetMemoryValve&) = delete;

  // A site stored more beyond a floor. Any sequence.
  void OnGrowthBeyondFloor();

  bool HasEndedForTesting() const { return ended_; }

 private:
  friend class base::RefCountedThreadSafe<MoltNetMemoryValve>;
  ~MoltNetMemoryValve();

  // On a worker: reads the machine's memory and decides.
  void Read();

  const base::FilePath path_;
  const Profile::OTRProfileID id_;
  const uint64_t reserve_;
  const ReadAvailableMemory read_;
  const EndIdentity end_;
  const scoped_refptr<base::SequencedTaskRunner> ui_task_runner_;
  std::atomic<bool> ended_{false};
  std::atomic<bool> reading_{false};
  // When the last reading started (base::TimeTicks, in microseconds).
  std::atomic<int64_t> last_reading_us_{0};
};

// The callback storage and blobs run as they grow past a floor: the valve
// of `profile`, made the first time, for a MoltNet profile (with the reserve
// `reserve`); a null callback for any other profile.
base::RepeatingClosure MoltNetMemoryValveCallback(Profile* profile,
                                                  uint64_t reserve);

}  // namespace molt_ai::tor

#endif  // CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_MEMORY_VALVE_H_
