// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_gesture.h"

#include <set>
#include <string>

#include "chrome/browser/molt_ai/voice/phonetic_key.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace voice {
namespace {

constexpr HeardEvidence kClean{/*from_microphone=*/true, /*clean=*/true};
constexpr HeardEvidence kNotClean{/*from_microphone=*/true, /*clean=*/false};
constexpr HeardEvidence kTyped{/*from_microphone=*/false, /*clean=*/false};

GestureBinding Camera(std::string site = "https://example.com camera") {
  return GestureBinding{GestureAction::kAllowPermission, std::move(site)};
}

// A deterministic stand-in for base::RandUint64() (splitmix64).
class Counter {
 public:
  explicit Counter(uint64_t start) : next_(start) {}
  uint64_t operator()() {
    uint64_t z = (next_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }

 private:
  uint64_t next_;
};

// A word of the list other than |word|.
std::string OtherWord(const std::string& word) {
  for (std::string_view w : ConfirmationWords()) {
    if (w != word) {
      return std::string(w);
    }
  }
  return std::string();
}

TEST(VoiceGestureWordsTest, EnoughWordsAllWithAKey) {
  EXPECT_GE(ConfirmationWords().size(), 32u);
  std::set<std::string_view> seen;
  for (std::string_view word : ConfirmationWords()) {
    EXPECT_TRUE(seen.insert(word).second) << word;
    EXPECT_FALSE(PhoneticKey(word).empty()) << word;
    for (char c : word) {
      EXPECT_TRUE(c >= 'a' && c <= 'z') << word;
    }
  }
}

TEST(VoiceGestureWordsTest, NoTwoWordsTheKeyConfuses) {
  const auto words = ConfirmationWords();
  for (size_t i = 0; i < words.size(); ++i) {
    for (size_t j = i + 1; j < words.size(); ++j) {
      EXPECT_FALSE(WordsConfusable(words[i], words[j], /*margin=*/1))
          << words[i] << " / " << words[j] << " (" << PhoneticKey(words[i])
          << " " << PhoneticKey(words[j]) << ")";
    }
  }
}

TEST(VoiceGestureWordsTest, NoWordSoundsLikeAnAnswer) {
  for (std::string_view word : ConfirmationWords()) {
    for (std::string_view answer : AnswerWords()) {
      EXPECT_FALSE(WordsConfusable(word, answer, /*margin=*/0))
          << word << " / " << answer;
    }
  }
}

TEST(VoiceGestureWordsTest, EachWordMatchesItselfOnly) {
  const auto words = ConfirmationWords();
  for (size_t i = 0; i < words.size(); ++i) {
    EXPECT_EQ(MatchConfirmationWord(words[i]), i) << words[i];
  }
}

TEST(VoiceGestureWordsTest, WhatIsSaidAroundTheWord) {
  const std::optional<size_t> harbor = MatchConfirmationWord("harbor");
  ASSERT_TRUE(harbor);
  EXPECT_EQ(MatchConfirmationWord("Harbor."), harbor);
  EXPECT_EQ(MatchConfirmationWord("yes, harbor"), harbor);
  EXPECT_EQ(MatchConfirmationWord("say harbor"), harbor);
  EXPECT_EQ(MatchConfirmationWord("harbor please"), harbor);
  EXPECT_EQ(MatchConfirmationWord("the harbor"), harbor);
  // A recognizer's plural.
  EXPECT_EQ(MatchConfirmationWord("harbors"), harbor);
  // The recognizer's spelling, and a sound-alike.
  EXPECT_EQ(MatchConfirmationWord("harbour"), harbor);
  EXPECT_EQ(MatchConfirmationWord("harber"), harbor);
  // A word split in two.
  const std::optional<size_t> butterfly = MatchConfirmationWord("butterfly");
  ASSERT_TRUE(butterfly);
  EXPECT_EQ(MatchConfirmationWord("butter fly"), butterfly);
}

// A sound-alike stands for a word for sure only when no other word of the
// list is nearly as close: what is close to two is asked again.
TEST(VoiceGestureWordsTest, ASoundAlikeOfTwoWordsIsUnsure) {
  // garden's within the key's bound, and falcon's within one more.
  EXPECT_EQ(HearConfirmationWord("pardon").kind, HeardWord::Kind::kUnsure);
  EXPECT_FALSE(MatchConfirmationWord("pardon"));
  EXPECT_EQ(HearConfirmationWord("say pardon please").kind,
            HeardWord::Kind::kUnsure);
  // Close to rocket alone: rocket, for sure.
  const HeardWord larget = HearConfirmationWord("larget");
  ASSERT_EQ(larget.kind, HeardWord::Kind::kWord);
  EXPECT_EQ(ConfirmationWords()[larget.index], "rocket");
  EXPECT_EQ(HearConfirmationWord("harbour").kind, HeardWord::Kind::kWord);
  EXPECT_EQ(HearConfirmationWord("open youtube").kind, HeardWord::Kind::kNone);
  EXPECT_EQ(HearConfirmationWord("no").kind, HeardWord::Kind::kNone);
}

// Every word of the list, said as it is, as a plural or around "yes" and
// "please", is that word for sure: the margin never makes a guess unsure.
TEST(VoiceGestureWordsTest, EveryWordOfTheListIsSure) {
  const auto words = ConfirmationWords();
  for (size_t i = 0; i < words.size(); ++i) {
    const std::string word(words[i]);
    for (const std::string& said :
         {word, word + "s", "yes " + word + " please", "say " + word}) {
      const HeardWord heard = HearConfirmationWord(said);
      EXPECT_EQ(heard.kind, HeardWord::Kind::kWord) << said;
      EXPECT_EQ(heard.index, i) << said;
    }
  }
}

TEST(VoiceGestureWordsTest, NotAWord) {
  EXPECT_FALSE(MatchConfirmationWord(""));
  EXPECT_FALSE(MatchConfirmationWord("yes"));
  EXPECT_FALSE(MatchConfirmationWord("no"));
  EXPECT_FALSE(MatchConfirmationWord("block"));
  EXPECT_FALSE(MatchConfirmationWord("allow"));
  EXPECT_FALSE(MatchConfirmationWord("four"));
  EXPECT_FALSE(MatchConfirmationWord("open youtube"));
  // Two words of the list: neither.
  EXPECT_FALSE(MatchConfirmationWord("harbor maple"));
  // A word in a sentence is not an answer.
  EXPECT_FALSE(MatchConfirmationWord("the harbor is lovely today"));
  EXPECT_FALSE(MatchConfirmationWord(std::string(5000, 'a')));
}

TEST(VoiceGestureBindingTest, DraftSitesInAnyOrder) {
  EXPECT_EQ(DraftSitesBinding("wf1", {"b.example", "a.example"}),
            DraftSitesBinding("wf1", {"a.example", "b.example", "a.example"}));
  EXPECT_NE(DraftSitesBinding("wf1", {"a.example"}),
            DraftSitesBinding("wf1", {"a.example", "b.example"}));
  EXPECT_NE(DraftSitesBinding("wf1", {"a.example"}),
            DraftSitesBinding("wf2", {"a.example"}));
  EXPECT_EQ(DraftSitesBinding("wf1", {"a.example"}).action,
            GestureAction::kConfirmDraftSites);
}

TEST(GestureBookTest, MintsOnTheWordHeardCleanly) {
  GestureBook book{Counter(1)};
  const std::optional<GesturePrompt> prompt = book.Begin(Camera(), 1000);
  ASSERT_TRUE(prompt);
  EXPECT_GT(prompt->id, 0);
  EXPECT_TRUE(MatchConfirmationWord(prompt->word));
  EXPECT_EQ(prompt->expires_ms, 1000 + kGesturePromptWindowMs);

  VoiceGesture gesture;
  EXPECT_EQ(
      book.Hear(prompt->id, "yes " + prompt->word, kClean, 2000, &gesture),
      HearResult::kMinted);
  EXPECT_EQ(gesture.prompt_id, prompt->id);
  EXPECT_EQ(gesture.binding, Camera());
  EXPECT_EQ(gesture.minted_ms, 2000);
  // The prompt is answered.
  EXPECT_EQ(book.Find(prompt->id, 2000), nullptr);
  EXPECT_EQ(book.Hear(prompt->id, prompt->word, kClean, 2100, &gesture),
            HearResult::kNoPrompt);
}

TEST(GestureBookTest, UsedOnceForItsBindingWithinItsLifetime) {
  GestureBook book{Counter(2)};
  std::optional<GesturePrompt> prompt = book.Begin(Camera(), 0);
  ASSERT_TRUE(prompt);
  VoiceGesture gesture;
  ASSERT_EQ(book.Hear(prompt->id, prompt->word, kClean, 100, &gesture),
            HearResult::kMinted);
  EXPECT_TRUE(book.Consume(gesture, Camera(), 200));
  // Once.
  EXPECT_FALSE(book.Consume(gesture, Camera(), 300));

  // Another binding: refused, and the gesture is gone.
  prompt = book.Begin(Camera(), 1000);
  ASSERT_TRUE(prompt);
  ASSERT_EQ(book.Hear(prompt->id, prompt->word, kClean, 1100, &gesture),
            HearResult::kMinted);
  EXPECT_FALSE(
      book.Consume(gesture, Camera("https://other.example camera"), 1200));
  EXPECT_FALSE(book.Consume(gesture, Camera(), 1300));

  // A caller's copy changed after minting doesn't change what it allows.
  prompt = book.Begin(Camera(), 2000);
  ASSERT_TRUE(prompt);
  ASSERT_EQ(book.Hear(prompt->id, prompt->word, kClean, 2100, &gesture),
            HearResult::kMinted);
  VoiceGesture forged = gesture;
  forged.binding = Camera("https://other.example camera");
  EXPECT_FALSE(
      book.Consume(forged, Camera("https://other.example camera"), 2200));

  // Late.
  prompt = book.Begin(Camera(), 3000);
  ASSERT_TRUE(prompt);
  ASSERT_EQ(book.Hear(prompt->id, prompt->word, kClean, 3100, &gesture),
            HearResult::kMinted);
  EXPECT_FALSE(book.Consume(gesture, Camera(), 3100 + kGestureLifetimeMs + 1));

  // Never minted.
  EXPECT_FALSE(book.Consume(VoiceGesture(), Camera(), 4000));
}

TEST(GestureBookTest, TheWordExpiresWithThePrompt) {
  GestureBook book{Counter(3)};
  const std::optional<GesturePrompt> prompt = book.Begin(Camera(), 0);
  ASSERT_TRUE(prompt);
  VoiceGesture gesture;
  EXPECT_EQ(book.Hear(prompt->id, prompt->word, kClean, kGesturePromptWindowMs,
                      &gesture),
            HearResult::kNoPrompt);
}

TEST(GestureBookTest, OnlyACleanHearingFromTheMicrophone) {
  GestureBook book{Counter(4)};
  const std::optional<GesturePrompt> prompt = book.Begin(Camera(), 0);
  ASSERT_TRUE(prompt);
  VoiceGesture gesture;
  EXPECT_EQ(book.Hear(prompt->id, prompt->word, kTyped, 100, &gesture),
            HearResult::kNotClean);
  EXPECT_EQ(book.Hear(prompt->id, prompt->word, kNotClean, 200, &gesture),
            HearResult::kNotClean);
  // Nor does another source's wrong word refuse the prompt.
  EXPECT_EQ(
      book.Hear(prompt->id, OtherWord(prompt->word), kNotClean, 300, &gesture),
      HearResult::kNotClean);
  // It still waits.
  EXPECT_EQ(book.Hear(prompt->id, prompt->word, kClean, 400, &gesture),
            HearResult::kMinted);
}

TEST(GestureBookTest, OtherSpeechLeavesThePromptWaiting) {
  GestureBook book{Counter(5)};
  const std::optional<GesturePrompt> prompt = book.Begin(Camera(), 0);
  ASSERT_TRUE(prompt);
  VoiceGesture gesture;
  EXPECT_EQ(book.Hear(prompt->id, "what", kClean, 100, &gesture),
            HearResult::kNotAWord);
  EXPECT_EQ(book.Hear(prompt->id, "yes", kClean, 200, &gesture),
            HearResult::kNotAWord);
  EXPECT_NE(book.Find(prompt->id, 300), nullptr);
}

TEST(GestureBookTest, AWrongWordRefusesThePromptAndTwoLockItOut) {
  GestureBook book{Counter(6)};
  std::optional<GesturePrompt> prompt = book.Begin(Camera(), 0);
  ASSERT_TRUE(prompt);
  VoiceGesture gesture;
  EXPECT_EQ(
      book.Hear(prompt->id, OtherWord(prompt->word), kClean, 100, &gesture),
      HearResult::kWrongWord);
  // One try: the right word no longer answers it.
  EXPECT_EQ(book.Hear(prompt->id, prompt->word, kClean, 200, &gesture),
            HearResult::kNoPrompt);
  EXPECT_FALSE(book.LockedOut(200));

  prompt = book.Begin(Camera(), 1000);
  ASSERT_TRUE(prompt);
  EXPECT_EQ(
      book.Hear(prompt->id, OtherWord(prompt->word), kClean, 1100, &gesture),
      HearResult::kWrongWord);
  EXPECT_TRUE(book.LockedOut(1100));
  EXPECT_FALSE(book.Begin(Camera(), 1200));
  EXPECT_FALSE(book.Begin(Camera(), 1100 + kLockoutMs - 1));
  EXPECT_TRUE(book.Begin(Camera(), 1100 + kLockoutMs));
}

// What is like a word of the list but not for sure which neither answers
// nor refuses the prompt, nor counts toward the lockout: the user says it
// again within the prompt's window.
TEST(GestureBookTest, AnUnsureWordIsAskedAgain) {
  GestureBook book{Counter(11)};
  std::optional<GesturePrompt> prompt = book.Begin(Camera(), 0);
  ASSERT_TRUE(prompt);
  VoiceGesture gesture;
  for (int i = 0; i < 2 * kMaxWrongWords + 1; ++i) {
    EXPECT_EQ(book.Hear(prompt->id, "pardon", kClean, 100 + i, &gesture),
              HearResult::kUnsure);
  }
  EXPECT_FALSE(book.LockedOut(200));
  EXPECT_NE(book.Find(prompt->id, 200), nullptr);
  EXPECT_EQ(book.Hear(prompt->id, prompt->word, kClean, 300, &gesture),
            HearResult::kMinted);
}

// The guesser's odds: whatever the prompt's word, every other word of the
// list refuses the prompt at the first try and counts toward the lockout;
// an unsure hearing is the same for every prompt word, so it tells nothing
// about it.
TEST(GestureBookTest, EveryOtherWordIsOneTryWhateverThePromptsWord) {
  const auto words = ConfirmationWords();
  for (uint64_t seed = 0; seed < 40; ++seed) {
    GestureBook book{Counter(seed)};
    std::optional<GesturePrompt> prompt = book.Begin(Camera(), 0);
    ASSERT_TRUE(prompt);
    for (std::string_view other : words) {
      if (other == prompt->word) {
        continue;
      }
      GestureBook fresh{Counter(seed)};
      std::optional<GesturePrompt> same = fresh.Begin(Camera(), 0);
      ASSERT_TRUE(same);
      ASSERT_EQ(same->word, prompt->word);
      VoiceGesture gesture;
      EXPECT_EQ(fresh.Hear(same->id, std::string(other), kClean, 100, &gesture),
                HearResult::kWrongWord)
          << other << " for " << prompt->word;
      EXPECT_EQ(fresh.Find(same->id, 100), nullptr);
      EXPECT_EQ(fresh.Hear(same->id, "pardon", kClean, 200, &gesture),
                HearResult::kNoPrompt);
    }
    VoiceGesture gesture;
    EXPECT_EQ(book.Hear(prompt->id, "pardon", kClean, 100, &gesture),
              HearResult::kUnsure);
  }
}

TEST(GestureBookTest, WrongWordsFarApartDontLockOut) {
  GestureBook book{Counter(7)};
  int64_t now = 0;
  for (int i = 0; i < 4; ++i) {
    std::optional<GesturePrompt> prompt = book.Begin(Camera(), now);
    ASSERT_TRUE(prompt);
    VoiceGesture gesture;
    EXPECT_EQ(
        book.Hear(prompt->id, OtherWord(prompt->word), kClean, now, &gesture),
        HearResult::kWrongWord);
    now += kWrongWordWindowMs;
  }
  EXPECT_FALSE(book.LockedOut(now));
}

TEST(GestureBookTest, ALockoutDuringAPromptClosesIt) {
  GestureBook book{Counter(8)};
  std::optional<GesturePrompt> waiting = book.Begin(Camera("a"), 0);
  ASSERT_TRUE(waiting);
  VoiceGesture gesture;
  for (int i = 0; i < kMaxWrongWords; ++i) {
    std::optional<GesturePrompt> prompt = book.Begin(Camera("b"), 10 + i);
    ASSERT_TRUE(prompt);
    ASSERT_EQ(book.Hear(prompt->id, OtherWord(prompt->word), kClean, 10 + i,
                        &gesture),
              HearResult::kWrongWord);
  }
  EXPECT_EQ(book.Hear(waiting->id, waiting->word, kClean, 100, &gesture),
            HearResult::kNoPrompt);
}

TEST(GestureBookTest, FreshWordsNeverTheRecentOnes) {
  GestureBook book{Counter(9)};
  std::vector<std::string> words;
  for (int i = 0; i < 200; ++i) {
    std::optional<GesturePrompt> prompt =
        book.Begin(Camera("s" + std::to_string(i)), i);
    ASSERT_TRUE(prompt);
    for (size_t back = 1; back <= kRecentWordsAvoided && back <= words.size();
         ++back) {
      EXPECT_NE(prompt->word, words[words.size() - back]);
    }
    words.push_back(prompt->word);
    book.Cancel(prompt->id);
  }
  // Every word comes up.
  EXPECT_EQ(std::set<std::string>(words.begin(), words.end()).size(),
            ConfirmationWords().size());
}

TEST(GestureBookTest, ABrokenRandomSourceStillGivesAWord) {
  GestureBook book{[] { return ~uint64_t{0}; }};
  const std::optional<GesturePrompt> prompt = book.Begin(Camera(), 0);
  ASSERT_TRUE(prompt);
  EXPECT_TRUE(MatchConfirmationWord(prompt->word));
}

TEST(GestureBookTest, ANewPromptForTheSameBindingReplacesTheOld) {
  GestureBook book{Counter(10)};
  const std::optional<GesturePrompt> first = book.Begin(Camera(), 0);
  const std::optional<GesturePrompt> second = book.Begin(Camera(), 10);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_NE(first->id, second->id);
  EXPECT_NE(first->word, second->word);
  VoiceGesture gesture;
  EXPECT_EQ(book.Hear(first->id, first->word, kClean, 20, &gesture),
            HearResult::kNoPrompt);
  EXPECT_EQ(book.Hear(second->id, second->word, kClean, 30, &gesture),
            HearResult::kMinted);
}

TEST(GestureBookTest, AGestureMintedForOnePromptIsNotAnother) {
  GestureBook book{Counter(11)};
  const std::optional<GesturePrompt> camera = book.Begin(Camera(), 0);
  const std::optional<GesturePrompt> download =
      book.Begin(GestureBinding{GestureAction::kKeepDangerousDownload, "7"}, 0);
  ASSERT_TRUE(camera);
  ASSERT_TRUE(download);
  VoiceGesture gesture;
  ASSERT_EQ(book.Hear(camera->id, camera->word, kClean, 10, &gesture),
            HearResult::kMinted);
  EXPECT_FALSE(book.Consume(
      gesture, GestureBinding{GestureAction::kKeepDangerousDownload, "7"}, 20));
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
