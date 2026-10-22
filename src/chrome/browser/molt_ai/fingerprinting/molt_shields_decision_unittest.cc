// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/fingerprinting/molt_shields_decision.h"

#include "base/memory/scoped_refptr.h"
#include "base/test/task_environment.h"
#include "base/unguessable_token.h"
#include "chrome/browser/molt_ai/shield/molt_shield_site_exceptions.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/common/content_settings.h"
#include "components/content_settings/core/common/content_settings_pattern.h"
#include "components/sync_preferences/testing_pref_service_syncable.h"
#include "net/base/schemeful_site.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/storage_key/storage_key.h"
#include "third_party/blink/public/mojom/storage_key/ancestor_chain_bit.mojom.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace molt_ai::fingerprinting {
namespace {

url::Origin Origin(const char* url) {
  return url::Origin::Create(GURL(url));
}

class MoltShieldsDecisionTest : public testing::Test {
 protected:
  void SetUp() override {
    HostContentSettingsMap::RegisterProfilePrefs(prefs_.registry());
    map_ = base::MakeRefCounted<HostContentSettingsMap>(
        &prefs_, /*is_off_the_record=*/false, /*store_last_modified=*/false,
        /*restore_session=*/false, /*should_record_metrics=*/false);
  }
  void TearDown() override { map_->ShutdownOnUIThread(); }

  // What the MoltShield UI writes for "Shields off" on `url`'s site.
  void ShieldsDownFor(const char* url) {
    map_->SetContentSettingCustomScope(
        ContentSettingsPattern::FromURLToSchemefulSitePattern(GURL(url)),
        ContentSettingsPattern::Wildcard(), shield::kShieldsDownSetting,
        CONTENT_SETTING_ALLOW);
  }

  ShieldsDecision Decide(const url::Origin& top_frame_origin,
                         RendererMode mode = RendererMode::kFarble) {
    return DecideForTopFrameOrigin(mode, map_.get(), top_frame_origin);
  }

