// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/runtime/gpu_driver_record.h"

#include <string>
#include <vector>

#include "chrome/browser/molt_ai/runtime/gpu_pipeline_cache_files.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace {

constexpr int64_t kDay = GpuDriverRecords::kDayMs;
// 2026-10-01, in ms since the Unix epoch.
constexpr int64_t kT0 = 1790812800000LL;
constexpr char kVersion[] = "0.2.5";

GpuDriverKey Radv(uint32_t driver_version = 0x01802000) {
  GpuDriverKey key;
  key.identified = true;
  key.vendor_id = 0x1002;
  key.device_id = 0x73ff;
  key.driver_id = 3;
  key.driver_version = driver_version;
  for (size_t i = 0; i < key.driver_uuid.size(); ++i) {
    key.driver_uuid[i] = static_cast<uint8_t>(0x10 * (i % 16) + i);
  }
  return key;
}

GpuDriverKey Other(uint32_t n) {
  GpuDriverKey key = Radv();
  key.device_id = 0x1000 + n;
  return key;
}

bool Allowed(GpuDriverRecords& records,
             const GpuDriverKey& key,
             int64_t now,
             const char* version = kVersion) {
  return records.AllowGpu(key, now, version, nullptr);
}

TEST(GpuDriverRecordTest, KeyText) {
  const GpuDriverKey key = Radv();
  const std::string text = GpuDriverKeyToString(key);
  EXPECT_EQ(
      "vk:00001002:000073ff:00000003:01802000:"
      "00112233445566778899aabbccddeeff",
      text);
  EXPECT_EQ(key, GpuDriverKeyFromString(text));
  EXPECT_EQ("unidentified", GpuDriverKeyToString(UnidentifiedGpuDriverKey()));
  EXPECT_EQ(UnidentifiedGpuDriverKey(), GpuDriverKeyFromString("unidentified"));
  // Only [0-9a-z:].
  for (char c : text) {
    EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == ':');
  }
  // Strict: uppercase, a short field, a missing separator, a stray byte.
  std::string upper = text;
  upper[5] = 'F';
  EXPECT_EQ(std::nullopt, GpuDriverKeyFromString(upper));
  EXPECT_EQ(std::nullopt, GpuDriverKeyFromString(text.substr(1)));
  EXPECT_EQ(std::nullopt, GpuDriverKeyFromString(text + "0"));
  std::string no_colon = text;
  no_colon[11] = '0';
  EXPECT_EQ(std::nullopt, GpuDriverKeyFromString(no_colon));
  std::string bad_uuid = text;
  bad_uuid.back() = 'g';
  EXPECT_EQ(std::nullopt, GpuDriverKeyFromString(bad_uuid));
  EXPECT_EQ(std::nullopt, GpuDriverKeyFromString(""));
  EXPECT_EQ(std::nullopt, GpuDriverKeyFromString("Unidentified"));
  // A different field is a different key.
  GpuDriverKey other = key;
  other.driver_uuid[15] ^= 1;
  EXPECT_NE(text, GpuDriverKeyToString(other));
}

TEST(GpuDriverRecordTest, SanitizesNames) {
  EXPECT_EQ("AMD Radeon RX 6600 (RADV NAVI23)",
            SanitizeGpuName("AMD Radeon RX 6600 (RADV NAVI23)"));
  EXPECT_EQ("llvmpipe (LLVM 19.1.1, 256 bits)",
            SanitizeGpuName("llvmpipe (LLVM 19.1.1, 256 bits)"));
  // Markup, quotes, controls and non-ASCII bytes go; spaces collapse.
  EXPECT_EQ("img srcx onerroralert(1)",
            SanitizeGpuName("<img src=x onerror=alert(1)>"));
  EXPECT_EQ("a b", SanitizeGpuName("  a \n\t  b  "));
  EXPECT_EQ("GeForce", SanitizeGpuName("Ge\xc3\xa9\"'`&;\\Force"));
  // Stops at a NUL, as the C string it came from.
  EXPECT_EQ("abc", SanitizeGpuName(std::string("abc\0def", 7)));
  // At most 64 bytes.
  EXPECT_EQ(std::string(kMaxGpuDisplayNameBytes, 'x'),
            SanitizeGpuName(std::string(1000, 'x')));
  EXPECT_EQ("", SanitizeGpuName("\x01\x02<>"));
}

