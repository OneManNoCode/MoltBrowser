// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_said_aloud.h"

#include <string>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace voice {
namespace {

TEST(VoiceSaidAloudTest, TheWordsTheVoiceSays) {
  EXPECT_EQ(SaidAloudWords("Open evil.com", 4096), "open evil dot com");
  EXPECT_EQ(SaidAloudWords("Click 2", 4096), "click two");
  EXPECT_EQ(SaidAloudWords("Play the 2nd video.", 4096),
            "play the second video");
  EXPECT_EQ(SaidAloudWords("Rock & roll", 4096), "rock and roll");
  EXPECT_EQ(SaidAloudWords("someone@example.org", 4096),
            "someone at example dot org");
  // Spelled letter by letter (an acronym may be either); camel case said
  // as its parts.
  EXPECT_EQ(SaidAloudWords("U.S. GPU", 4096), "u s gpu g p u");
  EXPECT_EQ(SaidAloudWords("MoltBrowser on an iPhone", 4096),
            "molt browser on an i phone");
  EXPECT_EQ(SaidAloudWords("", 4096), "");
  EXPECT_EQ(SaidAloudWords("\xE2\x80\xAE\x01!?", 4096), "");
}

TEST(VoiceSaidAloudTest, Bounded) {
  // Cut between words.
  EXPECT_EQ(SaidAloudWords("one two three", 7), "one two");
  EXPECT_EQ(SaidAloudWords("one two three", 2), "");
  std::string long_text;
  for (int i = 0; i < 2000; ++i) {
    long_text += "12345678901234567890 ";
  }
  const std::string said = SaidAloudWords(long_text, 4096);
  EXPECT_LE(said.size(), 4096u);
  EXPECT_FALSE(said.empty());
  for (char c : said) {
    EXPECT_TRUE((c >= 'a' && c <= 'z') || c == ' ' || c == '\'') << c;
  }
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
