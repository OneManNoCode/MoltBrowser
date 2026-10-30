// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_helper/voice_lts.h"

#include <string>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_voice {
namespace {

TEST(VoiceLtsTest, CommonPatterns) {
  // Names the lexicon lacks (VOICE_BENCH.md §3.9).
  EXPECT_EQ("ˈiːlɪʃ", LetterToSound("eilish"));
  // "ei" as in "receive" (the rules' reading, not the German one).
  EXPECT_EQ("sˈɛnhiːzɚ", LetterToSound("Sennheiser"));
  EXPECT_EQ("wˈiːknd", LetterToSound("weeknd"));
  // Digraphs, silent e, magic e.
  EXPECT_EQ("fˈoʊn", LetterToSound("phone"));
  EXPECT_EQ("nˈaɪt", LetterToSound("knight"));
  EXPECT_EQ("tʃˈɪp", LetterToSound("chip"));
  EXPECT_EQ("nˈeɪʃən", LetterToSound("nation"));
  EXPECT_EQ("bˈɪɡ", LetterToSound("bigg"));
}

TEST(VoiceLtsTest, Edges) {
  EXPECT_EQ("", LetterToSound(""));
  EXPECT_EQ("", LetterToSound("123 '-"));
  // No vowel: no stress mark.
  EXPECT_EQ("pθ", LetterToSound("pth"));
  // Long and repetitive input stays bounded (one sound per letter at
  // most a few bytes).
  const std::string long_word(40, 'a');
  EXPECT_LT(LetterToSound(long_word).size(), 40u * 4);
}

TEST(VoiceLtsTest, VowelSounds) {
  EXPECT_TRUE(StartsWithVowelSound("ˈæp"));
  EXPECT_TRUE(StartsWithVowelSound("ɪt"));
  EXPECT_FALSE(StartsWithVowelSound("bˈiː"));
  EXPECT_FALSE(StartsWithVowelSound(""));
}

}  // namespace
}  // namespace molt_voice