TEST(GpuDriverRecordTest, VersionText) {
  EXPECT_EQ("0.2.5", SanitizeVersion("0.2.5"));
  EXPECT_EQ("", SanitizeVersion("0.2.5-beta"));
  EXPECT_EQ("", SanitizeVersion(""));
  EXPECT_EQ("", SanitizeVersion(std::string(33, '1')));
  EXPECT_EQ(std::string(32, '1'), SanitizeVersion(std::string(32, '1')));
}

TEST(GpuDriverRecordTest, FormatsDriverVersions) {
  // NVIDIA 550.54.14.0.
  EXPECT_EQ("550.54.14.0",
            FormatGpuDriverVersion(0x10de, 4, (550u << 22) | (54u << 14) |
                                                  (14u << 6)));
  // Intel's Windows driver 101.5333.
  EXPECT_EQ("101.5333", FormatGpuDriverVersion(0x8086, 5, (101u << 14) | 5333));
  // Mesa 25.2.3 (Vulkan's encoding), Intel on Linux too.
  EXPECT_EQ("25.2.3",
            FormatGpuDriverVersion(0x1002, 3, (25u << 22) | (2u << 12) | 3));
  EXPECT_EQ("25.2.3",
            FormatGpuDriverVersion(0x8086, 6, (25u << 22) | (2u << 12) | 3));
}

TEST(GpuDriverRecordTest, ThreeFailuresTurnADriverOff) {
  GpuDriverRecords records;
  EXPECT_TRUE(Allowed(records, Radv(), kT0));
  EXPECT_FALSE(records.RecordFailure(Radv(), "AMD", kT0, kVersion));
  EXPECT_TRUE(Allowed(records, Radv(), kT0));
  EXPECT_FALSE(records.RecordFailure(Radv(), "AMD", kT0 + kDay, kVersion));
  EXPECT_TRUE(Allowed(records, Radv(), kT0 + kDay));
  EXPECT_TRUE(records.RecordFailure(Radv(), "AMD", kT0 + 2 * kDay, kVersion));
  EXPECT_FALSE(Allowed(records, Radv(), kT0 + 2 * kDay));
  ASSERT_EQ(1u, records.entries().size());
  const GpuDriverEntry& entry = records.entries()[0];
  EXPECT_TRUE(entry.turned_off);
  EXPECT_EQ(kT0 + 2 * kDay, entry.turned_off_ms);
  EXPECT_EQ("0.2.5", entry.turned_off_version);
  EXPECT_EQ("AMD", entry.name);
  EXPECT_EQ(3u, entry.failure_ms.size());
  // Only that driver: another device, or the same one with another driver
  // version or build, still may.
  EXPECT_TRUE(Allowed(records, Other(1), kT0 + 2 * kDay));
  EXPECT_TRUE(Allowed(records, Radv(0x01802001), kT0 + 2 * kDay));
  GpuDriverKey rebuilt = Radv();
  rebuilt.driver_uuid[0] ^= 0x80;
  EXPECT_TRUE(Allowed(records, rebuilt, kT0 + 2 * kDay));
  // Off stays off before a retry is due, whatever happens meanwhile.
  EXPECT_FALSE(records.RecordFailure(Radv(), "AMD", kT0 + 3 * kDay, kVersion));
  EXPECT_FALSE(Allowed(records, Radv(), kT0 + 29 * kDay));
}

TEST(GpuDriverRecordTest, OldFailuresAreForgotten) {
  GpuDriverRecords records;
  // Three failures, but never three within 30 days.
  records.RecordFailure(Radv(), "", kT0, kVersion);
  records.RecordFailure(Radv(), "", kT0 + 20 * kDay, kVersion);
  EXPECT_FALSE(records.RecordFailure(Radv(), "", kT0 + 31 * kDay, kVersion));
  EXPECT_TRUE(Allowed(records, Radv(), kT0 + 31 * kDay));
  // Still two within 30 days of kT0 + 31: a third now turns it off.
  EXPECT_TRUE(records.RecordFailure(Radv(), "", kT0 + 40 * kDay, kVersion));
  // An entry with nothing for 30 days is dropped altogether.
  GpuDriverRecords quiet;
  quiet.RecordFailure(Radv(), "", kT0, kVersion);
  quiet.TurnedOff(kT0 + 30 * kDay, kVersion, nullptr);
  EXPECT_TRUE(quiet.entries().empty());
}

