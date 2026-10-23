// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/molt_net_partitioning_policy.h"

#include <string>
#include <string_view>

#include "net/base/features.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai::tor {
namespace {

MoltNetProcessWidePartitioning AllOn() {
  MoltNetProcessWidePartitioning state;
  state.connections_partitioned = true;
  state.http_cache_split = true;
  state.third_party_storage_partitioned = true;
  return state;
}

bool Names(const std::string& error, std::string_view feature) {
  return error.find(feature) != std::string::npos;
}

TEST(MoltNetPartitioningPolicyTest, OpensWhenEverythingIsPartitioned) {
  EXPECT_EQ(MoltNetPartitioningError(AllOn()), "");
}

// Nobody filled the state in: refuse rather than assume.
TEST(MoltNetPartitioningPolicyTest, DefaultStateRefuses) {
  const std::string error =
      MoltNetPartitioningError(MoltNetProcessWidePartitioning());
  EXPECT_FALSE(error.empty());
  EXPECT_TRUE(Names(error, kMoltNetConnectionsFeature));
  EXPECT_TRUE(Names(error, kMoltNetHttpCacheFeature));
  EXPECT_TRUE(Names(error, kMoltNetThirdPartyStorageFeature));
}

TEST(MoltNetPartitioningPolicyTest, ConnectionsOffRefuses) {
  MoltNetProcessWidePartitioning state = AllOn();
  state.connections_partitioned = false;
  const std::string error = MoltNetPartitioningError(state);
  EXPECT_TRUE(Names(error, kMoltNetConnectionsFeature)) << error;
  EXPECT_FALSE(Names(error, kMoltNetHttpCacheFeature)) << error;
  EXPECT_FALSE(Names(error, kMoltNetThirdPartyStorageFeature)) << error;
}

TEST(MoltNetPartitioningPolicyTest, HttpCacheOffRefuses) {
  MoltNetProcessWidePartitioning state = AllOn();
  state.http_cache_split = false;
  const std::string error = MoltNetPartitioningError(state);
  EXPECT_TRUE(Names(error, kMoltNetHttpCacheFeature)) << error;
  EXPECT_FALSE(Names(error, kMoltNetConnectionsFeature)) << error;
  EXPECT_FALSE(Names(error, kMoltNetThirdPartyStorageFeature)) << error;
}

TEST(MoltNetPartitioningPolicyTest, ThirdPartyStorageOffRefuses) {
  MoltNetProcessWidePartitioning state = AllOn();
  state.third_party_storage_partitioned = false;
  const std::string error = MoltNetPartitioningError(state);
  EXPECT_TRUE(Names(error, kMoltNetThirdPartyStorageFeature)) << error;
  EXPECT_FALSE(Names(error, kMoltNetConnectionsFeature)) << error;
  EXPECT_FALSE(Names(error, kMoltNetHttpCacheFeature)) << error;
}

// The error is shown to the user: it says what to do and names each
// switch once, separated so a user can copy one into a search.
TEST(MoltNetPartitioningPolicyTest, ErrorListsEverySwitchThatIsOff) {
  MoltNetProcessWidePartitioning state = AllOn();
  state.connections_partitioned = false;
  state.third_party_storage_partitioned = false;
  const std::string error = MoltNetPartitioningError(state);
  EXPECT_TRUE(Names(error, std::string(kMoltNetConnectionsFeature) + ", " +
                               std::string(kMoltNetThirdPartyStorageFeature)))
      << error;
  EXPECT_TRUE(Names(error, "Restart MoltBrowser")) << error;
}

// The names in the error are the ones --disable-features and field trials
// use for the switches Chromium reads.
TEST(MoltNetPartitioningPolicyTest, FeatureNamesAreChromiums) {
  EXPECT_EQ(std::string_view(
                net::features::kPartitionConnectionsByNetworkIsolationKey.name),
            kMoltNetConnectionsFeature);
  EXPECT_EQ(
      std::string_view(net::features::kSplitCacheByNetworkIsolationKey.name),
      kMoltNetHttpCacheFeature);
  EXPECT_EQ(
      std::string_view(net::features::kThirdPartyStoragePartitioning.name),
      kMoltNetThirdPartyStorageFeature);
}

}  // namespace
}  // namespace molt_ai::tor