  base::test::TaskEnvironment task_environment_;
  sync_preferences::TestingPrefServiceSyncable prefs_;
  scoped_refptr<HostContentSettingsMap> map_;
};

TEST_F(MoltShieldsDecisionTest, ShieldsAreUpByDefault) {
  const ShieldsDecision decision = Decide(Origin("https://www.example.com/"));
  EXPECT_FALSE(decision.shields_down);
  // The site travels with the decision, serialized the way Blink's
  // BlinkSchemefulSite serializes it.
  EXPECT_EQ(decision.top_level_site, "https://example.com");
}

TEST_F(MoltShieldsDecisionTest, ShieldsOffCoversTheWholeTopLevelSite) {
  ShieldsDownFor("https://www.example.com/page");
  for (const char* url :
       {"https://example.com/", "https://www.example.com/x",
        "https://shop.example.com/", "https://example.com:8443/"}) {
    const ShieldsDecision decision = Decide(Origin(url));
    EXPECT_TRUE(decision.shields_down) << url;
    EXPECT_EQ(decision.top_level_site, "https://example.com") << url;
  }
  // Other sites, and the http:// one (sites are schemeful), keep Shields.
  EXPECT_FALSE(Decide(Origin("http://example.com/")).shields_down);
  EXPECT_FALSE(Decide(Origin("https://example.org/")).shields_down);
}

TEST_F(MoltShieldsDecisionTest, AnExceptionForOneSubdomainDoesntCount) {
  // The farbling key is the whole site's, so Shields off on one subdomain
  // leaves farbling on everywhere on the site, that subdomain included.
  map_->SetContentSettingCustomScope(
      ContentSettingsPattern::FromString("https://sub.example.net"),
      ContentSettingsPattern::Wildcard(), shield::kShieldsDownSetting,
      CONTENT_SETTING_ALLOW);
  EXPECT_FALSE(Decide(Origin("https://sub.example.net/")).shields_down);
}

TEST_F(MoltShieldsDecisionTest, OnlyFarblingProfilesHaveShieldsDown) {
  ShieldsDownFor("https://example.com/");
  // MoltNet farbles where Shields are off: it looks like every other MoltNet
  // user.
  EXPECT_FALSE(
      Decide(Origin("https://example.com/"), RendererMode::kUniform)
          .shields_down);
  EXPECT_FALSE(
      Decide(Origin("https://example.com/"), RendererMode::kOff).shields_down);
  // No content settings: Shields up.
  EXPECT_FALSE(DecideForTopFrameOrigin(RendererMode::kFarble, nullptr,
                                       Origin("https://example.com/"))
                   .shields_down);
}

TEST_F(MoltShieldsDecisionTest, SandboxedTopLevelPagesUseTheirPrecursor) {
  ShieldsDownFor("https://example.com/");
  const url::Origin sandboxed =
      Origin("https://www.example.com/").DeriveNewOpaqueOrigin();
  const ShieldsDecision decision = Decide(sandboxed);
  EXPECT_TRUE(decision.shields_down);
  EXPECT_EQ(decision.top_level_site, "https://example.com");

  // An opaque origin without a precursor has no site: Shields up.
  const ShieldsDecision none = Decide(url::Origin());
  EXPECT_FALSE(none.shields_down);
  EXPECT_TRUE(none.top_level_site.empty());
}

TEST_F(MoltShieldsDecisionTest, NonWebTopLevelSitesNeverHaveShieldsDown) {
  ShieldsDownFor("https://example.com/");
  EXPECT_FALSE(Decide(Origin("chrome://settings/")).shields_down);
  EXPECT_FALSE(Decide(Origin("file:///tmp/page.html")).shields_down);
}

TEST_F(MoltShieldsDecisionTest, WorkersFollowTheirStorageKeysTopLevelSite) {
  ShieldsDownFor("https://example.com/");
  const blink::StorageKey first_party =
      blink::StorageKey::CreateFirstParty(Origin("https://www.example.com/"));
  EXPECT_TRUE(
      AreShieldsDownForWorker(RendererMode::kFarble, map_.get(), first_party));
  EXPECT_FALSE(
      AreShieldsDownForWorker(RendererMode::kUniform, map_.get(), first_party));
  EXPECT_FALSE(
      AreShieldsDownForWorker(RendererMode::kFarble, nullptr, first_party));
  EXPECT_FALSE(AreShieldsDownForWorker(
      RendererMode::kFarble, map_.get(),
      blink::StorageKey::CreateFirstParty(Origin("https://example.org/"))));

  // A nonce-keyed worker (fenced frame, credentialless iframe): its key's
  // top-level site is its own.
  EXPECT_TRUE(AreShieldsDownForWorker(
      RendererMode::kFarble, map_.get(),
      blink::StorageKey::CreateWithNonce(Origin("https://example.com/"),
                                         base::UnguessableToken::Create())));

  if (!blink::StorageKey::IsThirdPartyStoragePartitioningEnabled()) {
    GTEST_SKIP() << "third-party storage partitioning is off";
  }
  // A tracker's worker under example.com: example.com's decision, like the
  // tracker's iframes there.
  EXPECT_TRUE(AreShieldsDownForWorker(
      RendererMode::kFarble, map_.get(),
      blink::StorageKey::Create(
          Origin("https://tracker.example/"),
          net::SchemefulSite(Origin("https://example.com/")),
          blink::mojom::AncestorChainBit::kCrossSite)));
  // The same tracker under another site: that site's decision, not its own.
  ShieldsDownFor("https://tracker.example/");
  EXPECT_FALSE(AreShieldsDownForWorker(
      RendererMode::kFarble, map_.get(),
      blink::StorageKey::Create(
          Origin("https://tracker.example/"),
          net::SchemefulSite(Origin("https://example.org/")),
          blink::mojom::AncestorChainBit::kCrossSite)));
}

}  // namespace
}  // namespace molt_ai::fingerprinting