TEST(GpuDriverRecordTest, RetryAfterThirtyDaysIsProbation) {
  GpuDriverRecords records;
  for (int i = 0; i < 3; ++i) {
    records.RecordFailure(Radv(), "", kT0 + i, kVersion);
  }
  const int64_t off = kT0 + 2;
  EXPECT_FALSE(Allowed(records, Radv(), off + 30 * kDay - 1));
  bool changed = false;
  EXPECT_TRUE(records.AllowGpu(Radv(), off + 30 * kDay, kVersion, &changed));
  EXPECT_TRUE(changed);
  EXPECT_TRUE(records.entries()[0].probation);
  // Asking again changes nothing.
  EXPECT_TRUE(records.AllowGpu(Radv(), off + 30 * kDay, kVersion, &changed));
  EXPECT_FALSE(changed);
  // One failure on probation turns it off again, for another 30 days.
  EXPECT_TRUE(
      records.RecordFailure(Radv(), "", off + 31 * kDay, kVersion));
  EXPECT_FALSE(Allowed(records, Radv(), off + 60 * kDay));
  EXPECT_TRUE(Allowed(records, Radv(), off + 61 * kDay));
}

TEST(GpuDriverRecordTest, ProbationEndsAfterAQuietMonth) {
  GpuDriverRecords records;
  for (int i = 0; i < 3; ++i) records.RecordFailure(Radv(), "", kT0, kVersion);
  EXPECT_TRUE(Allowed(records, Radv(), kT0 + 30 * kDay));
  // Worked for 30 days after the retry: forgotten, three failures again.
  records.TurnedOff(kT0 + 60 * kDay, kVersion, nullptr);
  EXPECT_TRUE(records.entries().empty());
  EXPECT_FALSE(records.RecordFailure(Radv(), "", kT0 + 61 * kDay, kVersion));
}

TEST(GpuDriverRecordTest, RetryAfterAnUpdate) {
  GpuDriverRecords records;
  for (int i = 0; i < 3; ++i) records.RecordFailure(Radv(), "", kT0, "0.2.5");
  EXPECT_FALSE(Allowed(records, Radv(), kT0 + kDay, "0.2.5"));
  // A new MoltBrowser (a new llama.cpp): one retry at once.
  EXPECT_TRUE(Allowed(records, Radv(), kT0 + kDay, "0.2.6"));
  EXPECT_TRUE(records.entries()[0].probation);
  EXPECT_TRUE(records.RecordFailure(Radv(), "", kT0 + kDay, "0.2.6"));
  EXPECT_FALSE(Allowed(records, Radv(), kT0 + 2 * kDay, "0.2.6"));
  // An unusable version string never counts as an update.
  EXPECT_FALSE(Allowed(records, Radv(), kT0 + 2 * kDay, "nightly"));
  EXPECT_FALSE(Allowed(records, Radv(), kT0 + 2 * kDay, ""));
  // Nor does any version against one that wasn't recorded (turned off by a
  // build whose version was unusable): that one waits the 30 days.
  GpuDriverRecords unversioned;
  for (int i = 0; i < 3; ++i) {
    unversioned.RecordFailure(Radv(), "", kT0, "nightly");
  }
  EXPECT_EQ("", unversioned.entries()[0].turned_off_version);
  EXPECT_FALSE(Allowed(unversioned, Radv(), kT0 + kDay, "0.2.6"));
  EXPECT_TRUE(Allowed(unversioned, Radv(), kT0 + 30 * kDay, "0.2.6"));
}

TEST(GpuDriverRecordTest, UserRetry) {
  GpuDriverRecords records;
  EXPECT_FALSE(records.RetryAll(kT0));
  for (int i = 0; i < 3; ++i) records.RecordFailure(Radv(), "", kT0, kVersion);
  for (int i = 0; i < 3; ++i) {
    records.RecordFailure(Other(1), "", kT0, kVersion);
  }
  EXPECT_EQ(2u, records.TurnedOff(kT0, kVersion, nullptr).size());
  EXPECT_TRUE(records.RetryAll(kT0 + kDay));
  EXPECT_TRUE(records.TurnedOff(kT0 + kDay, kVersion, nullptr).empty());
  EXPECT_TRUE(Allowed(records, Radv(), kT0 + kDay));
  // Probation: the first failure turns it off again.
  EXPECT_TRUE(records.RecordFailure(Radv(), "", kT0 + 2 * kDay, kVersion));
}

TEST(GpuDriverRecordTest, FailuresBeforeTheDeviceWasReported) {
  GpuDriverRecords records;
  const GpuDriverKey unknown = UnidentifiedGpuDriverKey();
  for (int i = 0; i < 2; ++i) records.RecordFailure(unknown, "", kT0, kVersion);
  EXPECT_TRUE(Allowed(records, unknown, kT0));
  EXPECT_TRUE(records.RecordFailure(unknown, "", kT0, kVersion));
  EXPECT_FALSE(Allowed(records, unknown, kT0));
  // Identified drivers are unaffected.
  EXPECT_TRUE(Allowed(records, Radv(), kT0));
}

