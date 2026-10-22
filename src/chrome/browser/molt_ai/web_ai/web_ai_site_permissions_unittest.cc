// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/web_ai/web_ai_site_permissions.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai::web_ai {
namespace {

constexpr int64_t kDayMs = int64_t{24} * 60 * 60 * 1000;
// 2026-10-22, in ms since the Unix epoch.
constexpr int64_t kNow = int64_t{1792627200} * 1000;

const std::string kA = "https://a.example";
const std::string kB = "https://b.example";
const std::string kC = "http://c.example:8080";

std::string SiteN(size_t n) {
  return "https://s" + std::to_string(n) + ".example";
}

// Closes the prompt |times| times, a minute apart, ending at |end|.
void Dismiss(SitePermissionTable& table,
             const std::string& site,
             int times,
             int64_t end) {
  for (int i = times - 1; i >= 0; --i) {
    table.RecordDismissal(site, end - i * 60 * 1000);
  }
}

// ---------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------

TEST(WebAISitePermissionsTest, PlausibleSiteKeys) {
  for (const char* good :
       {"https://example.com", "http://example.com:8080",
        "https://xn--bcher-kva.example", "http://[::1]:8080",
        "https://192.0.2.1"}) {
    EXPECT_TRUE(IsPlausibleSiteKey(good)) << good;
  }
  const std::string nul_inside("https://a\0b.example", 19);
  for (const std::string& bad : std::vector<std::string>{
           "", "https://", "http://", "ftp://example.com",
           "chrome://settings", "HTTPS://EXAMPLE.COM", "https://a.example/",
           "https://a.example/path", "https://a.example?q", "https://a#f",
           "https://user@a.example", "https://a b.example",
           "https://a\tb.example", "https://a\\b.example",
           "https://b\xc3\xbc" "cher.example", "https://a\x7f", nul_inside,
           " https://a.example", "https://a.example "}) {
    EXPECT_FALSE(IsPlausibleSiteKey(bad)) << bad;
  }
  std::string longest = "https://" + std::string(kMaxSiteKeyBytes - 8, 'a');
  EXPECT_TRUE(IsPlausibleSiteKey(longest));
  EXPECT_FALSE(IsPlausibleSiteKey(longest + "a"));
}

// ---------------------------------------------------------------------------
// Answers
// ---------------------------------------------------------------------------

TEST(WebAISitePermissionsTest, AnswersAreRememberedPerSite) {
  SitePermissionTable table;
  EXPECT_EQ(table.Get(kA, kNow), SiteConsent::kAsk);
  EXPECT_TRUE(table.SetAnswer(kA, SiteConsent::kAllowed, kNow));
  EXPECT_TRUE(table.SetAnswer(kB, SiteConsent::kBlocked, kNow));
  EXPECT_EQ(table.Get(kA, kNow), SiteConsent::kAllowed);
  EXPECT_EQ(table.Get(kB, kNow), SiteConsent::kBlocked);
  EXPECT_EQ(table.Get(kC, kNow), SiteConsent::kAsk);
  // Answers don't expire.
  EXPECT_EQ(table.Get(kA, kNow + 3650 * kDayMs), SiteConsent::kAllowed);
  // A new answer replaces the old one; kAsk forgets the site.
  EXPECT_TRUE(table.SetAnswer(kA, SiteConsent::kBlocked, kNow + 1));
  EXPECT_EQ(table.Get(kA, kNow), SiteConsent::kBlocked);
  EXPECT_EQ(table.Find(kA)->answered_ms, kNow + 1);
  EXPECT_TRUE(table.SetAnswer(kA, SiteConsent::kAsk, kNow));
  EXPECT_EQ(table.Find(kA), nullptr);
  EXPECT_EQ(table.size(), 1u);
}

TEST(WebAISitePermissionsTest, ImplausibleKeysAreNotStored) {
  SitePermissionTable table;
  EXPECT_FALSE(table.SetAnswer("", SiteConsent::kAllowed, kNow));
  EXPECT_FALSE(table.SetAnswer("file:///etc", SiteConsent::kAllowed, kNow));
  EXPECT_FALSE(table.RecordDismissal("https://a.example/x", kNow));
  EXPECT_FALSE(table.Insert("javascript:alert(1)",
                            SiteRecord{SiteConsent::kAllowed, kNow, 0, 0}));
  EXPECT_EQ(table.size(), 0u);
}

// ---------------------------------------------------------------------------
// Embargo
// ---------------------------------------------------------------------------

TEST(WebAISitePermissionsTest, ThirdCloseSetsAWeekLongPause) {
  SitePermissionTable table;
  EXPECT_FALSE(table.RecordDismissal(kA, kNow - 2000));
  EXPECT_FALSE(table.RecordDismissal(kA, kNow - 1000));
  EXPECT_EQ(table.Get(kA, kNow), SiteConsent::kAsk);
  EXPECT_TRUE(table.RecordDismissal(kA, kNow));
  EXPECT_TRUE(IsUnderEmbargo(*table.Find(kA), kNow));
  // A paused site looks exactly like a blocked one.
  EXPECT_EQ(table.Get(kA, kNow), SiteConsent::kBlocked);
  EXPECT_EQ(table.Get(kA, kNow + kEmbargoMs - 1), SiteConsent::kBlocked);
  EXPECT_EQ(table.Get(kA, kNow + kEmbargoMs), SiteConsent::kAsk);
  // The count stays: the next close pauses it again right away.
  EXPECT_TRUE(table.RecordDismissal(kA, kNow + 8 * kDayMs));
  EXPECT_EQ(table.Find(kA)->dismissals, 4u);
  EXPECT_EQ(table.Get(kA, kNow + 8 * kDayMs + kEmbargoMs - 1),
            SiteConsent::kBlocked);
  // Other sites aren't affected.
  EXPECT_EQ(table.Get(kB, kNow), SiteConsent::kAsk);
}

TEST(WebAISitePermissionsTest, AnAnswerEndsThePauseAndItsHistory) {
  SitePermissionTable table;
  Dismiss(table, kA, 3, kNow);
  ASSERT_EQ(table.Get(kA, kNow), SiteConsent::kBlocked);
  // The user can still allow it (from the settings page's point of view:
  // remove, then answer the next prompt; here directly).
  table.SetAnswer(kA, SiteConsent::kAllowed, kNow + 1);
  EXPECT_EQ(table.Get(kA, kNow + 2), SiteConsent::kAllowed);
  EXPECT_EQ(table.Find(kA)->dismissals, 0u);
  // Closes don't count for an answered site (no prompt is shown for it).
  EXPECT_FALSE(table.RecordDismissal(kA, kNow + 3));
  EXPECT_EQ(table.Find(kA)->dismissals, 0u);
  EXPECT_EQ(table.Get(kA, kNow + 3), SiteConsent::kAllowed);
}

// The page raises the prompt on a click: a click that follows within the
// double-click interval (a double click it asked for) is not an answer.
TEST(WebAISitePermissionsTest, AnswersTooSoonAfterThePromptDontCount) {
  EXPECT_FALSE(IsTimelyPromptAnswer(0));
  EXPECT_FALSE(IsTimelyPromptAnswer(1));
  EXPECT_FALSE(IsTimelyPromptAnswer(kPromptInputProtectionMs - 1));
  EXPECT_TRUE(IsTimelyPromptAnswer(kPromptInputProtectionMs));
  EXPECT_TRUE(IsTimelyPromptAnswer(60 * 1000));
  // A monotonic clock never goes back, but if it did, it isn't an answer.
  EXPECT_FALSE(IsTimelyPromptAnswer(-1));
  EXPECT_FALSE(IsTimelyPromptAnswer(std::numeric_limits<int64_t>::min()));
  EXPECT_GE(kPromptInputProtectionMs, 500);
}

TEST(WebAISitePermissionsTest, ClockChangesNeverStretchAPause) {
  SiteRecord record;
  record.dismissals = kDismissalsBeforeEmbargo;
  record.last_dismissal_ms = kNow;
  // The clock set back a little: still paused; a lot: not paused.
  EXPECT_TRUE(IsUnderEmbargo(record, kNow - kDayMs));
  EXPECT_FALSE(IsUnderEmbargo(record, kNow - kEmbargoMs));
  EXPECT_FALSE(IsUnderEmbargo(record, kNow - 400 * kDayMs));
  // Extreme values don't overflow.
  EXPECT_FALSE(IsUnderEmbargo(record, std::numeric_limits<int64_t>::max()));
  EXPECT_FALSE(IsUnderEmbargo(record, std::numeric_limits<int64_t>::min()));
  record.last_dismissal_ms = std::numeric_limits<int64_t>::max();
  EXPECT_FALSE(IsUnderEmbargo(record, kNow));
  // Two closes are never a pause, an answer never is.
  record.last_dismissal_ms = kNow;
  record.dismissals = kDismissalsBeforeEmbargo - 1;
  EXPECT_FALSE(IsUnderEmbargo(record, kNow));
  record.dismissals = kDismissalsBeforeEmbargo;
  record.answer = SiteConsent::kAllowed;
  EXPECT_FALSE(IsUnderEmbargo(record, kNow));
  EXPECT_EQ(EffectiveConsent(record, kNow), SiteConsent::kAllowed);
}

// ---------------------------------------------------------------------------
// The settings page
// ---------------------------------------------------------------------------

TEST(WebAISitePermissionsTest, ListShowsAnswersAndPauses) {
  SitePermissionTable table;
  table.SetAnswer(kC, SiteConsent::kAllowed, kNow - 5);
  table.SetAnswer(kA, SiteConsent::kAllowed, kNow - 4);
  table.SetAnswer(kB, SiteConsent::kBlocked, kNow - 3);
  Dismiss(table, "https://paused.example", 3, kNow - 2);
  Dismiss(table, "https://twice.example", 2, kNow - 1);
  const std::vector<SiteListEntry> rows = table.List(kNow);
  ASSERT_EQ(rows.size(), 4u);
  EXPECT_EQ(rows[0].site, kC);  // "http://..." sorts before "https://..."
  EXPECT_EQ(rows[0].kind, SiteListEntry::Kind::kAllowed);
  EXPECT_EQ(rows[1].site, kA);
  EXPECT_EQ(rows[1].kind, SiteListEntry::Kind::kAllowed);
  EXPECT_EQ(rows[1].since_ms, kNow - 4);
  EXPECT_EQ(rows[2].site, kB);
  EXPECT_EQ(rows[2].kind, SiteListEntry::Kind::kBlocked);
  EXPECT_EQ(rows[3].site, "https://paused.example");
  EXPECT_EQ(rows[3].kind, SiteListEntry::Kind::kPaused);
  EXPECT_EQ(rows[3].since_ms, kNow - 2);
  EXPECT_EQ(rows[3].until_ms, kNow - 2 + kEmbargoMs);
  // Once the pause is over the row goes (the history stays, unlisted).
  EXPECT_EQ(table.List(kNow + kEmbargoMs).size(), 3u);
  EXPECT_EQ(table.size(), 5u);
}

TEST(WebAISitePermissionsTest, RemoveMatchesOnlyTheExactKey) {
  SitePermissionTable table;
  table.SetAnswer(kA, SiteConsent::kBlocked, kNow);
  Dismiss(table, kB, 3, kNow);
  for (const char* other :
       {"", "https://a.example/", "HTTPS://A.EXAMPLE", "https://a.exampl",
        "https://a.example.", " https://a.example", "https://a.example\n",
        "a.example", "https://[*.]a.example"}) {
    EXPECT_FALSE(table.Remove(other)) << other;
  }
  EXPECT_EQ(table.size(), 2u);
  EXPECT_TRUE(table.Remove(kA));
  EXPECT_FALSE(table.Remove(kA));
  EXPECT_EQ(table.Get(kA, kNow), SiteConsent::kAsk);
  // Removing a paused site lets it ask again, from zero.
  EXPECT_TRUE(table.Remove(kB));
  EXPECT_FALSE(table.RecordDismissal(kB, kNow + 1));
  EXPECT_EQ(table.Get(kB, kNow + 1), SiteConsent::kAsk);
  EXPECT_TRUE(table.Clear());
  EXPECT_FALSE(table.Clear());
  EXPECT_EQ(table.size(), 0u);
}

// ---------------------------------------------------------------------------
// Clear browsing data
// ---------------------------------------------------------------------------

TEST(WebAISitePermissionsTest, SiteSettingsClearsAnswersInTheRange) {
  SitePermissionTable table;
  table.SetAnswer(kA, SiteConsent::kAllowed, kNow - 2 * kDayMs);
  table.SetAnswer(kB, SiteConsent::kBlocked, kNow - 10);
  table.SetAnswer(kC, SiteConsent::kAllowed, kNow - 5);
  Dismiss(table, "https://paused.example", 3, kNow - 1);
  const std::vector<std::string> all = table.Sites();
  // "Last hour": B and C go, A (two days old) and the prompt history stay.
  EXPECT_EQ(table.RemoveAnswersBetween(kNow - 3600 * 1000,
                                       std::numeric_limits<int64_t>::max(),
                                       all),
            2u);
  EXPECT_EQ(table.Get(kA, kNow), SiteConsent::kAllowed);
  EXPECT_EQ(table.Get(kB, kNow), SiteConsent::kAsk);
  EXPECT_EQ(table.Get("https://paused.example", kNow), SiteConsent::kBlocked);
  // Only the sites the filter matched.
  EXPECT_EQ(table.RemoveAnswersBetween(0, std::numeric_limits<int64_t>::max(),
                                       {kB, kC, "https://other.example"}),
            0u);
  EXPECT_EQ(table.RemoveAnswersBetween(0, std::numeric_limits<int64_t>::max(),
                                       {kA}),
            1u);
  EXPECT_EQ(table.size(), 1u);
}

TEST(WebAISitePermissionsTest, SiteSettingsRangeIsHalfOpen) {
  SitePermissionTable table;
  table.SetAnswer(kA, SiteConsent::kAllowed, kNow);
  // [begin, end): an answer at |end| stays, one at |begin| goes.
  EXPECT_EQ(table.RemoveAnswersBetween(kNow - 10, kNow, {kA}), 0u);
  EXPECT_EQ(table.RemoveAnswersBetween(kNow + 1, kNow + 10, {kA}), 0u);
  EXPECT_EQ(table.Get(kA, kNow), SiteConsent::kAllowed);
  EXPECT_EQ(table.RemoveAnswersBetween(kNow, kNow + 1, {kA}), 1u);
  EXPECT_EQ(table.Get(kA, kNow), SiteConsent::kAsk);
}

TEST(WebAISitePermissionsTest, HistoryClearsThePromptHistoryOnly) {
  SitePermissionTable table;
  table.SetAnswer(kA, SiteConsent::kBlocked, kNow);
  Dismiss(table, kB, 3, kNow);
  Dismiss(table, kC, 1, kNow);
  EXPECT_EQ(table.RemovePromptHistory({kA, kB}), 1u);
  EXPECT_EQ(table.Get(kA, kNow), SiteConsent::kBlocked);
  EXPECT_EQ(table.Get(kB, kNow), SiteConsent::kAsk);
  EXPECT_NE(table.Find(kC), nullptr);
  EXPECT_EQ(table.RemovePromptHistory(table.Sites()), 1u);
  EXPECT_EQ(table.size(), 1u);
}

// ---------------------------------------------------------------------------
// Bounds
// ---------------------------------------------------------------------------

TEST(WebAISitePermissionsTest, FullTableDropsTheOldestHistoryFirst) {
  SitePermissionTable table;
  // Answers, oldest first, then two sites with prompt history.
  for (size_t i = 0; i + 2 < SitePermissionTable::kMaxSites; ++i) {
    table.SetAnswer(SiteN(i), SiteConsent::kBlocked,
                    kNow + static_cast<int64_t>(i));
  }
  table.RecordDismissal("https://h1.example", kNow + 100000);
  table.RecordDismissal("https://h2.example", kNow + 200000);
  ASSERT_EQ(table.size(), SitePermissionTable::kMaxSites);
  // A new answer pushes out the oldest prompt history, not an answer.
  table.SetAnswer("https://new1.example", SiteConsent::kAllowed, kNow + 1);
  EXPECT_EQ(table.Find("https://h1.example"), nullptr);
  EXPECT_NE(table.Find(SiteN(0)), nullptr);
  table.SetAnswer("https://new2.example", SiteConsent::kAllowed, kNow + 2);
  EXPECT_EQ(table.Find("https://h2.example"), nullptr);
  // With only answers left, the oldest answer goes.
  table.RecordDismissal("https://h3.example", kNow + 3);
  EXPECT_EQ(table.Find(SiteN(0)), nullptr);
  EXPECT_NE(table.Find(SiteN(1)), nullptr);
  EXPECT_EQ(table.size(), SitePermissionTable::kMaxSites);
  // Changing a site that's already there never evicts anything.
  table.SetAnswer(SiteN(1), SiteConsent::kAllowed, kNow + 4);
  table.RecordDismissal("https://h3.example", kNow + 5);
  EXPECT_NE(table.Find(SiteN(2)), nullptr);
  EXPECT_EQ(table.size(), SitePermissionTable::kMaxSites);
}

TEST(WebAISitePermissionsTest, DismissalCountSaturates) {
  SitePermissionTable table;
  for (uint32_t i = 0; i < kMaxStoredDismissals + 50; ++i) {
    table.RecordDismissal(kA, kNow);
  }
  EXPECT_EQ(table.Find(kA)->dismissals, kMaxStoredDismissals);
}

// ---------------------------------------------------------------------------
// The stored form (and its migration rules)
// ---------------------------------------------------------------------------

TEST(WebAISitePermissionsTest, StoredVersions) {
  EXPECT_EQ(ClassifyStoredVersion(std::nullopt), StoredFormat::kDiscard);
  EXPECT_EQ(ClassifyStoredVersion(0), StoredFormat::kDiscard);
  EXPECT_EQ(ClassifyStoredVersion(-1), StoredFormat::kDiscard);
  EXPECT_EQ(ClassifyStoredVersion(kStoredFormatVersion), StoredFormat::kRead);
  // A later format only adds fields; this code reads the ones it knows.
  EXPECT_EQ(ClassifyStoredVersion(kStoredFormatVersion + 1),
            StoredFormat::kRead);
}

TEST(WebAISitePermissionsTest, StoredAnswers) {
  StoredSite allow;
  allow.answer = "allow";
  allow.answered_ms = static_cast<double>(kNow - kDayMs);
  std::optional<SiteRecord> r = RecordFromStored(allow, kNow);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->answer, SiteConsent::kAllowed);
  EXPECT_EQ(r->answered_ms, kNow - kDayMs);

