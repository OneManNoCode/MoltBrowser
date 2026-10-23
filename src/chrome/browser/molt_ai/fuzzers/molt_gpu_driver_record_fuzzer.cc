// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The lasting GPU failure record and the pipeline cache files' names
// (runtime/gpu_driver_record.h, runtime/gpu_pipeline_cache_files.h;
// docs/design/SANDBOXED_INFERENCE.md sections 11.10 and 11.12). Their
// inputs are what a possibly compromised inference service reports (device
// names and keys, at most two failures a session) and what Local State
// holds (key text).
//
// Checks, after every operation of a random sequence (failures, retries,
// queries, restores, with times that jump either way): at most kMaxEntries
// entries, no two with one key, every key round-trips through its text,
// names are SanitizeGpuName()'s subset within 64 bytes, at most
// kFailuresToTurnOff failure times, probation never on a turned-off entry,
// and Restore() of the entries is idempotent. Key text, names and file
// names from raw bytes: a parsed key round-trips exactly; a file name is
// only ever "vk-" + 32 lowercase hex digits + ".bin".

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <fuzzer/FuzzedDataProvider.h>

#include "base/check.h"
#include "chrome/browser/molt_ai/runtime/gpu_driver_record.h"
#include "chrome/browser/molt_ai/runtime/gpu_pipeline_cache_files.h"

namespace {

using molt_ai::GpuDriverEntry;
using molt_ai::GpuDriverKey;
using molt_ai::GpuDriverRecords;

bool NameChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == ' ' || c == '(' || c == ')' ||
         c == '+' || c == '-' || c == '.' || c == '_' || c == '/' ||
         c == ',' || c == '[' || c == ']';
}

void CheckName(const std::string& name) {
  CHECK(name.size() <= molt_ai::kMaxGpuDisplayNameBytes);
  for (char c : name) CHECK(NameChar(c));
  CHECK(name.empty() || (name.front() != ' ' && name.back() != ' '));
}

GpuDriverKey Key(FuzzedDataProvider& fdp) {
  if (fdp.ConsumeIntegralInRange<int>(0, 7) == 0) {
    return molt_ai::UnidentifiedGpuDriverKey();
  }
  GpuDriverKey key;
  key.identified = true;
  // A few distinct drivers most of the time, so entries repeat.
  if (fdp.ConsumeBool()) {
    key.device_id = fdp.ConsumeIntegralInRange<uint32_t>(0, 11);
    key.vendor_id = 0x1002;
    return key;
  }
  key.vendor_id = fdp.ConsumeIntegral<uint32_t>();
  key.device_id = fdp.ConsumeIntegral<uint32_t>();
  key.driver_id = fdp.ConsumeIntegral<uint32_t>();
  key.driver_version = fdp.ConsumeIntegral<uint32_t>();
  for (uint8_t& b : key.driver_uuid) b = fdp.ConsumeIntegral<uint8_t>();
  return key;
}

void CheckRecords(const GpuDriverRecords& records, int64_t now) {
  const std::vector<GpuDriverEntry>& entries = records.entries();
  CHECK(entries.size() <= GpuDriverRecords::kMaxEntries);
  for (size_t i = 0; i < entries.size(); ++i) {
    const GpuDriverEntry& e = entries[i];
    for (size_t j = i + 1; j < entries.size(); ++j) {
      CHECK(!(e.key == entries[j].key));
    }
    const std::optional<GpuDriverKey> back =
        molt_ai::GpuDriverKeyFromString(molt_ai::GpuDriverKeyToString(e.key));
    CHECK(back && *back == e.key);
    CheckName(e.name);
    CHECK(e.failure_ms.size() <= GpuDriverRecords::kFailuresToTurnOff);
    CHECK(!(e.probation && e.turned_off));
    CHECK(molt_ai::SanitizeVersion(e.turned_off_version) ==
          e.turned_off_version);
  }
  GpuDriverRecords once;
  once.Restore(entries, now);
  GpuDriverRecords twice;
  twice.Restore(once.entries(), now);
  CHECK(once.entries() == twice.entries());
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  FuzzedDataProvider fdp(data, size);
  GpuDriverRecords records;
  int64_t now = 1790812800000LL;
  const char* const versions[] = {"0.2.5", "0.2.6", "", "x"};
  for (int step = 0; step < 64 && fdp.remaining_bytes() > 0; ++step) {
    // Time moves on (by up to ~120 days) or back (a clock change).
    now += fdp.ConsumeIntegralInRange<int64_t>(-GpuDriverRecords::kDayMs,
                                               120 * GpuDriverRecords::kDayMs);
    const char* version = versions[fdp.ConsumeIntegralInRange<int>(0, 3)];
    switch (fdp.ConsumeIntegralInRange<int>(0, 5)) {
      case 0:
      case 1: {
        const GpuDriverKey key = Key(fdp);
        const std::string name = fdp.ConsumeRandomLengthString(300);
        records.RecordFailure(key, name, now, version);
        break;
      }
      case 2: {
        const GpuDriverKey key = Key(fdp);
        bool changed = false;
        const bool allow = records.AllowGpu(key, now, version, &changed);
        // Asked again at once: the same answer, nothing more changes.
        bool again = true;
        CHECK(records.AllowGpu(key, now, version, &again) == allow);
        CHECK(!again);
        break;
      }
      case 3:
        for (const GpuDriverEntry& e : records.TurnedOff(now, version,
                                                         nullptr)) {
          CHECK(e.turned_off);
        }
        break;
      case 4:
        records.RetryAll(now);
        CHECK(records.TurnedOff(now, version, nullptr).empty());
        break;
      case 5:
        records.Restore(records.entries(), now);
        break;
    }
    CheckRecords(records, now);
  }

  // Raw text: key text, names, file names.
  const std::string text = fdp.ConsumeRandomLengthString(200);
  if (std::optional<GpuDriverKey> key = molt_ai::GpuDriverKeyFromString(text)) {
    CHECK(molt_ai::GpuDriverKeyToString(*key) == text);
  }
  CheckName(molt_ai::SanitizeGpuName(text));
  const std::string file = molt_ai::PipelineCacheFileName(text);
  if (!file.empty()) CHECK(molt_ai::IsPipelineCacheFileName(file));
  if (molt_ai::IsPipelineCacheFileName(text)) {
    CHECK(text.size() == 39 && text.substr(0, 3) == "vk-");
    for (char c : text.substr(3, 32)) {
      CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
  }
  std::array<uint8_t, 16> uuid{};
  const std::string key_text = molt_ai::PipelineCacheKeyText(
      Key(fdp), uuid, fdp.ConsumeRandomLengthString(100));
  for (char c : key_text) {
    CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == ':' ||
          c == '|' || c == '.' || c == '/' || c == '_' || c == '-');
  }
  return 0;
}
