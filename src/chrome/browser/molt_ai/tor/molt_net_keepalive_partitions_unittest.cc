// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The registry behind MoltNet's per-site keepalive limits
// (services/network/molt_keepalive_partitions.h, std-only), with a stand-in
// recorder.

#include "services/network/molt_keepalive_partitions.h"

#include <cstdint>
#include <string>

#include "testing/gtest/include/gtest/gtest.h"

namespace network {
namespace {

struct Recorder {
  int inflight = 0;
  int frames = 0;
  bool MoltIsIdle() const { return inflight == 0 && frames == 0; }
};

constexpr uintptr_t kMoltNet = 0x1000;
constexpr uintptr_t kOtherMoltNet = 0x2000;
constexpr uintptr_t kRegular = 0x3000;

TEST(MoltNetKeepalivePartitionsTest, OtherContextsShareTheServicesRecorder) {
  MoltKeepalivePartitions<Recorder> partitions;
  Recorder shared;
  partitions.AddContext(kMoltNet);
  EXPECT_EQ(partitions.RecorderFor(kRegular, "https://a.test", &shared),
            &shared);
  EXPECT_EQ(partitions.RecorderFor(kRegular, "", &shared), &shared);
  EXPECT_FALSE(partitions.IsPartitioned(kRegular));
  EXPECT_EQ(partitions.RecorderCount(), 0u);
}

TEST(MoltNetKeepalivePartitionsTest, EachMoltNetSiteHasItsOwn) {
  MoltKeepalivePartitions<Recorder> partitions;
  Recorder shared;
  partitions.AddContext(kMoltNet);
  Recorder* a = partitions.RecorderFor(kMoltNet, "https://a.test", &shared);
  Recorder* b = partitions.RecorderFor(kMoltNet, "https://b.test", &shared);
  Recorder* none = partitions.RecorderFor(kMoltNet, "", &shared);
  EXPECT_NE(a, &shared);
  EXPECT_NE(a, b);
  EXPECT_NE(none, a);
  EXPECT_NE(none, &shared);
  // The same site gets the same one.
  EXPECT_EQ(a, partitions.RecorderFor(kMoltNet, "https://a.test", &shared));
  // Another MoltNet context (another identity) has its own, also for a.
  partitions.AddContext(kOtherMoltNet);
  EXPECT_NE(a,
            partitions.RecorderFor(kOtherMoltNet, "https://a.test", &shared));
  EXPECT_EQ(partitions.RecorderCount(), 4u);
}

TEST(MoltNetKeepalivePartitionsTest, AContextsRecordersGoWithIt) {
  MoltKeepalivePartitions<Recorder> partitions;
  Recorder shared;
  partitions.AddContext(kMoltNet);
  partitions.AddContext(kOtherMoltNet);
  partitions.RecorderFor(kMoltNet, "https://a.test", &shared)->inflight = 5;
  partitions.RecorderFor(kMoltNet, "https://b.test", &shared);
  Recorder* other =
      partitions.RecorderFor(kOtherMoltNet, "https://a.test", &shared);
  partitions.RemoveContext(kMoltNet);
  EXPECT_FALSE(partitions.IsPartitioned(kMoltNet));
  EXPECT_EQ(partitions.RecorderCount(), 1u);
  EXPECT_EQ(other,
            partitions.RecorderFor(kOtherMoltNet, "https://a.test", &shared));
  // Removing a context that has none, or twice, is harmless.
  partitions.RemoveContext(kRegular);
  partitions.RemoveContext(kMoltNet);
  EXPECT_EQ(partitions.RecorderCount(), 1u);
  // A new context at the same address starts afresh.
  partitions.AddContext(kMoltNet);
  EXPECT_EQ(partitions.RecorderFor(kMoltNet, "https://a.test", &shared)
                ->inflight,
            0);
}

// A site's recorder goes once nothing uses it (no factory, nothing in
// flight), so the recorders don't grow with every site visited; one made
// again starts at nothing, which is what the idle one counted. One in use
// stays, whatever its context's other sites do.
TEST(MoltNetKeepalivePartitionsTest, AnIdleSitesRecorderGoes) {
  MoltKeepalivePartitions<Recorder> partitions;
  Recorder shared;
  partitions.AddContext(kMoltNet);
  Recorder* a = partitions.RecorderFor(kMoltNet, "https://a.test", &shared);
  Recorder* b = partitions.RecorderFor(kMoltNet, "https://b.test", &shared);
  a->frames = 1;
  b->inflight = 3;
  partitions.ForgetIfIdle(kMoltNet, "https://a.test");
  partitions.ForgetIfIdle(kMoltNet, "https://b.test");
  EXPECT_EQ(partitions.RecorderCount(), 2u);
  // a's page closes; b's keepalive requests outlive their page, then end.
  a->frames = 0;
  partitions.ForgetIfIdle(kMoltNet, "https://a.test");
  EXPECT_EQ(partitions.RecorderCount(), 1u);
  b->inflight = 0;
  partitions.ForgetIfIdle(kMoltNet, "https://b.test");
  EXPECT_EQ(partitions.RecorderCount(), 0u);
  // Unknown ones, and the shared one, are left alone.
  partitions.ForgetIfIdle(kRegular, "https://a.test");
  partitions.ForgetIfIdle(kMoltNet, "https://c.test");
  EXPECT_EQ(partitions.RecorderFor(kMoltNet, "https://a.test", &shared)
                ->inflight,
            0);
}

// Visiting many sites in turn leaves only the recorders still in use.
TEST(MoltNetKeepalivePartitionsTest, RecordersDontGrowWithSitesVisited) {
  MoltKeepalivePartitions<Recorder> partitions;
  Recorder shared;
  partitions.AddContext(kMoltNet);
  for (int i = 0; i < 10000; ++i) {
    const std::string site = "https://s" + std::to_string(i) + ".test";
    Recorder* r = partitions.RecorderFor(kMoltNet, site, &shared);
    r->frames = 1;
    r->frames = 0;
    partitions.ForgetIfIdle(kMoltNet, site);
  }
  EXPECT_EQ(partitions.RecorderCount(), 0u);
}

}  // namespace
}  // namespace network
