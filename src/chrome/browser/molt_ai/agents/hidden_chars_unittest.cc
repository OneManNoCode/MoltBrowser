// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/agents/hidden_chars.h"

#include <string>
#include <string_view>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace hidden_chars {
namespace {

constexpr char kReplacement[] = "\xEF\xBF\xBD";

struct Case {
  const char* what;
  std::string_view bytes;
  size_t hidden;  // HiddenCharAt(): its length, or 0 when shown as it is
};

// Every range at both ends, and the characters just outside it.
constexpr Case kCases[] = {
    // C1 controls and the soft hyphen.
    {"U+007F", "\x7F", 0},
    {"U+0080", "\xC2\x80", 2},
    {"U+0085", "\xC2\x85", 2},
    {"U+009F", "\xC2\x9F", 2},
    {"U+00A0", "\xC2\xA0", 0},
    {"U+00AC", "\xC2\xAC", 0},
    {"U+00AD", "\xC2\xAD", 2},
    {"U+00AE", "\xC2\xAE", 0},
    // Arabic letter mark.
    {"U+061B", "\xD8\x9B", 0},
    {"U+061C", "\xD8\x9C", 2},
    {"U+061D", "\xD8\x9D", 0},
    // Hangul fillers.
    {"U+115E", "\xE1\x85\x9E", 0},
    {"U+115F", "\xE1\x85\x9F", 3},
    {"U+1160", "\xE1\x85\xA0", 3},
    {"U+1161", "\xE1\x85\xA1", 0},
    {"U+3163", "\xE3\x85\xA3", 0},
    {"U+3164", "\xE3\x85\xA4", 3},
    {"U+3165", "\xE3\x85\xA5", 0},
    {"U+FF9F", "\xEF\xBE\x9F", 0},
    {"U+FFA0", "\xEF\xBE\xA0", 3},
    {"U+FFA1", "\xEF\xBE\xA1", 0},
    // Mongolian vowel separator.
    {"U+180D", "\xE1\xA0\x8D", 0},
    {"U+180E", "\xE1\xA0\x8E", 3},
    {"U+180F", "\xE1\xA0\x8F", 0},
    // Zero-width characters, LRM and RLM.
    {"U+200A", "\xE2\x80\x8A", 0},
    {"U+200B", "\xE2\x80\x8B", 3},
    {"U+200D", "\xE2\x80\x8D", 3},
    {"U+200F", "\xE2\x80\x8F", 3},
    {"U+2010", "\xE2\x80\x90", 0},
    // Line and paragraph separators, bidi embeddings and overrides.
    {"U+2027", "\xE2\x80\xA7", 0},
    {"U+2028", "\xE2\x80\xA8", 3},
    {"U+2029", "\xE2\x80\xA9", 3},
    {"U+202A", "\xE2\x80\xAA", 3},
    {"U+202E", "\xE2\x80\xAE", 3},
    {"U+202F", "\xE2\x80\xAF", 0},
    // Word joiner and invisible operators; bidi isolates.
    {"U+205F", "\xE2\x81\x9F", 0},
    {"U+2060", "\xE2\x81\xA0", 3},
    {"U+2064", "\xE2\x81\xA4", 3},
    {"U+2065", "\xE2\x81\xA5", 0},
    {"U+2066", "\xE2\x81\xA6", 3},
    {"U+2069", "\xE2\x81\xA9", 3},
    {"U+206A", "\xE2\x81\xAA", 0},
    // Variation selectors.
    {"U+FDFF", "\xEF\xB7\xBF", 0},
    {"U+FE00", "\xEF\xB8\x80", 3},
    {"U+FE0F", "\xEF\xB8\x8F", 3},
    {"U+FE10", "\xEF\xB8\x90", 0},
    {"U+DFFFF", "\xF3\x9F\xBF\xBF", 0},
    {"U+E0100", "\xF3\xA0\x84\x80", 4},
    {"U+E01EF", "\xF3\xA0\x87\xAF", 4},
    {"U+E01F0", "\xF3\xA0\x87\xB0", 0},
    // Byte-order mark.
    {"U+FEFE", "\xEF\xBB\xBE", 0},
    {"U+FEFF", "\xEF\xBB\xBF", 3},
    {"U+FF00", "\xEF\xBC\x80", 0},
    // Tag characters.
    {"U+E0000", "\xF3\xA0\x80\x80", 4},
    {"U+E0041", "\xF3\xA0\x81\x81", 4},
    {"U+E007F", "\xF3\xA0\x81\xBF", 4},
    {"U+E0080", "\xF3\xA0\x82\x80", 0},
    {"U+E00FF", "\xF3\xA0\x83\xBF", 0},
    // Ordinary text.
    {"a", "a", 0},
    {"U+00E9", "\xC3\xA9", 0},
    {"U+65E5", "\xE6\x97\xA5", 0},
    {"U+1F600", "\xF0\x9F\x98\x80", 0},
    {"U+FFFD", "\xEF\xBF\xBD", 0},
};

TEST(HiddenCharsTest, EveryRangeAndItsNeighbours) {
  for (const Case& c : kCases) {
    // At the start, in the middle and at the end of a text.
    EXPECT_EQ(HiddenCharAt(c.bytes, 0), c.hidden) << c.what;
    const std::string in_text = "ab" + std::string(c.bytes) + "cd";
    EXPECT_EQ(HiddenCharAt(in_text, 2), c.hidden) << c.what;
    const std::string at_end = "ab" + std::string(c.bytes);
    EXPECT_EQ(HiddenCharAt(at_end, 2), c.hidden) << c.what;
    // Shown: one U+FFFD for one hidden character, the rest as it is.
    EXPECT_EQ(ShowHidden(in_text),
              "ab" + (c.hidden ? std::string(kReplacement)
                               : std::string(c.bytes)) +
                  "cd")
        << c.what;
  }
}

TEST(HiddenCharsTest, SequencesCutShortAreNotHidden) {
  for (std::string_view cut :
       {"\xC2", "\xD8", "\xE2\x80", "\xE2\x81", "\xE1\x85", "\xEF\xBB",
        "\xEF\xB8", "\xF3\xA0\x80", "\xF3\xA0\x84", "\xF3\xA0"}) {
    EXPECT_EQ(HiddenCharAt(cut, 0), 0u);
    EXPECT_EQ(ShowHidden(cut), cut);
  }
  // Out of range: nothing.
  EXPECT_EQ(HiddenCharAt("", 0), 0u);
  EXPECT_EQ(HiddenCharAt("\xE2\x80\xAE", 3), 0u);
  EXPECT_EQ(HiddenCharAt("\xE2\x80\xAE", 1), 0u);  // a continuation byte
}

TEST(HiddenCharsTest, ShowsEachOneOnce) {
  // A sentence in tag characters, bidi around text, a BOM: each one
  // U+FFFD, the visible text kept.
  std::string tags;
  for (char c : std::string_view("hi")) {
    tags += "\xF3\xA0\x81";
    tags += static_cast<char>(0x80 + (c & 0x3F));
  }
  EXPECT_EQ(ShowHidden("pay" + tags + "now"),
            "pay" + std::string(kReplacement) + kReplacement + "now");
  EXPECT_EQ(ShowHidden("\xE2\x80\xAE" "evil" "\xE2\x80\xAC\xEF\xBB\xBF"),
            std::string(kReplacement) + "evil" + kReplacement + kReplacement);
  EXPECT_EQ(ShowHidden("plain text"), "plain text");
  EXPECT_EQ(ShowHidden(""), "");
}

}  // namespace
}  // namespace hidden_chars
}  // namespace molt_ai
