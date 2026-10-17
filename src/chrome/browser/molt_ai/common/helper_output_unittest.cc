// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/common/helper_output.h"

#include <string>

#include "base/strings/string_util.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace {

TEST(SanitizeHelperTextTest, ValidTextIsKept) {
  EXPECT_EQ(SanitizeHelperText("hello", 100), "hello");
  EXPECT_EQ(SanitizeHelperText("caf\xC3\xA9", 100), "caf\xC3\xA9");
  EXPECT_EQ(SanitizeHelperText("", 100), "");
}

TEST(SanitizeHelperTextTest, InvalidBytesBecomeReplacementCharacters) {
  // Latin-1 'é' (0xE9) and a stray continuation byte.
  const std::string out = SanitizeHelperText("caf\xE9 \x80!", 100);
  EXPECT_TRUE(base::IsStringUTF8AllowingNoncharacters(out));
  EXPECT_EQ(out, "caf\xEF\xBF\xBD \xEF\xBF\xBD!");
}

TEST(SanitizeHelperTextTest, CutsAtACharacterBoundary) {
  // "aé" is 3 bytes; a 2-byte cut would split the é.
  EXPECT_EQ(SanitizeHelperText("a\xC3\xA9", 2), "a");
  EXPECT_EQ(SanitizeHelperText("a\xC3\xA9", 3), "a\xC3\xA9");
  EXPECT_EQ(SanitizeHelperText("abc", 0), "");
}

TEST(SanitizeHelperTextTest, CutAfterReplacement) {
  // Each invalid byte grows to 3 bytes; the limit applies to the result.
  const std::string out = SanitizeHelperText("\xFF\xFF\xFF", 4);
  EXPECT_EQ(out, "\xEF\xBF\xBD");
}

}  // namespace
}  // namespace molt_ai
