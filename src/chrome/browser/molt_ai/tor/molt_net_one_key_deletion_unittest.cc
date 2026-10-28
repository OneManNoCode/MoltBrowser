// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// What a deletion of one storage key's data reads where storage is kept per
// site (content/browser/browsing_data/molt_one_key_deletion_policy.h, std-
// only): Clear-Site-Data's deletion reads its own key's usage, never the
// whole partition's.

#include "content/browser/browsing_data/molt_one_key_deletion_policy.h"

#include "testing/gtest/include/gtest/gtest.h"

namespace content {
namespace {

// A deletion names one storage key when it is given the key, or a deleting
// filter with its storage key set (Clear-Site-Data's); a preserving filter,
// or one without a storage key, names none.
static_assert(MoltDeletionNamesOneStorageKey(/*storage_key_given=*/true,
                                             /*deleting_filter=*/false,
                                             /*filter_has_storage_key=*/false));
static_assert(MoltDeletionNamesOneStorageKey(false, true, true));
static_assert(!MoltDeletionNamesOneStorageKey(false, true, false));
static_assert(!MoltDeletionNamesOneStorageKey(false, false, true));
static_assert(!MoltDeletionNamesOneStorageKey(false, false, false));

TEST(MoltNetOneKeyDeletionTest, ClearSiteDataReadsItsOwnKey) {
  // Clear-Site-Data: a deleting filter with the response's storage key, over
  // all time, in MoltNet's partition.
  const bool names_one_key = MoltDeletionNamesOneStorageKey(
      /*storage_key_given=*/false, /*deleting_filter=*/true,
      /*filter_has_storage_key=*/true);
  EXPECT_EQ(MoltDomStorageDeletionReads(/*keeps_per_site=*/true,
                                        names_one_key),
            MoltDeletionReads::kOneStorageKey);
  EXPECT_EQ(MoltQuotaDeletionReads(/*keeps_per_site=*/true, names_one_key,
                                   /*all_time=*/true),
            MoltDeletionReads::kOneStorageKey);
}

TEST(MoltNetOneKeyDeletionTest, OtherDeletionsReadTheWholePartition) {
  // Every other profile: Chromium's listing.
  EXPECT_EQ(MoltDomStorageDeletionReads(/*keeps_per_site=*/false, true),
            MoltDeletionReads::kWholePartition);
  EXPECT_EQ(MoltQuotaDeletionReads(false, true, true),
            MoltDeletionReads::kWholePartition);
  // A deletion that names no key (clearing browsing data, by domain): the
  // whole listing, which its filter needs.
  EXPECT_EQ(MoltDomStorageDeletionReads(true, /*names_one_key=*/false),
            MoltDeletionReads::kWholePartition);
  EXPECT_EQ(MoltQuotaDeletionReads(true, false, true),
            MoltDeletionReads::kWholePartition);
  // A time range in another profile, or naming no key: the whole listing.
  EXPECT_EQ(MoltQuotaDeletionReads(false, true, /*all_time=*/false),
            MoltDeletionReads::kWholePartition);
  EXPECT_EQ(MoltQuotaDeletionReads(true, false, /*all_time=*/false),
            MoltDeletionReads::kWholePartition);
}

// A deletion of one key over a time range reads that key's buckets modified
// in the range (the rows Chromium's listing reads, of that key), not every
// site's.
TEST(MoltNetOneKeyDeletionTest, ATimeRangeReadsItsOwnKeysBuckets) {
  EXPECT_EQ(MoltQuotaDeletionReads(/*keeps_per_site=*/true,
                                   /*names_one_key=*/true,
                                   /*all_time=*/false),
            MoltDeletionReads::kOneStorageKeyModifiedBetween);
  // Its DOM storage reads its own areas whatever the range.
  EXPECT_EQ(MoltDomStorageDeletionReads(true, true),
            MoltDeletionReads::kOneStorageKey);
}

// A Clear-Site-Data response the browser can't place in a document or
// worker names the key a document of its origin would have where the
// request was made, where storage is kept per site, so its deletion reads
// that key only.
TEST(MoltNetOneKeyDeletionTest, AResponseWithNoDocumentNamesItsOwnKey) {
  // A request the browser made with no partition (no network key): the
  // origin's first-party key.
  EXPECT_EQ(MoltClearSiteDataKeyFor(/*keeps_per_site=*/true,
                                    /*has_calculated_key=*/false,
                                    /*origin_opaque=*/false,
                                    /*request_has_nonce=*/false,
                                    /*request_has_partition=*/false),
            MoltClearSiteDataKey::kFirstParty);
  // A page's request (a keepalive request after its page went, a
  // download's): the key in the request's partition.
  EXPECT_EQ(MoltClearSiteDataKeyFor(true, false, false, false, true),
            MoltClearSiteDataKey::kInRequestPartition);
  // A credentialless frame's request (after its page went): its nonce's.
  EXPECT_EQ(MoltClearSiteDataKeyFor(true, false, false, true, true),
            MoltClearSiteDataKey::kWithNonce);
  // A response in a document or worker keeps the key calculated for it.
  for (const bool nonce : {false, true}) {
    for (const bool partition : {false, true}) {
      EXPECT_EQ(MoltClearSiteDataKeyFor(true, true, false, nonce, partition),
                MoltClearSiteDataKey::kAsCalculated);
      // Every other profile: Chromium's (no key, the origin's listing).
      EXPECT_EQ(MoltClearSiteDataKeyFor(false, false, false, nonce, partition),
                MoltClearSiteDataKey::kAsCalculated);
      // An opaque origin has no key of its own to name.
      EXPECT_EQ(MoltClearSiteDataKeyFor(true, false, true, nonce, partition),
                MoltClearSiteDataKey::kAsCalculated);
    }
  }
}

// The key in the request's partition is the origin's first-party key only
// for a request its own site's page made with no cross-site frame on the
// way: a request another site's page made names the origin's storage under
// that site, which is all a document of the origin there could delete.
TEST(MoltNetOneKeyDeletionTest, AnotherSitesRequestNamesItsPartition) {
  // The origin's own page: same-site partition of its own site.
  EXPECT_FALSE(MoltClearSiteDataKeyIsCrossSite(
      /*partition_is_third_party=*/false, /*partition_site_is_origins=*/true));
  // Another site's page (its document's request to the origin, or a frame
  // of the origin embedded there).
  EXPECT_TRUE(MoltClearSiteDataKeyIsCrossSite(true, false));
  // Its own site, under a cross-site frame (A embeds B embeds A).
  EXPECT_TRUE(MoltClearSiteDataKeyIsCrossSite(true, true));
  // A same-site partition of another site never makes a same-site key
  // (blink::StorageKey::IsValid() would refuse it).
  EXPECT_TRUE(MoltClearSiteDataKeyIsCrossSite(false, false));
}

}  // namespace
}  // namespace content