  StoredSite block;
  block.answer = "block";
  block.answered_ms = static_cast<double>(kNow);
  // History stored beside an answer is dropped (an answer clears it).
  block.dismissals = 5;
  block.last_dismissal_ms = static_cast<double>(kNow);
  r = RecordFromStored(block, kNow);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->answer, SiteConsent::kBlocked);
  EXPECT_EQ(r->dismissals, 0u);

  // Missing, broken or future times count as now.
  for (std::optional<double> t :
       {std::optional<double>(), std::optional<double>(std::nan("")),
        std::optional<double>(std::numeric_limits<double>::infinity()),
        std::optional<double>(-1.0), std::optional<double>(1e300),
        std::optional<double>(static_cast<double>(kNow + kDayMs))}) {
    allow.answered_ms = t;
    r = RecordFromStored(allow, kNow);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->answered_ms, kNow);
  }

  // Other strings aren't answers.
  for (const char* other : {"ALLOW", "allow ", "ask", "", "granted"}) {
    StoredSite s;
    s.answer = other;
    s.answered_ms = static_cast<double>(kNow);
    EXPECT_FALSE(RecordFromStored(s, kNow)) << other;
  }
}

TEST(WebAISitePermissionsTest, StoredPromptHistory) {
  StoredSite s;
  s.dismissals = 3;
  s.last_dismissal_ms = static_cast<double>(kNow - kDayMs);
  std::optional<SiteRecord> r = RecordFromStored(s, kNow);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->answer, SiteConsent::kAsk);
  EXPECT_EQ(r->dismissals, 3u);
  EXPECT_TRUE(IsUnderEmbargo(*r, kNow));

  s.dismissals = 1000000;
  EXPECT_EQ(RecordFromStored(s, kNow)->dismissals, kMaxStoredDismissals);
  // An unknown answer string with history is just history.
  s.answer = "maybe";
  EXPECT_TRUE(RecordFromStored(s, kNow));

  for (std::optional<int> bad : {std::optional<int>(), std::optional<int>(0),
                                 std::optional<int>(-3)}) {
    s.dismissals = bad;
    EXPECT_FALSE(RecordFromStored(s, kNow));
  }
  s.dismissals = 2;
  for (std::optional<double> bad :
       {std::optional<double>(), std::optional<double>(std::nan("")),
        std::optional<double>(-5.0),
        std::optional<double>(static_cast<double>(kMaxStoredTimeMs) * 2)}) {
    s.last_dismissal_ms = bad;
    EXPECT_FALSE(RecordFromStored(s, kNow));
  }
  // An empty entry is dropped.
  EXPECT_FALSE(RecordFromStored(StoredSite(), kNow));
}

