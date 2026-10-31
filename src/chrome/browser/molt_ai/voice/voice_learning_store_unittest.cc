// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The learning file (voice_learning_store.h): a round trip, and a hostile
// or broken file read as untrusted: refused whole, or entry by entry, and
// capped.

#include "chrome/browser/molt_ai/voice/voice_learning_store.h"

#include <algorithm>
#include <string>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace voice {
namespace {

constexpr int64_t kNow = 1'800'000'000;

TEST(VoiceLearningStoreTest, RoundTrip) {
  LearnedData data;
  ASSERT_TRUE(data.vocabulary.NoteName("Queen", kNow));
  ASSERT_TRUE(data.vocabulary.NoteName("Queen", kNow + 1));
  ASSERT_TRUE(data.vocabulary.NoteCorrection("green", "Queen", kNow + 2));
  data.hints.Worked("www.youtube.com", SitePart::kSearchField, "Search",
                    kNow + 3);
  data.hints.Worked("www.youtube.com", SitePart::kPlayer, "Video player",
                    kNow + 4);
  const std::string json = SerializeLearning(data);
  const LearnedData read = ParseLearning(json, kNow + 10);
  ASSERT_EQ(read.vocabulary.entries().size(), 2u);
  EXPECT_EQ(read.vocabulary.HotwordPhrases(16),
            data.vocabulary.HotwordPhrases(16));
  EXPECT_EQ(read.vocabulary.ApplyCorrections("play green"), "play Queen");
  const SiteHint* hint = read.hints.Find("www.youtube.com");
  ASSERT_TRUE(hint);
  EXPECT_EQ(hint->search_field, "Search");
  EXPECT_EQ(hint->player, "Video player");
  EXPECT_EQ(hint->last_used, kNow + 4);
  EXPECT_TRUE(read.learn);
  EXPECT_EQ(SerializeLearning(read), json);
  // The switch is kept too.
  LearnedData off;
  off.learn = false;
  EXPECT_FALSE(ParseLearning(SerializeLearning(off), kNow).learn);
  EXPECT_TRUE(ParseLearning("{\"version\":1,\"learn\":7}", kNow).learn);
  // Aged as it is read.
  EXPECT_TRUE(
      ParseLearning(json, kNow + kVocabularyLifetimeSeconds + 100).empty());
}

TEST(VoiceLearningStoreTest, ABrokenFileIsNothing) {
  for (const std::string& json :
       {std::string(), std::string("not json"), std::string("[]"),
        std::string("{}"), std::string("{\"version\":2,\"words\":[]}"),
        std::string("{\"version\":\"1\"}"),
        std::string(kMaxLearningFileBytes + 1, ' '),
        std::string("{\"version\":1,\"words\":[{\"phrase\":\"Queen\",") +
            "\"uses\":1,\"last\":1800000000}],}",  // trailing comma
        std::string("{\"version\":1,\"a\":[[[[[[[[[[[]]]]]]]]]]]}")}) {
    EXPECT_TRUE(ParseLearning(json, kNow).empty()) << json.substr(0, 60);
  }
}

TEST(VoiceLearningStoreTest, BadEntriesAreDropped) {
  const std::string json =
      "{\"version\":1,"
      "\"words\":["
      "{\"phrase\":\"Queen\",\"uses\":2,\"last\":1800000000},"
      "{\"phrase\":\"yes\",\"uses\":2,\"last\":1800000000},"
      "{\"phrase\":\"1234\",\"uses\":2,\"last\":1800000000},"
      "{\"phrase\":\"Adele\",\"uses\":-1,\"last\":1800000000},"
      "{\"phrase\":\"Adele\",\"uses\":1,\"last\":-5},"
      "{\"phrase\":\"Adele\",\"uses\":1,\"last\":1.5},"
      "{\"phrase\":\"Adele\",\"uses\":1,\"last\":1e300},"
      "{\"phrase\":\"Adele\",\"uses\":1},"
      "{\"phrase\":7,\"uses\":1,\"last\":1800000000},"
      "\"Adele\","
      "{\"phrase\":\"Queen\",\"uses\":1,\"last\":1800000000}],"
      "\"corrections\":["
      "{\"heard\":\"green\",\"meant\":\"Queen\",\"uses\":1,\"last\":1800000000},"
      "{\"heard\":\"GREEN\",\"meant\":\"Queen\",\"uses\":1,\"last\":1800000000},"
      "{\"heard\":\"okay\",\"meant\":\"Oakley\",\"uses\":1,\"last\":1800000000},"
      "{\"meant\":\"Oakley\",\"uses\":1,\"last\":1800000000}],"
      "\"hints\":["
      "{\"host\":\"www.youtube.com\",\"search\":\"Search\",\"list\":\"\","
      "\"player\":\"\",\"last\":1800000000},"
      "{\"host\":\"Evil Host\",\"search\":\"Search\",\"last\":1800000000},"
      "{\"host\":\"a.example\",\"search\":\"\\u202eSearch\","
      "\"last\":1800000000},"
      "{\"host\":\"b.example\",\"last\":1800000000}]}";
  const LearnedData read = ParseLearning(json, kNow);
  ASSERT_EQ(read.vocabulary.entries().size(), 2u);
  EXPECT_EQ(read.vocabulary.entries()[0].heard, "green");
  EXPECT_EQ(read.vocabulary.entries()[1].phrase, "Queen");
  ASSERT_EQ(read.hints.hints().size(), 1u);
  EXPECT_EQ(read.hints.hints()[0].host, "www.youtube.com");
}

TEST(VoiceLearningStoreTest, Capped) {
  std::string json = "{\"version\":1,\"words\":[";
  for (int i = 0; i < 400; ++i) {
    if (i) {
      json += ',';
    }
    json += "{\"phrase\":\"band " + std::string(1, 'a' + i % 26) + "q" +
            std::string(1, 'a' + i / 26) +
            "\",\"uses\":1,\"last\":1800000000}";
  }
  json += "],\"hints\":[";
  for (int i = 0; i < 300; ++i) {
    if (i) {
      json += ',';
    }
    json += "{\"host\":\"s" + std::to_string(i) +
            ".example\",\"player\":\"Player\",\"last\":1800000000}";
  }
  json += "]}";
  ASSERT_LE(json.size(), kMaxLearningFileBytes);
  const LearnedData read = ParseLearning(json, kNow);
  EXPECT_EQ(read.vocabulary.entries().size(), kMaxVocabularyEntries);
  EXPECT_EQ(read.hints.hints().size(), kMaxHintedSites);
}

// The routines from before profiles that this profile declined
// (profile-routines): kept across a round trip, read as untrusted.
TEST(VoiceLearningStoreTest, DeclinedRoutines) {
  LearnedData data;
  EXPECT_TRUE(AddDeclinedRoutines(data.declined_routines,
                                  {"voice_1700000000_1", "wf_2"}));
  // Repeats and what isn't a file's id are dropped.
  EXPECT_FALSE(AddDeclinedRoutines(data.declined_routines,
                                   {"wf_2", "", "../x", "a/b", "c\\d", "e:f",
                                    "g.molt", "h\ni", std::string(129, 'x')}));
  const LearnedData read = ParseLearning(SerializeLearning(data), kNow);
  EXPECT_EQ(read.declined_routines,
            (std::vector<std::string>{"voice_1700000000_1", "wf_2"}));

  // A hostile file: entries of the wrong type or shape dropped, repeats
  // kept once, the oldest dropped past the cap.
  std::string json =
      "{\"version\":1,\"declined_routines\":[7,null,\"../../x\",\"a\","
      "\"a\",{\"id\":\"b\"}";
  for (size_t i = 0; i < kMaxDeclinedRoutines + 5; ++i) {
    json += ",\"r" + std::to_string(i) + "\"";
  }
  json += "]}";
  const LearnedData hostile = ParseLearning(json, kNow);
  ASSERT_EQ(hostile.declined_routines.size(), kMaxDeclinedRoutines);
  EXPECT_EQ(hostile.declined_routines.back(),
            "r" + std::to_string(kMaxDeclinedRoutines + 4));
  EXPECT_EQ(std::count(hostile.declined_routines.begin(),
                       hostile.declined_routines.end(), "a"),
            0);
  // Not a list: nothing.
  EXPECT_TRUE(ParseLearning("{\"version\":1,\"declined_routines\":\"a\"}", kNow)
                  .declined_routines.empty());
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
