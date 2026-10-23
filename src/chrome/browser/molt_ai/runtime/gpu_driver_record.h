// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The lasting record of GPU failures (docs/design/SANDBOXED_INFERENCE.md
// section 11.10). GpuFallbackGuard (stream_validator.h) turns the GPU off
// for the rest of a session; this record remembers, across sessions, which
// GPU drivers failed, so a driver that crashes at every load stops costing a
// crash per model per session:
//
//  - A GPU failure (the cases of section 11.5, plus a device lost while
//    loading) counts against the driver the inference service reported for
//    that load: vendor and device ids, driver id, driver version and driver
//    UUID (GpuDriverKey). A failure before the service reported a device
//    counts against one "unidentified" entry.
//  - kFailuresToTurnOff failures within kFailureMemoryMs turn the driver
//    off: models load on the CPU, and the service is told to skip
//    that driver, until its version (any part of the key) changes.
//  - A turned-off driver is retried once kRetryAfterMs have passed since,
//    or when MoltBrowser's version differs from the one that turned it off
//    (a MoltBrowser update is when llama.cpp and its Vulkan backend are
//    updated), or when the user asks (molt://ai-settings). A retry is
//    probation: one more failure turns it off again.
//  - At most kMaxEntries entries; entries not turned off are forgotten
//    kFailureMemoryMs after their last failure (or the retry that started
//    their probation). When full, the entry not turned off with the oldest
//    event goes first.
//
// Everything in a key or a name came from the hostile inference service:
// keys are numbers (formatted as hex here, never parsed from a service
// string), names are sanitized to a short printable-ASCII subset, and
// neither is ever made into a path. Times are wall-clock milliseconds since
// the Unix epoch (the record outlives the session); a clock that went back
// makes nothing expire early.
//
// Std-only, so it is unit-tested standalone. The browser stores it in Local
// State (gpu_inference_prefs.h): it is about the machine's hardware, not a
// profile's browsing.

#ifndef CHROME_BROWSER_MOLT_AI_RUNTIME_GPU_DRIVER_RECORD_H_
#define CHROME_BROWSER_MOLT_AI_RUNTIME_GPU_DRIVER_RECORD_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace molt_ai {

struct GpuDriverKey {
  // False: a GPU failure before the service reported its device (the other
  // fields are zero).
  bool identified = false;
  uint32_t vendor_id = 0;
  uint32_t device_id = 0;
  // VkDriverId; 0 = not reported.
  uint32_t driver_id = 0;
  // The vendor's own encoding.
  uint32_t driver_version = 0;
  // VkPhysicalDeviceIDProperties::driverUUID.
  std::array<uint8_t, 16> driver_uuid{};

  bool operator==(const GpuDriverKey&) const = default;
};

// The key for failures before a device was reported.
GpuDriverKey UnidentifiedGpuDriverKey();

// "vk:<vendor>:<device>:<driver id>:<driver version>:<driver uuid>" in
// lowercase hex, or "unidentified". Only [0-9a-z:] characters, so it is safe
// in a pref, a log line and a hash.
std::string GpuDriverKeyToString(const GpuDriverKey& key);
// The inverse, strict (exactly the form above); nullopt otherwise. For keys
// read back from Local State.
std::optional<GpuDriverKey> GpuDriverKeyFromString(std::string_view text);

// The longest name kept.
inline constexpr size_t kMaxGpuDisplayNameBytes = 64;

// A GPU's name for display (VkPhysicalDeviceProperties::deviceName as the
// service reported it): letters, digits, space and ()+-._/, others removed,
// runs of spaces collapsed, trimmed, at most kMaxGpuDisplayNameBytes.
std::string SanitizeGpuName(std::string_view raw);

// MoltBrowser's version as the record keeps it: digits and dots, at most
// kMaxVersionBytes ("" for anything else).
inline constexpr size_t kMaxVersionBytes = 32;
std::string SanitizeVersion(std::string_view version);