TEST(GpuDriverRecordTest, AtMostEightEntries) {
  GpuDriverRecords records;
  // Two turned off, then a stream of other failing keys (a hostile service
  // inventing devices): the turned-off ones are kept.
  for (int i = 0; i < 3; ++i) records.RecordFailure(Radv(), "", kT0, kVersion);
  for (int i = 0; i < 3; ++i) {
    records.RecordFailure(Other(99), "", kT0, kVersion);
  }
  for (uint32_t n = 0; n < 100; ++n) {
    records.RecordFailure(Other(n), "", kT0 + n, kVersion);
    ASSERT_LE(records.entries().size(), GpuDriverRecords::kMaxEntries);
  }
  EXPECT_FALSE(Allowed(records, Radv(), kT0 + 200));
  EXPECT_FALSE(Allowed(records, Other(99), kT0 + 200));
  // The newest of the others are the ones kept.
  EXPECT_EQ(Other(98).device_id, records.entries().back().key.device_id);
  // All eight turned off: the one turned off longest ago makes room.
  GpuDriverRecords full;
  for (uint32_t n = 0; n < GpuDriverRecords::kMaxEntries; ++n) {
    for (int i = 0; i < 3; ++i) {
      full.RecordFailure(Other(n), "", kT0 + n, kVersion);
    }
  }
  full.RecordFailure(Other(50), "", kT0 + 50, kVersion);
  EXPECT_EQ(GpuDriverRecords::kMaxEntries, full.entries().size());
  EXPECT_TRUE(Allowed(full, Other(0), kT0 + 51));
  EXPECT_FALSE(Allowed(full, Other(1), kT0 + 51));
}

TEST(GpuDriverRecordTest, ClockGoingBackExpiresNothing) {
  GpuDriverRecords records;
  for (int i = 0; i < 3; ++i) records.RecordFailure(Radv(), "", kT0, kVersion);
  EXPECT_FALSE(Allowed(records, Radv(), kT0 - 365 * kDay));
  EXPECT_EQ(1u, records.TurnedOff(kT0 - 365 * kDay, kVersion, nullptr).size());
}

TEST(GpuDriverRecordTest, RestoreValidates) {
  std::vector<GpuDriverEntry> stored;
  GpuDriverEntry off;
  off.key = Radv();
  off.name = "<b>AMD</b>";
  off.failure_ms = {kT0, kT0 + 1, kT0 + 2, kT0 + 3, -5, 0};
  off.last_event_ms = kT0 + 3;
  off.turned_off = true;
  off.turned_off_ms = kT0 + 3;
  off.turned_off_version = "0.2.5\"><script>";
  off.probation = true;  // not with turned_off
  stored.push_back(off);
  stored.push_back(off);  // a duplicate
  GpuDriverEntry future;
  future.key = Other(1);
  future.failure_ms = {kT0 + 400 * kDay};
  future.last_event_ms = kT0 + 400 * kDay;
  stored.push_back(future);
  GpuDriverEntry bogus;  // unidentified, yet with fields set
  bogus.key.vendor_id = 1;
  stored.push_back(bogus);
  for (uint32_t n = 10; n < 30; ++n) {
    GpuDriverEntry e;
    e.key = Other(n);
    e.failure_ms = {kT0};
    e.last_event_ms = kT0;
    stored.push_back(e);
  }

  GpuDriverRecords records;
  records.Restore(stored, kT0 + 10);
  ASSERT_EQ(GpuDriverRecords::kMaxEntries, records.entries().size());
  const GpuDriverEntry& a = records.entries()[0];
  EXPECT_EQ(Radv(), a.key);
  EXPECT_EQ("bAMD/b", a.name);
  EXPECT_EQ((std::vector<int64_t>{kT0 + 1, kT0 + 2, kT0 + 3}), a.failure_ms);
  EXPECT_TRUE(a.turned_off);
  EXPECT_FALSE(a.probation);
  EXPECT_EQ("", a.turned_off_version);
  // Ahead of the clock: taken as now.
  const GpuDriverEntry& b = records.entries()[1];
  EXPECT_EQ(Other(1), b.key);
  EXPECT_EQ((std::vector<int64_t>{kT0 + 10}), b.failure_ms);
  EXPECT_EQ(kT0 + 10, b.last_event_ms);
  for (const GpuDriverEntry& e : records.entries()) {
    EXPECT_NE(1u, e.key.vendor_id);
  }
  // Restore replaces; expired entries are forgotten (the turned-off one
  // stays until a retry).
  records.Restore(stored, kT0 + 500 * kDay);
  ASSERT_EQ(1u, records.entries().size());
  EXPECT_EQ(Radv(), records.entries()[0].key);
}

