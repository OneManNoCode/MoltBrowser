// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/phonetic_key.h"

#include <string>
#include <string_view>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace voice {
namespace {

TEST(PhoneticKeyTest, Keys) {
  struct Case {
    std::string_view word;
    std::string_view key;
  } cases[] = {
      {"rhapsody", "RaPSaTa"},
      {"rap", "RaP"},
      {"city", "SaTa"},
      {"queen", "KWaN"},
      {"quinn", "KWaN"},
      {"molt", "MaLT"},
      {"mold", "MaLT"},
      {"live", "LaP"},
      {"life", "LaP"},
      {"the", "Ta"},
      {"me", "Ma"},
      {"bohemian", "PaMaN"},
      {"station", "STaXaN"},
      {"version", "PaRXaN"},
      {"sign", "SaN"},
      {"song", "SaN"},
      {"night", "NaT"},
      {"knight", "NaT"},
      {"ghost", "KaST"},
      {"phone", "PaN"},
      {"school", "SKaL"},
      {"church", "XaRX"},
      {"christmas", "KRaSTMaS"},
      {"edge", "aX"},
      {"page", "PaX"},
      {"climb", "KLaM"},
      {"wrap", "RaP"},
      {"who", "Wa"},
      {"yes", "YaS"},
      {"xylophone", "SaLaPaN"},
      {"box", "PaKS"},
      {"watch", "WaX"},
      {"cow", "Ka"},
      {"oh", "a"},
      {"psalm", "SaLM"},
  };
  for (const Case& c : cases) {
    EXPECT_EQ(PhoneticKey(c.word), c.key) << c.word;
  }
  // Only lower-case ASCII letters have a key.
  for (std::string_view none :
       {"", "4th", "mp3", "Queen", "caf\xC3\xA9", "\xCE\xB1\xCE\xB2", "a-b"}) {
    EXPECT_EQ(PhoneticKey(none), "") << none;
  }
  EXPECT_EQ(PhoneticKey(std::string(kMaxPhoneticWordBytes + 1, 'a')), "");
}

TEST(PhoneticKeyTest, SplitAndMergedWordsJoin) {
  EXPECT_EQ(JoinPhoneticKeys(PhoneticKey("rap"), PhoneticKey("city")),
            PhoneticKey("rhapsody"));
  EXPECT_EQ(JoinPhoneticKeys(PhoneticKey("mon"), PhoneticKey("trail")),
            PhoneticKey("montreal"));
  EXPECT_EQ(JoinPhoneticKeys("aP", "Pa"), "aPa");
  EXPECT_EQ(JoinPhoneticKeys("", "Pa"), "Pa");
}

TEST(PhoneticKeyTest, Distance) {
  EXPECT_EQ(PhoneticDistance("KWaN", "KWaN", 0), 0);
  EXPECT_EQ(PhoneticDistance("MaN", "MaM", 5), 1);  // close sounds
  EXPECT_EQ(PhoneticDistance("LaP", "RaP", 5), 1);
  EXPECT_EQ(PhoneticDistance("KaRaN", "SaRaN", 5), 1);  // the two c's
  EXPECT_EQ(PhoneticDistance("KaT", "LaT", 5), 3);      // far sounds
  EXPECT_EQ(PhoneticDistance("PaT", "PaTa", 5), 1);     // a vowel
  EXPECT_EQ(PhoneticDistance("PaT", "PaTS", 5), 2);     // a consonant
  EXPECT_EQ(PhoneticDistance("Wa", "a", 5), 1);         // a semivowel
  // Past the bound: bound + 1, whatever the real distance.
  EXPECT_EQ(PhoneticDistance("KaT", "LaT", 2), 3);
  EXPECT_EQ(PhoneticDistance("PaTaKaS", "MaNaLaR", 3), 4);
  EXPECT_EQ(PhoneticDistance("a", "aaaaaaaaaaaa", 4), 5);
  EXPECT_EQ(PhoneticDistance("Pa", "Pa", -3), 0);
  // Symmetric.
  EXPECT_EQ(PhoneticDistance("RaPSaTa", "RaPSTa", 5),
            PhoneticDistance("RaPSTa", "RaPSaTa", 5));
  // Long keys are not compared.
  const std::string long_key(400, 'a');
  EXPECT_EQ(PhoneticDistance(long_key, long_key + "P", 5), 6);
}

TEST(PhoneticKeyTest, BoundsAndSimilarity) {
  EXPECT_EQ(PhoneticBound(1), 0);
  EXPECT_EQ(PhoneticBound(2), 0);
  EXPECT_EQ(PhoneticBound(3), 1);
  EXPECT_EQ(PhoneticBound(7), 3);
  EXPECT_EQ(PhoneticBound(30), 5);
  EXPECT_DOUBLE_EQ(PhoneticSimilarity("RaPSaTa", "RaPSaTa"), 0.9);
  EXPECT_DOUBLE_EQ(PhoneticSimilarity("KWaN", "KWaN"), 0.8);
  EXPECT_DOUBLE_EQ(PhoneticSimilarity("LaP", "LaP"), 0.7);
  EXPECT_NEAR(PhoneticSimilarity("RaPSaTa", "RaPSaTaa"), 0.9 * (1 - 1 / 8.0),
              1e-9);
  EXPECT_EQ(PhoneticSimilarity("Pa", "Ta"), 0);  // short keys must be equal
  EXPECT_EQ(PhoneticSimilarity("KaRaK", "LaRaK"), 0);  // a far sound
  EXPECT_EQ(PhoneticSimilarity("", "Pa"), 0);
  EXPECT_TRUE(SameOrCloseSounds('M', 'N'));
  EXPECT_TRUE(SameOrCloseSounds('P', 'P'));
  EXPECT_FALSE(SameOrCloseSounds('P', 'K'));
}

// Recognition errors seen in practice match; different words don't.
TEST(PhoneticKeyTest, RecognitionErrors) {
  const auto alike = [](std::string_view heard, std::string_view shown) {
    return PhoneticSimilarity(PhoneticKey(heard), PhoneticKey(shown)) > 0;
  };
  EXPECT_TRUE(alike("wembly", "wembley"));
  EXPECT_TRUE(alike("trailor", "trailer"));
  EXPECT_TRUE(alike("anchor", "anker"));
  EXPECT_TRUE(alike("cappella", "acapella"));
  EXPECT_TRUE(alike("kieran", "ciaran"));
  EXPECT_TRUE(alike("remaster", "remastered"));
  EXPECT_TRUE(alike("wanes", "waynes"));
  EXPECT_FALSE(alike("karaoke", "lyric"));
  EXPECT_FALSE(alike("queen", "trailer"));
  EXPECT_FALSE(alike("guitar", "piano"));
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
