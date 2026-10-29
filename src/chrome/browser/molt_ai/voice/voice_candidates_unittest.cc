// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_candidates.h"

#include <optional>
#include <string>
#include <vector>

#include "chrome/browser/molt_ai/voice/page_map.h"
#include "chrome/browser/molt_ai/voice/page_map_test_trees.h"
#include "chrome/browser/molt_ai/voice/spoken_choice.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace voice {
namespace {

ChoiceMatch Match(const CandidateSet& set, const char* said, bool bare = true) {
  std::optional<ChoiceRef> ref =
      ParseChoiceRef(said, Language::kEnglish, bare);
  EXPECT_TRUE(ref) << said;
  return MatchSpokenChoice(ref.value_or(ChoiceRef()), set.candidates,
                           MatchOptions());
}

const ChoiceField* Field(const ChoiceCandidate& c, ChoiceFieldKind kind) {
  for (const ChoiceField& field : c.fields) {
    if (field.kind == kind) {
      return &field;
    }
  }
  return nullptr;
}

TEST(VoiceCandidatesTest, ResultsBecomeCandidatesWithTheirFields) {
  const PageMap map = BuildPageMap(test::YouTubeLikeResults());
  ASSERT_FALSE(map.lists.empty());
  const PageList& results = map.lists[0];
  const CandidateSet set = CandidatesFromList(map, results, false);
  ASSERT_EQ(set.size(), static_cast<size_t>(test::kVideoResults));
  ASSERT_EQ(set.targets.size(), set.size());
  for (int i = 0; i < test::kVideoResults; ++i) {
    SCOPED_TRACE(i);
    const ChoiceCandidate& c = set.candidates[i];
    const CandidateTarget& t = set.targets[i];
    EXPECT_EQ(c.position, i + 1);
    EXPECT_EQ(c.screen_number, 0);
    EXPECT_EQ(c.role, ChoiceRole::kVideo);
    EXPECT_EQ(c.title, test::VideoResult(i).title);
    ASSERT_TRUE(Field(c, ChoiceFieldKind::kAuthor));
    EXPECT_EQ(Field(c, ChoiceFieldKind::kAuthor)->text,
              test::VideoResult(i).channel);
    ASSERT_TRUE(Field(c, ChoiceFieldKind::kDuration));
    EXPECT_EQ(Field(c, ChoiceFieldKind::kDuration)->text,
              test::VideoResult(i).duration);
    ASSERT_TRUE(Field(c, ChoiceFieldKind::kDate));
    ASSERT_TRUE(Field(c, ChoiceFieldKind::kCount));
    EXPECT_LE(c.fields.size(), kMaxChoiceFields);

    EXPECT_EQ(t.source, CandidateSource::kListEntry);
    EXPECT_EQ(t.index, i);
    EXPECT_EQ(t.item_number, results.entries[i].number);
    EXPECT_EQ(t.handle, results.entries[i].handle);
    EXPECT_EQ(t.kind, ItemKind::kLink);
    EXPECT_FALSE(t.url.empty());
  }
}

TEST(VoiceCandidatesTest, TheOwnersChoicesOnTheResults) {
  const PageMap map = BuildPageMap(test::YouTubeLikeResults());
  const CandidateSet set = CandidatesFromList(map, map.lists[0], false);

  ChoiceMatch m = Match(set, "the fourth one");
  ASSERT_EQ(m.outcome, ChoiceOutcome::kUnique);
  EXPECT_EQ(set.candidates[m.index].title,
            "Queen - Bohemian Rhapsody (Live Aid 1985)");

  // Three live videos by Queen: the browser asks which.
  m = Match(set, "the live one by Queen");
  ASSERT_EQ(m.outcome, ChoiceOutcome::kAmbiguous);
  std::vector<std::string> close;
  for (size_t i : m.close) {
    close.push_back(set.candidates[i].title);
  }
  EXPECT_EQ(close, (std::vector<std::string>{
                       "Queen - Bohemian Rhapsody (Live at Rock Montreal 1981)",
                       "Queen - Bohemian Rhapsody (Live Aid 1985)",
                       "Queen - Bohemian Rhapsody (Live at Wembley Stadium "
                       "1986)"}));

  // ... and the answer picks among the three read back.
  const CandidateSet offered = SubsetOf(set, m.close);
  ASSERT_EQ(offered.size(), 3u);
  EXPECT_EQ(offered.candidates[1].position, 2);
  EXPECT_EQ(offered.targets[1].index, 3);
  m = Match(offered, "the live aid one");
  ASSERT_EQ(m.outcome, ChoiceOutcome::kUnique);
  EXPECT_EQ(offered.targets[m.index].index, 3);
  m = Match(offered, "two");
  ASSERT_EQ(m.outcome, ChoiceOutcome::kUnique);
  EXPECT_EQ(offered.targets[m.index].index, 3);
}

TEST(VoiceCandidatesTest, ShownNumbersAreTheItemNumbers) {
  const PageMap map = BuildPageMap(test::YouTubeLikeResults());
  const PageList& results = map.lists[0];
  const CandidateSet set = CandidatesFromList(map, results, true);
  EXPECT_EQ(set.candidates[3].screen_number, results.entries[3].number);
  // "Number N" is the item showing N, not the Nth result.
  const std::string said =
      "number " + std::to_string(results.entries[3].number);
  const ChoiceMatch m = Match(set, said.c_str());
  ASSERT_EQ(m.outcome, ChoiceOutcome::kUnique);
  EXPECT_EQ(m.index, 3u);
}

TEST(VoiceCandidatesTest, ItemsByNumberAndName) {
  const PageMap map = BuildPageMap(test::YouTubeLikeResults());
  const CandidateSet items = CandidatesFromItems(map);
  ASSERT_EQ(items.size(), map.items.size());
  for (size_t i = 0; i < items.size(); ++i) {
    EXPECT_EQ(items.candidates[i].position, map.items[i].number);
    EXPECT_EQ(items.candidates[i].screen_number, map.items[i].number);
    EXPECT_EQ(items.targets[i].handle, map.items[i].handle);
  }
  ChoiceMatch m = Match(items, "number 3", false);
  ASSERT_EQ(m.outcome, ChoiceOutcome::kUnique);
  EXPECT_EQ(items.targets[m.index].kind, ItemKind::kTextField);
  m = Match(items, "the guide button", false);
  ASSERT_EQ(m.outcome, ChoiceOutcome::kUnique);
  EXPECT_EQ(items.candidates[m.index].title, "Guide");
}

TEST(VoiceCandidatesTest, LinksHeadingsAndTabs) {
  const PageMap map = BuildPageMap(test::WikipediaArticle());
  const CandidateSet links = CandidatesFromLinks(map, false);
  ASSERT_FALSE(links.empty());
  for (size_t i = 0; i < links.size(); ++i) {
    EXPECT_EQ(links.candidates[i].position, static_cast<int>(i + 1));
    EXPECT_EQ(links.candidates[i].role, ChoiceRole::kLink);
    EXPECT_EQ(links.targets[i].kind, ItemKind::kLink);
  }
  const CandidateSet headings = CandidatesFromHeadings(map);
  ASSERT_EQ(headings.size(), map.headings.size());
  ASSERT_FALSE(headings.empty());
  EXPECT_EQ(headings.candidates[0].title, map.headings[0].text);
  EXPECT_EQ(headings.targets[0].source, CandidateSource::kHeading);

  const CandidateSet tabs = CandidatesFromTabs(
      {{"Results - VideoSite", "video.test"}, {"Inbox", "mail.example"}});
  ASSERT_EQ(tabs.size(), 2u);
  const ChoiceMatch m = Match(tabs, "the inbox tab", false);
  ASSERT_EQ(m.outcome, ChoiceOutcome::kUnique);
  EXPECT_EQ(tabs.targets[m.index].index, 1);
  EXPECT_EQ(tabs.targets[m.index].source, CandidateSource::kTab);
}

TEST(VoiceCandidatesTest, SubsetSkipsIndicesOutOfRange) {
  const CandidateSet tabs =
      CandidatesFromTabs({{"A", "a.test"}, {"B", "b.test"}});
  const CandidateSet subset = SubsetOf(tabs, {1, 7, 0});
  ASSERT_EQ(subset.size(), 2u);
  EXPECT_EQ(subset.candidates[0].title, "B");
  EXPECT_EQ(subset.candidates[0].position, 1);
  EXPECT_EQ(subset.candidates[1].title, "A");
  EXPECT_EQ(subset.candidates[1].position, 2);
}

TEST(VoiceCandidatesTest, RolesOfEntries) {
  PageList list;
  PageListEntry entry;
  EXPECT_EQ(EntryRole(list, entry), ChoiceRole::kListItem);
  entry.duration_seconds = 60;
  EXPECT_EQ(EntryRole(list, entry), ChoiceRole::kVideo);
  list.kind = ListKind::kFeed;
  EXPECT_EQ(EntryRole(list, entry), ChoiceRole::kArticle);
  EXPECT_EQ(ItemRole(ItemKind::kButton), ChoiceRole::kButton);
  EXPECT_EQ(ItemRole(ItemKind::kTextField), ChoiceRole::kTextField);
  EXPECT_EQ(ItemRole(ItemKind::kSwitch), ChoiceRole::kCheckBox);
  EXPECT_EQ(ItemRole(ItemKind::kClickable), ChoiceRole::kOther);
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