TEST(GpuDriverRecordTest, RoundTripsThroughTheStoredForm) {
  GpuDriverRecords records;
  for (int i = 0; i < 3; ++i) records.RecordFailure(Radv(), "R", kT0, "1.0");
  records.RecordFailure(Other(2), "O", kT0, "1.0");
  GpuDriverRecords copy;
  copy.Restore(records.entries(), kT0 + 1);
  EXPECT_EQ(records.entries(), copy.entries());
}

// ---- Pipeline cache files (section 11.12) ----

TEST(GpuPipelineCacheFilesTest, KeyText) {
  std::array<uint8_t, 16> uuid{};
  uuid[0] = 0xab;
  const std::string text =
      PipelineCacheKeyText(Radv(), uuid, "0.2.5/152.0.7977.169");
  EXPECT_EQ(
      "molt-vk-pipeline-cache:1|" + GpuDriverKeyToString(Radv()) +
          "|ab000000000000000000000000000000|0.2.5/152.0.7977.169",
      text);
  // The shader set keeps only [0-9a-z./_-].
  EXPECT_EQ(PipelineCacheKeyText(Radv(), uuid, "0.2.5/152"),
            PipelineCacheKeyText(Radv(), uuid, "0.2.5/1|5\n2"));
  // Every part counts.
  EXPECT_NE(text, PipelineCacheKeyText(Radv(1), uuid, "0.2.5/152.0.7977.169"));
  std::array<uint8_t, 16> other_uuid = uuid;
  other_uuid[15] = 1;
  EXPECT_NE(text,
            PipelineCacheKeyText(Radv(), other_uuid, "0.2.5/152.0.7977.169"));
  EXPECT_NE(text, PipelineCacheKeyText(Radv(), uuid, "0.2.6/152.0.7977.169"));
  // No file for an unidentified device.
  EXPECT_EQ("", PipelineCacheKeyText(UnidentifiedGpuDriverKey(), uuid, "1"));
}

TEST(GpuPipelineCacheFilesTest, FileNames) {
  const std::string digest(64, 'a');
  EXPECT_EQ("vk-" + std::string(32, 'a') + ".bin",
            PipelineCacheFileName(digest));
  EXPECT_TRUE(IsPipelineCacheFileName(PipelineCacheFileName(digest)));
  EXPECT_EQ("", PipelineCacheFileName(std::string(31, 'a')));
  EXPECT_EQ("", PipelineCacheFileName(std::string(31, 'a') + "G"));
  EXPECT_EQ("", PipelineCacheFileName("../../" + std::string(32, 'a')));
  EXPECT_FALSE(IsPipelineCacheFileName("vk-" + std::string(32, 'A') + ".bin"));
  EXPECT_FALSE(IsPipelineCacheFileName("vk-" + std::string(31, 'a') + ".bin"));
  EXPECT_FALSE(IsPipelineCacheFileName("vk-" + std::string(32, 'a') + ".tmp"));
  EXPECT_FALSE(IsPipelineCacheFileName("settings.json"));
  EXPECT_FALSE(IsPipelineCacheFileName(""));
}

TEST(GpuPipelineCacheFilesTest, DirectoryCap) {
  auto name = [](char c) { return "vk-" + std::string(32, c) + ".bin"; };
  std::vector<PipelineCacheFileInfo> files = {
      {name('1'), 10}, {name('2'), 50}, {name('3'), 30},
      {name('4'), 40}, {name('5'), 20}, {"notes.txt", 0},
      {name('6'), 60}};
  // Six of ours, four kept: the two least recently modified go, never the
  // one just written (|keep|), and never a file that isn't ours.
  EXPECT_EQ((std::vector<std::string>{name('5'), name('1')}),
            PipelineCacheFilesToDelete(files, name('6')));
  EXPECT_EQ((std::vector<std::string>{name('3'), name('5')}),
            PipelineCacheFilesToDelete(files, name('1')));
  files.resize(4);
  EXPECT_TRUE(PipelineCacheFilesToDelete(files, "").empty());
}

}  // namespace
}  // namespace molt_ai
