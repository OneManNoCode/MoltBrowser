// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/runtime/gpu_inference_prefs.h"

#include <utility>
#include <vector>

#include "base/values.h"
#include "components/prefs/testing_pref_service.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace {

constexpr int64_t kT0 = 1790812800000LL;

GpuDriverEntry Entry() {
  GpuDriverEntry entry;
  entry.key.identified = true;
  entry.key.vendor_id = 0x10de;
  entry.key.device_id = 0x2684;
  entry.key.driver_id = 4;
  entry.key.driver_version = 0x89a00000;  // above INT32_MAX
  entry.key.driver_uuid[3] = 0x7f;
  entry.name = "NVIDIA GeForce RTX 4090";
  entry.failure_ms = {kT0, kT0 + 1, kT0 + 2};
  entry.last_event_ms = kT0 + 2;
  entry.turned_off = true;
  entry.turned_off_ms = kT0 + 2;
  entry.turned_off_version = "0.2.5";
  return entry;
}

TEST(GpuInferencePrefsTest, Registered) {
  TestingPrefServiceSimple prefs;
  RegisterGpuInferenceLocalStatePrefs(prefs.registry());
  EXPECT_EQ(static_cast<int>(GpuInferenceMode::kAutomatic),
            prefs.GetInteger(kGpuInferenceModePref));
  EXPECT_TRUE(prefs.GetList(kGpuDriverRecordsPref).empty());
}

TEST(GpuInferencePrefsTest, Mode) {
  EXPECT_EQ(GpuInferenceMode::kAutomatic, GpuInferenceModeFromPref(0));
  EXPECT_EQ(GpuInferenceMode::kOff, GpuInferenceModeFromPref(1));
  // Anything else (a hand-edited Local State) is the default.
  EXPECT_EQ(GpuInferenceMode::kAutomatic, GpuInferenceModeFromPref(2));
  EXPECT_EQ(GpuInferenceMode::kAutomatic, GpuInferenceModeFromPref(-1));
}

TEST(GpuInferencePrefsTest, RoundTrip) {
  GpuDriverEntry probation = Entry();
  probation.key = UnidentifiedGpuDriverKey();
  probation.name = "";
  probation.turned_off = false;
  probation.turned_off_ms = 0;
  probation.turned_off_version = "";
  probation.probation = true;
  probation.failure_ms = {};
  const std::vector<GpuDriverEntry> entries = {Entry(), probation};
  EXPECT_EQ(entries, GpuDriverEntriesFromPref(GpuDriverEntriesToPref(entries)));
}

TEST(GpuInferencePrefsTest, BadEntriesDropped) {
  base::ListValue list = GpuDriverEntriesToPref({Entry()});
  list.Append(base::Value("not a dict"));
  base::DictValue bad_key;
  bad_key.Set("key", "vk:../../etc");
  list.Append(std::move(bad_key));
  base::DictValue no_key;
  no_key.Set("name", "x");
  list.Append(std::move(no_key));
  base::DictValue odd;
  odd.Set("key", GpuDriverKeyToString(Entry().key));
  odd.Set("name", "<script>alert(1)</script>");
  base::ListValue failures;
  failures.Append(-1.0);
  failures.Append(1e300);
  failures.Append("soon");
  failures.Append(static_cast<double>(kT0));
  odd.Set("failures", std::move(failures));
  odd.Set("off", "yes");  // not a bool
  odd.Set("off_version", "1.0; rm");
  list.Append(std::move(odd));

  const std::vector<GpuDriverEntry> entries = GpuDriverEntriesFromPref(list);
  ASSERT_EQ(2u, entries.size());
  EXPECT_EQ(Entry(), entries[0]);
  EXPECT_EQ("scriptalert(1)/script", entries[1].name);
  EXPECT_EQ((std::vector<int64_t>{kT0}), entries[1].failure_ms);
  EXPECT_FALSE(entries[1].turned_off);
  EXPECT_EQ("", entries[1].turned_off_version);
}

TEST(GpuInferencePrefsTest, AtMostEightRead) {
  std::vector<GpuDriverEntry> many;
  for (uint32_t i = 0; i < 20; ++i) {
    GpuDriverEntry e = Entry();
    e.key.device_id = i;
    many.push_back(e);
  }
  EXPECT_EQ(GpuDriverRecords::kMaxEntries,
            GpuDriverEntriesFromPref(GpuDriverEntriesToPref(many)).size());
}

}  // namespace
}  // namespace molt_ai
