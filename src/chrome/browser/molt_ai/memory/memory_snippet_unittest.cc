// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/memory/memory_snippet.h"

#include <string>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace memory {
namespace {

constexpr char kEllipsis[] = "\xE2\x80\xA6";

// Strict enough for these tests: no character cut in half.
bool IsWholeUtf8(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3
                                              : (c >> 3) == 0x1E ? 4 : 0;
    if (n == 0 || i + n > s.size()) return false;
    for (size_t k = 1; k < n; ++k) {
      if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
    }
    i += n;
  }
  return true;
}

std::string Filler(size_t n) {
  std::string s;
  while (s.size() < n) s += "lorem ipsum dolor sit amet ";
  return s.substr(0, n);
}

TEST(BestSnippetTest, ShortChunkIsWhole) {
  EXPECT_EQ(BestSnippet("a short page", "page", 240), "a short page");
  EXPECT_EQ(BestSnippet("anything", "x", 0), "");
}

TEST(BestSnippetTest, NoMatchKeepsTheStart) {
  const std::string chunk = Filler(1000);
  const std::string s = BestSnippet(chunk, "kubernetes operator", 120);
  EXPECT_EQ(s.rfind(kEllipsis), s.size() - 3);
  EXPECT_EQ(s.compare(0, 11, "lorem ipsum"), 0);
  EXPECT_LE(s.size(), 120u + 3);
}

TEST(BestSnippetTest, FindsTheMatchingPassage) {
  const std::string chunk = Filler(500) + " The Miso glazed salmon RECIPE needs " +
                            "white miso. " + Filler(500);
  const std::string s = BestSnippet(chunk, "miso recipe", 120);
  EXPECT_NE(s.find("Miso glazed salmon RECIPE"), std::string::npos) << s;
  EXPECT_EQ(s.compare(0, 3, kEllipsis), 0);
  EXPECT_LE(s.size(), 120u + 6);
}

TEST(BestSnippetTest, PrefersTheWindowWithMoreQueryWords) {
  const std::string chunk = "flight " + Filler(400) + " cheap flight to bali " +
                            Filler(400);
  const std::string s = BestSnippet(chunk, "flight bali deal", 80);
  EXPECT_NE(s.find("flight to bali"), std::string::npos) << s;
}

TEST(BestSnippetTest, PrefixMatchOnlyAtWordStarts) {
  const std::string chunk =
      Filler(300) + " xrecipes " + Filler(300) + " recipes here " + Filler(300);
  const std::string s = BestSnippet(chunk, "recipe", 60);
  EXPECT_NE(s.find("recipes here"), std::string::npos) << s;
}

TEST(BestSnippetTest, NeverSplitsCharacters) {
  std::string chunk;
  for (int i = 0; i < 200; ++i) chunk += "\xC3\xA9t\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC ";
  for (size_t max = 1; max < 60; ++max) {
    for (const char* q : {"", "t", "\xE6\x97\xA5", "zzz"}) {
      const std::string s = BestSnippet(chunk, q, max);
      EXPECT_TRUE(IsWholeUtf8(s)) << max << " " << q;
      EXPECT_LE(s.size(), max + 6);
    }
  }
}

TEST(BestSnippetTest, HostileQueries) {
  const std::string chunk = Filler(2000);
  std::string many;
  for (int i = 0; i < 100; ++i) many += "lorem ipsum ";
  EXPECT_TRUE(IsWholeUtf8(BestSnippet(chunk, many, 200)));
  EXPECT_TRUE(IsWholeUtf8(BestSnippet(chunk, std::string(5000, 'a'), 200)));
  EXPECT_TRUE(
      IsWholeUtf8(BestSnippet(chunk, std::string("\xff\xfe\0", 3), 200)));
}

}  // namespace
}  // namespace memory
}  // namespace molt_ai
