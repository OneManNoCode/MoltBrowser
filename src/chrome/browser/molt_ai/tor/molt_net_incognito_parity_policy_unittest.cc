// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/molt_net_incognito_parity_policy.h"

#include <string>
#include <string_view>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace tor {
namespace {

TEST(MoltNetIncognitoParityPolicyTest, RecognisesMoltNetProfileIds) {
  // What Profile::OTRProfileID::CreateUnique("MoltNet::Tor") produces.
  EXPECT_TRUE(IsMoltNetOtrProfileIdString(
      "MoltNet::Tor-3f2b8c1e-4d5a-4b6c-9e7f-0a1b2c3d4e5f"));
  EXPECT_TRUE(IsMoltNetOtrProfileIdString(std::string(kMoltNetOtrProfilePrefix) +
                                          "-x"));
  // As before this helper existed: anything after the dash counts.
  EXPECT_TRUE(IsMoltNetOtrProfileIdString("MoltNet::Tor-"));
}

TEST(MoltNetIncognitoParityPolicyTest, RejectsOtherProfileIds) {
  for (std::string_view id : {
           "",
           "MoltNet::Tor",
           "MoltNet::Tor_1",
           "MoltNet::Torx-1",
           "MoltNet::To-1",
           "moltnet::tor-1",
           "MOLTNET::TOR-1",
           " MoltNet::Tor-1",
           "xMoltNet::Tor-1",
           "Devtools::BrowserContext-MoltNet::Tor-1",
           // 12 characters and a dash, then the prefix: only the start
           // counts.
           "Test::OTR-12-MoltNet::Tor-1",
           // Chromium's own OTR profile ids.
           "profile::primary_otr",
           "Devtools::BrowserContext-3f2b8c1e-4d5a-4b6c-9e7f-0a1b2c3d4e5f",
           "MediaRouter::Presentation-3f2b8c1e-4d5a-4b6c-9e7f-0a1b2c3d4e5f",
           "CaptivePortal::Signin-3f2b8c1e-4d5a-4b6c-9e7f-0a1b2c3d4e5f",
           "Test::OTR-3f2b8c1e-4d5a-4b6c-9e7f-0a1b2c3d4e5f",
       }) {
    EXPECT_FALSE(IsMoltNetOtrProfileIdString(id)) << id;
  }
  // An embedded NUL doesn't make a look-alike match.
  EXPECT_FALSE(IsMoltNetOtrProfileIdString(
      std::string_view("MoltNet::Tor\0-1", 15)));
}

TEST(MoltNetIncognitoParityPolicyTest, ClassifiesProfiles) {
  EXPECT_EQ(PrivateModeProfile::kNotOffTheRecord,
            ClassifyPrivateModeProfile(/*is_off_the_record=*/false,
                                       /*is_incognito_profile=*/false,
                                       /*has_moltnet_otr_id=*/false));
  EXPECT_EQ(PrivateModeProfile::kIncognito,
            ClassifyPrivateModeProfile(/*is_off_the_record=*/true,
                                       /*is_incognito_profile=*/true,
                                       /*has_moltnet_otr_id=*/false));
  EXPECT_EQ(PrivateModeProfile::kMoltNet,
            ClassifyPrivateModeProfile(/*is_off_the_record=*/true,
                                       /*is_incognito_profile=*/false,
                                       /*has_moltnet_otr_id=*/true));
  // The guest session, DevTools contexts, captive-portal windows, ...
  EXPECT_EQ(PrivateModeProfile::kOtherOffTheRecord,
            ClassifyPrivateModeProfile(/*is_off_the_record=*/true,
                                       /*is_incognito_profile=*/false,
                                       /*has_moltnet_otr_id=*/false));
  // Only an off-the-record profile has an OTR id: a regular profile is never
  // taken for MoltNet (or Incognito), whatever the other flags say.
  EXPECT_EQ(PrivateModeProfile::kNotOffTheRecord,
            ClassifyPrivateModeProfile(/*is_off_the_record=*/false,
                                       /*is_incognito_profile=*/true,
                                       /*has_moltnet_otr_id=*/true));
}

TEST(MoltNetIncognitoParityPolicyTest, IncognitoAndMoltNetGetTheProtections) {
  EXPECT_TRUE(GetsIncognitoProtections(PrivateModeProfile::kIncognito));
  EXPECT_TRUE(GetsIncognitoProtections(PrivateModeProfile::kMoltNet));
  EXPECT_FALSE(GetsIncognitoProtections(PrivateModeProfile::kNotOffTheRecord));
  EXPECT_FALSE(
      GetsIncognitoProtections(PrivateModeProfile::kOtherOffTheRecord));
}

// At every hook the helper replaces Profile::IsIncognitoProfile(), which is
// only ever true for an off-the-record profile. For every profile that isn't
// MoltNet's, the answer must stay exactly Chromium's.
TEST(MoltNetIncognitoParityPolicyTest, NonMoltNetProfilesKeepChromiumsAnswer) {
  for (bool off_the_record : {false, true}) {
    for (bool incognito : {false, true}) {
      if (incognito && !off_the_record) {
        continue;  // Not a real profile.
      }
      EXPECT_EQ(incognito,
                GetsIncognitoProtections(ClassifyPrivateModeProfile(
                    off_the_record, incognito, /*has_moltnet_otr_id=*/false)))
          << "off_the_record=" << off_the_record
          << " incognito=" << incognito;
    }
  }
  // And a MoltNet profile (never the primary one) gets them.
  EXPECT_TRUE(GetsIncognitoProtections(ClassifyPrivateModeProfile(
      /*is_off_the_record=*/true, /*is_incognito_profile=*/false,
      /*has_moltnet_otr_id=*/true)));
}

}  // namespace
}  // namespace tor
}  // namespace molt_ai
