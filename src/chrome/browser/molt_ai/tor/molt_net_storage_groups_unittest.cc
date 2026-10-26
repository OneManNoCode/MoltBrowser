// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Which site a storage key's data counts to in MoltNet's per-site storage
// budgets, with real storage keys: localStorage and sessionStorage
// (components/services/storage/dom_storage/molt_dom_storage_site.h) and the
// quota share (storage/browser/quota/molt_storage_budget.h). A
// credentialless iframe's key has its own origin's site as top-level site
// wherever it is embedded; it counts with its nonce, so a site's
// credentialless frames on two sites, or on a site and the site's own
// pages, never share a total or a quota they could read.

#include <cstdint>
#include <map>

#include "base/unguessable_token.h"
#include "components/services/storage/dom_storage/molt_dom_storage_site.h"
#include "net/base/schemeful_site.h"
#include "storage/browser/quota/molt_storage_budget.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/storage_key/storage_key.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace molt_ai::tor {
namespace {

url::Origin O(const char* spec) {
  return url::Origin::Create(GURL(spec));
}

blink::StorageKey Embedded(const char* origin, const char* top_level) {
  return blink::StorageKey::Create(
      O(origin), net::SchemefulSite(O(top_level)),
      blink::mojom::AncestorChainBit::kCrossSite,
      /*third_party_partitioning_allowed=*/true);
}

TEST(MoltNetStorageGroupsTest, KeysCountToTheirTopLevelSite) {
  const blink::StorageKey first_party =
      blink::StorageKey::CreateFirstParty(O("https://a.test"));
  const blink::StorageKey subdomain =
      blink::StorageKey::CreateFirstParty(O("https://x.a.test"));
  const blink::StorageKey embedded = Embedded("https://t.test", "https://a.test");
  EXPECT_EQ(storage::MoltDomStorageSiteOf(first_party), "https://a.test");
  EXPECT_TRUE(storage::MoltDomStorageSameSite(first_party, subdomain));
  EXPECT_TRUE(storage::MoltDomStorageSameSite(first_party, embedded));
  EXPECT_TRUE(storage::MoltSameBudgetSite(first_party, embedded));
  EXPECT_FALSE(storage::MoltSameBudgetSite(
      embedded, blink::StorageKey::CreateFirstParty(O("https://t.test"))));
}

TEST(MoltNetStorageGroupsTest, ACredentiallessFrameCountsWithItsNonce) {
  const base::UnguessableToken page_a = base::UnguessableToken::Create();
  const base::UnguessableToken page_b = base::UnguessableToken::Create();
  const blink::StorageKey tracker =
      blink::StorageKey::CreateFirstParty(O("https://t.test"));
  const blink::StorageKey under_a =
      blink::StorageKey::CreateWithNonce(O("https://t.test"), page_a);
  const blink::StorageKey under_b =
      blink::StorageKey::CreateWithNonce(O("https://t.test"), page_b);
  const blink::StorageKey other_under_a =
      blink::StorageKey::CreateWithNonce(O("https://cdn.test"), page_a);
  // Chromium gives such a key its own origin's site as top-level site.
  ASSERT_EQ(under_a.top_level_site(), tracker.top_level_site());
  ASSERT_EQ(under_a.top_level_site(), under_b.top_level_site());

  EXPECT_FALSE(storage::MoltDomStorageSameSite(under_a, tracker));
  EXPECT_FALSE(storage::MoltDomStorageSameSite(under_a, under_b));
  EXPECT_TRUE(storage::MoltDomStorageSameSite(under_a, other_under_a));
  EXPECT_NE(storage::MoltDomStorageSiteOf(under_a), "https://t.test");

  EXPECT_FALSE(storage::MoltSameBudgetSite(under_a, tracker));
  EXPECT_FALSE(storage::MoltSameBudgetSite(tracker, under_a));
  EXPECT_FALSE(storage::MoltSameBudgetSite(under_a, under_b));
  EXPECT_TRUE(storage::MoltSameBudgetSite(under_a, other_under_a));
}

// What a credentialless frame's quota is computed from: its nonce's usage,
// never the site's own pages' or its frames' under another page.
TEST(MoltNetStorageGroupsTest, AQuotaShareReadsOnlyItsOwnGroup) {
  const base::UnguessableToken page_a = base::UnguessableToken::Create();
  const base::UnguessableToken page_b = base::UnguessableToken::Create();
  const blink::StorageKey tracker =
      blink::StorageKey::CreateFirstParty(O("https://t.test"));
  const blink::StorageKey under_a =
      blink::StorageKey::CreateWithNonce(O("https://t.test"), page_a);
  const blink::StorageKey under_b =
      blink::StorageKey::CreateWithNonce(O("https://t.test"), page_b);
  const std::map<blink::StorageKey, int64_t> usage = {
      {tracker, 1000}, {under_a, 20}, {under_b, 300}};
  EXPECT_EQ(storage::MoltSiteUsage(usage, under_a), 20);
  EXPECT_EQ(storage::MoltSiteUsage(usage, under_b), 300);
  EXPECT_EQ(storage::MoltSiteUsage(usage, tracker), 1000);
}

// Opaque top-level sites all serialize as "null": each such key counts on
// its own, never with every other opaque top-level site's.
TEST(MoltNetStorageGroupsTest, OpaqueTopLevelSitesAreNotOneGroup) {
  const blink::StorageKey under_one = blink::StorageKey::Create(
      O("https://t.test"), net::SchemefulSite(url::Origin()),
      blink::mojom::AncestorChainBit::kCrossSite,
      /*third_party_partitioning_allowed=*/true);
  const blink::StorageKey under_another = blink::StorageKey::Create(
      O("https://t.test"), net::SchemefulSite(url::Origin()),
      blink::mojom::AncestorChainBit::kCrossSite,
      /*third_party_partitioning_allowed=*/true);
  ASSERT_TRUE(under_one.top_level_site().opaque());
  EXPECT_FALSE(storage::MoltDomStorageSameSite(under_one, under_another));
  EXPECT_NE(storage::MoltDomStorageSiteOf(under_one), "null");
  EXPECT_FALSE(storage::MoltSameBudgetSite(under_one, under_another));
}

}  // namespace
}  // namespace molt_ai::tor