TEST(WebAISitePermissionsTest, StoredFormRoundTrips) {
  SitePermissionTable table;
  table.SetAnswer(kA, SiteConsent::kAllowed, kNow - 7);
  table.SetAnswer(kB, SiteConsent::kBlocked, kNow - 6);
  Dismiss(table, kC, 4, kNow - 5);
  SitePermissionTable reloaded;
  for (const auto& [site, record] : table.records()) {
    std::optional<SiteRecord> back =
        RecordFromStored(StoredFromRecord(record), kNow);
    ASSERT_TRUE(back) << site;
    EXPECT_TRUE(reloaded.Insert(site, *back));
  }
  ASSERT_EQ(reloaded.size(), table.size());
  for (const auto& [site, record] : table.records()) {
    const SiteRecord* other = reloaded.Find(site);
    ASSERT_NE(other, nullptr);
    EXPECT_EQ(other->answer, record.answer);
    EXPECT_EQ(other->answered_ms, record.answered_ms);
    EXPECT_EQ(other->dismissals, record.dismissals);
    EXPECT_EQ(other->last_dismissal_ms, record.last_dismissal_ms);
  }
  EXPECT_EQ(StoredFromRecord(*table.Find(kA)).answer, std::string("allow"));
  EXPECT_EQ(StoredFromRecord(*table.Find(kB)).answer, std::string("block"));
  EXPECT_FALSE(StoredFromRecord(*table.Find(kC)).answer);
}