// A driver version for display, in its vendor's encoding: NVIDIA
// (10.8.8.6 bits), Intel on Windows (18.14), else Vulkan's
// (major.minor.patch).
std::string FormatGpuDriverVersion(uint32_t vendor_id,
                                   uint32_t driver_id,
                                   uint32_t driver_version);

struct GpuDriverEntry {
  GpuDriverEntry();
  ~GpuDriverEntry();
  GpuDriverEntry(const GpuDriverEntry&);
  GpuDriverEntry& operator=(const GpuDriverEntry&);
  GpuDriverEntry(GpuDriverEntry&&);
  GpuDriverEntry& operator=(GpuDriverEntry&&);

  GpuDriverKey key;
  // Sanitized (SanitizeGpuName); display only.
  std::string name;
  // The most recent failures, oldest first (at most kFailuresToTurnOff).
  std::vector<int64_t> failure_ms;
  // The last failure, or the retry that put it on probation: an entry not
  // turned off is forgotten kFailureMemoryMs after it.
  int64_t last_event_ms = 0;
  bool turned_off = false;
  int64_t turned_off_ms = 0;
  // MoltBrowser's version when it was turned off (SanitizeVersion).
  std::string turned_off_version;
  // Retried after being turned off: one failure turns it off again.
  bool probation = false;

  bool operator==(const GpuDriverEntry&) const = default;
};

class GpuDriverRecords {
 public:
  static constexpr size_t kMaxEntries = 8;
  static constexpr uint32_t kFailuresToTurnOff = 3;
  static constexpr int64_t kDayMs = 24LL * 60 * 60 * 1000;
  static constexpr int64_t kFailureMemoryMs = 30 * kDayMs;
  static constexpr int64_t kRetryAfterMs = 30 * kDayMs;

  GpuDriverRecords();
  ~GpuDriverRecords();
  GpuDriverRecords(const GpuDriverRecords&);
  GpuDriverRecords& operator=(const GpuDriverRecords&);

  // Whether the GPU may be used with this driver now. A retry that is due
  // happens first (the entry goes on probation). Returns whether anything
  // changed in |*changed| (may be null), for the caller to save.
  bool AllowGpu(const GpuDriverKey& key,
                int64_t now_ms,
                std::string_view browser_version,
                bool* changed);

  // A GPU failure on |key|. |name| is the reported device name (sanitized
  // here). Returns true if this turned the driver off.
  bool RecordFailure(const GpuDriverKey& key,
                     std::string_view name,
                     int64_t now_ms,
                     std::string_view browser_version);

  // The drivers turned off now, retries that are due applied first (the
  // service's skip list; the settings page). Sets |*changed| like AllowGpu.
  std::vector<GpuDriverEntry> TurnedOff(int64_t now_ms,
                                        std::string_view browser_version,
                                        bool* changed);

  // The user's retry: every turned-off driver goes on probation. Returns
  // whether any was turned off.
  bool RetryAll(int64_t now_ms);

  // Replaces the entries with |entries| as read back from disk: names
  // sanitized again, times not after 0 dropped and times ahead of the clock
  // taken as now, at most kFailuresToTurnOff failure times (the latest),
  // the version sanitized, probation only when not turned off,
  // duplicates dropped (the first kept), expired ones forgotten, at most
  // kMaxEntries.
  void Restore(std::vector<GpuDriverEntry> entries, int64_t now_ms);

  void Clear();

  const std::vector<GpuDriverEntry>& entries() const { return entries_; }

 private:
  GpuDriverEntry* Find(const GpuDriverKey& key);
  // Applies the retry rules to |entry|; true if it changed.
  static bool MaybeRetry(GpuDriverEntry* entry,
                         int64_t now_ms,
                         std::string_view browser_version);
  // Forgets entries not turned off whose last event is too old.
  void Expire(int64_t now_ms);

  std::vector<GpuDriverEntry> entries_;
};

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_RUNTIME_GPU_DRIVER_RECORD_H_
