// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The personal vocabulary (voice_vocabulary.h): names kept, capped and aged
// by last use; corrections lined up with what they correct and applied to
// later utterances; nothing that answers a question or looks secret ever
// learned.

#include "chrome/browser/molt_ai/voice/voice_vocabulary.h"

#include <string>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace voice {
namespace {

constexpr int64_t kNow = 1'800'000'000;
constexpr int64_t kDay = 24 * 3600;

TEST(VoiceVocabularyTest, NamesBecomeHotwordsWhenSaidOften) {
  PersonalVocabulary v;
  EXPECT_TRUE(v.NoteName("Queen", kNow));
  EXPECT_TRUE(v.NoteName("Bohemian Rhapsody", kNow + 1));
  // Said once: kept, not yet a hotword.
  EXPECT_EQ(v.entries().size(), 2u);
  EXPECT_TRUE(v.HotwordPhrases(16).empty());
  // Said again (in any case): counted, the latest spelling kept.
  EXPECT_TRUE(v.NoteName("queen", kNow + 2));
  EXPECT_EQ(v.entries().size(), 2u);
  EXPECT_EQ(v.HotwordPhrases(16), std::vector<std::string>{"queen"});
  EXPECT_TRUE(v.NoteName("Bohemian  Rhapsody", kNow + 3));
  // Most recently used first; a correction before any name.
  EXPECT_EQ(v.HotwordPhrases(16),
            (std::vector<std::string>{"Bohemian Rhapsody", "queen"}));
  EXPECT_TRUE(v.NoteCorrection("green", "Queen Official", kNow + 4));
  EXPECT_EQ(v.HotwordPhrases(16),
            (std::vector<std::string>{"Queen Official", "Bohemian Rhapsody",
                                      "queen"}));
  EXPECT_EQ(v.HotwordPhrases(1), std::vector<std::string>{"Queen Official"});
}

TEST(VoiceVocabularyTest, CappedAndAgedByLastUse) {
  PersonalVocabulary v;
  for (int i = 0; i < 250; ++i) {
    const std::string name = "band " + std::string(1, 'a' + i % 26) + "q" +
                             std::string(1, 'a' + i / 26);
    ASSERT_TRUE(v.NoteName(name, kNow + i)) << name;
  }
  // The least recently used went first.
  ASSERT_EQ(v.entries().size(), kMaxVocabularyEntries);
  EXPECT_EQ(v.entries().front().last_used, kNow + 50);
  // Using an old one keeps it: the next new one pushes out the oldest.
  ASSERT_TRUE(v.NoteName(v.entries().front().phrase, kNow + 300));
  ASSERT_TRUE(v.NoteName("someone new", kNow + 301));
  EXPECT_EQ(v.entries().size(), kMaxVocabularyEntries);
  bool kept = false;
  for (const VocabularyEntry& e : v.entries()) {
    kept = kept || e.last_used == kNow + 300;
    EXPECT_NE(e.last_used, kNow + 51);
  }
  EXPECT_TRUE(kept);
  // Unused for 180 days: gone.
  v.Age(kNow + 249 + kVocabularyLifetimeSeconds + 1);
  EXPECT_EQ(v.entries().size(), 2u);
  v.Age(kNow + 301 + kVocabularyLifetimeSeconds + 1);
  EXPECT_TRUE(v.empty());
  // A date past "now" (the clock moved back) is dropped too.
  ASSERT_TRUE(v.NoteName("Queen", kNow + 10 * kDay));
  v.Age(kNow);
  EXPECT_TRUE(v.empty());
}

TEST(VoiceVocabularyTest, NeverLearnsAnswersOrSecrets) {
  PersonalVocabulary v;
  for (const char* phrase : {
           "yes", "Yes", "okay", "OK", "sure", "no", "nope", "of course",
           "yes please", "Right Said Fred", "do it again", "go ahead now",
           // Secret-looking.
           "1234", "4111 1111", "abc123", "my password", "PIN", "otp code",
           // Not a phrase.
           "", " ", "!!!", "one two three four five six seven",
           "a very long name that goes on and on past sixty four bytes long",
           "bad \xFF utf8", "12"}) {
    EXPECT_FALSE(LearnablePhrase(phrase)) << phrase;
    EXPECT_FALSE(v.NoteName(phrase, kNow)) << phrase;
  }
  EXPECT_TRUE(v.empty());
  // Ordinary names, with a short number in them.
  for (const char* phrase :
       {"Queen", "AC/DC", "Maroon 5", "Beyoncé", "the New York Times",
        "Sigur Rós", "Blink 182"}) {
    EXPECT_TRUE(LearnablePhrase(phrase)) << phrase;
  }
  // Hidden characters are cleaned away.
  EXPECT_EQ(LearnedPhrase("Que\xE2\x80\x8B" "en"), "Queen");
  EXPECT_EQ(LearnedPhrase("  Bohemian\n Rhapsody "), "Bohemian Rhapsody");
}

TEST(VoiceVocabularyTest, CorrectionsApplyToWholeWords) {
  PersonalVocabulary v;
  ASSERT_TRUE(v.NoteCorrection("green", "Queen", kNow));
  std::vector<std::string> used;
  EXPECT_EQ(v.ApplyCorrections("play the live one by Green", &used),
            "play the live one by Queen");
  EXPECT_EQ(used, std::vector<std::string>{"green"});
  // Whole words only, and nothing when nothing applies.
  EXPECT_EQ(v.ApplyCorrections("search for greenery"), std::nullopt);
  EXPECT_EQ(v.ApplyCorrections("open YouTube"), std::nullopt);
  // The longest first; never applied to its own output.
  // The same words in another case aren't a correction.
  EXPECT_FALSE(v.NoteCorrection("green day", "Green Day", kNow));
  ASSERT_TRUE(v.NoteCorrection("green day", "Greenday", kNow));
  ASSERT_TRUE(v.NoteCorrection("queen", "Queen Latifah", kNow));
  EXPECT_EQ(v.ApplyCorrections("search for green day and green"),
            "search for Greenday and Queen");
  // Punctuation and the rest of the utterance stay as they were.
  EXPECT_EQ(v.ApplyCorrections("Hey Molt, play GREEN, please"),
            "Hey Molt, play Queen, please");
  // A correction is replaced, not duplicated.
  ASSERT_TRUE(v.NoteCorrection("green", "Queen", kNow + 5));
  size_t corrections = 0;
  for (const VocabularyEntry& e : v.entries()) {
    corrections += e.correction() && e.heard == "green";
  }
  EXPECT_EQ(corrections, 1u);
  // Too long or not UTF-8: left alone.
  EXPECT_EQ(v.ApplyCorrections(std::string(2000, 'a')), std::nullopt);
  EXPECT_EQ(v.ApplyCorrections("green \xFF"), std::nullopt);
}

TEST(VoiceVocabularyTest, CorrectionsNeverTouchAnswers) {
  PersonalVocabulary v;
  EXPECT_FALSE(v.NoteCorrection("yeah", "Yale", kNow));
  EXPECT_FALSE(v.NoteCorrection("Yale", "yes", kNow));
  EXPECT_FALSE(v.NoteCorrection("know", "no", kNow));
  EXPECT_FALSE(v.NoteCorrection("Queen", "queen", kNow));
  EXPECT_FALSE(v.NoteCorrection("", "Queen", kNow));
  EXPECT_FALSE(v.NoteCorrection("green", "1234", kNow));
  EXPECT_TRUE(v.empty());
}

TEST(VoiceVocabularyTest, ForgetTakesEitherSide) {
  PersonalVocabulary v;
  ASSERT_TRUE(v.NoteName("Queen", kNow));
  ASSERT_TRUE(v.NoteCorrection("green", "Queen", kNow));
  ASSERT_TRUE(v.NoteCorrection("clean", "Queen Official", kNow));
  ASSERT_TRUE(v.NoteName("Adele", kNow));
  EXPECT_EQ(v.Forget("queen"), 2u);
  EXPECT_EQ(v.Forget("Clean"), 1u);
  EXPECT_EQ(v.Forget("nobody"), 0u);
  EXPECT_EQ(v.Forget(""), 0u);
  ASSERT_EQ(v.entries().size(), 1u);
  EXPECT_EQ(v.entries()[0].phrase, "Adele");
  v.Clear();
  EXPECT_TRUE(v.empty());
}

TEST(VoiceVocabularyTest, EveryReadingOfThePageCounts) {
  using K = VoiceCommandKind;
  // The page's text read out, however the reading began or went on: what
  // is heard then may be the page, so nothing of it is learned.
  for (K kind :
       {K::kReadPage, K::kReadFromHere, K::kReadSelection, K::kReadSection,
        K::kNextParagraph, K::kPreviousParagraph, K::kResume, K::kNextHeading,
        K::kPreviousHeading, K::kReadResults, K::kReadLinks, K::kReadHeadings,
        K::kReadTitle, K::kDescribeScreen, K::kNextItem, K::kPreviousItem,
        K::kRepeat}) {
    EXPECT_TRUE(ReadsPageText(kind)) << VoiceCommandKindName(kind);
  }
  for (K kind : {K::kSearch, K::kOpenSite, K::kChoose, K::kType, K::kYes,
                 K::kCorrection, K::kRunRoutine}) {
    EXPECT_FALSE(ReadsPageText(kind)) << VoiceCommandKindName(kind);
  }
}

TEST(VoiceVocabularyTest, AlignsACorrectionWithWhatItCorrects) {
  // The words meant: the span that sounds most like them.
  std::optional<AlignedCorrection> c =
      AlignCorrection("play the live one by Green", "Queen");
  ASSERT_TRUE(c);
  EXPECT_EQ(c->heard, "green");
  EXPECT_EQ(c->meant, "Queen");
  EXPECT_EQ(c->corrected, "play the live one by Queen");
  c = AlignCorrection("search for cats on YouTube", "hats");
  ASSERT_TRUE(c);
  EXPECT_EQ(c->heard, "cats");
  EXPECT_EQ(c->corrected, "search for hats on YouTube");
  // A whole command: what differs between the same ends.
  c = AlignCorrection("open you tube", "open YouTube");
  ASSERT_TRUE(c);
  EXPECT_EQ(c->heard, "you tube");
  EXPECT_EQ(c->meant, "YouTube");
  EXPECT_EQ(c->corrected, "open YouTube");
  // Another request, not a mishearing: done, nothing learned.
  c = AlignCorrection("open YouTube", "open the BBC");
  ASSERT_TRUE(c);
  EXPECT_EQ(c->corrected, "open the BBC");
  EXPECT_TRUE(c->heard.empty());
  EXPECT_TRUE(c->meant.empty());
  // Nothing sounds like it: the correction stands on its own.
  EXPECT_EQ(AlignCorrection("search for cats", "dogs"), std::nullopt);
  EXPECT_EQ(AlignCorrection("search for news", "music"), std::nullopt);
  // The same, or only words added: nothing to learn.
  EXPECT_EQ(AlignCorrection("play Queen", "Queen"), std::nullopt);
  EXPECT_EQ(AlignCorrection("play Queen", "play Queen"), std::nullopt);
  EXPECT_EQ(AlignCorrection("play", "play Queen now"), std::nullopt);
  EXPECT_EQ(AlignCorrection("", "Queen"), std::nullopt);
  EXPECT_EQ(AlignCorrection("play Green", ""), std::nullopt);
}

TEST(VoiceVocabularyTest, RestoresOnlyValidEntries) {
  PersonalVocabulary v;
  VocabularyEntry name;
  name.phrase = "Queen";
  name.uses = 3;
  name.last_used = kNow;
  EXPECT_TRUE(v.Restore(name));
  EXPECT_FALSE(v.Restore(name));  // twice
  VocabularyEntry bad = name;
  bad.phrase = "yes";
  EXPECT_FALSE(v.Restore(bad));
  bad.phrase = " Adele ";  // not as kept
  EXPECT_FALSE(v.Restore(bad));
  bad.phrase = "Adele";
  bad.uses = 0;
  EXPECT_FALSE(v.Restore(bad));
  bad.uses = 1;
  bad.last_used = -1;
  EXPECT_FALSE(v.Restore(bad));
  VocabularyEntry correction;
  correction.phrase = "Queen";
  correction.heard = "Green";  // not folded
  correction.uses = 1;
  correction.last_used = kNow;
  EXPECT_FALSE(v.Restore(correction));
  correction.heard = "queen";  // the same as what was meant
  EXPECT_FALSE(v.Restore(correction));
  correction.heard = "okay";
  EXPECT_FALSE(v.Restore(correction));
  correction.heard = "green";
  EXPECT_TRUE(v.Restore(correction));
  EXPECT_EQ(v.entries().size(), 2u);
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