TEST(WebAISitePermissionsTest, InsertCleansRecords) {
  SitePermissionTable table;
  EXPECT_FALSE(table.Insert(kA, SiteRecord()));
  SiteRecord mixed{SiteConsent::kBlocked, -5, 7, kNow};
  EXPECT_TRUE(table.Insert(kA, mixed));
  EXPECT_EQ(table.Find(kA)->dismissals, 0u);
  EXPECT_EQ(table.Find(kA)->answered_ms, 0);
  SiteRecord history{SiteConsent::kAsk, 0, 1000, kNow};
  EXPECT_TRUE(table.Insert(kB, history));
  EXPECT_EQ(table.Find(kB)->dismissals, kMaxStoredDismissals);
  for (size_t i = 0; i < 2 * SitePermissionTable::kMaxSites; ++i) {
    table.Insert(SiteN(i), SiteRecord{SiteConsent::kAllowed,
                                      kNow + static_cast<int64_t>(i), 0, 0});
  }
  EXPECT_EQ(table.size(), SitePermissionTable::kMaxSites);
}

// Random operations keep the table's invariants: an answered site has no
// history, the table stays bounded, and Get() agrees with List().
TEST(WebAISitePermissionsTest, InvariantsUnderRandomOperations) {
  SitePermissionTable table;
  uint64_t state = 0x9e3779b97f4a7c15u;
  auto next = [&state]() {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  };
  int64_t now = kNow;
  for (int step = 0; step < 20000; ++step) {
    const std::string site = SiteN(next() % 700);
    now += static_cast<int64_t>(next() % (2 * kDayMs));
    switch (next() % 6) {
      case 0:
        table.SetAnswer(site, SiteConsent::kAllowed, now);
        break;
      case 1:
        table.SetAnswer(site, SiteConsent::kBlocked, now);
        break;
      case 2:
      case 3:
        table.RecordDismissal(site, now);
        break;
      case 4:
        table.Remove(site);
        break;
      case 5:
        if (next() % 50 == 0) {
          table.RemovePromptHistory(table.Sites());
        }
        break;
    }
    ASSERT_LE(table.size(), SitePermissionTable::kMaxSites);
  }
  const std::vector<SiteListEntry> rows = table.List(now);
  size_t listed = 0;
  for (const auto& [site, record] : table.records()) {
    if (record.answer != SiteConsent::kAsk) {
      EXPECT_EQ(record.dismissals, 0u);
    } else {
      EXPECT_GE(record.dismissals, 1u);
    }
    const SiteConsent consent = table.Get(site, now);
    if (consent != SiteConsent::kAsk) {
      ++listed;
    }
  }
  EXPECT_EQ(rows.size(), listed);
}

}  // namespace
}  // namespace molt_ai::web_ai
