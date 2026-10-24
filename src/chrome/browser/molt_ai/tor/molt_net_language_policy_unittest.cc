// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/molt_net_language_policy.h"

#include <string>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai::tor {
namespace {

// molt_net_pref_isolation_unittest.cc checks these names against
// language::prefs' constants, and that LanguagePrefs can't change them.
TEST(MoltNetLanguagePolicyTest, FixesTheLanguageListAndTheSelectedList) {
  EXPECT_TRUE(IsMoltNetFixedLanguagePref("intl.accept_languages"));
  EXPECT_TRUE(IsMoltNetFixedLanguagePref("intl.selected_languages"));
  // The policy's own pref stays where Chromium puts it (policies rank above
  // anything MoltNet could write); it only feeds LanguagePrefs.
  EXPECT_FALSE(IsMoltNetFixedLanguagePref("intl.forced_languages"));
  // Look-alikes and other language prefs.
  for (const char* other :
       {"", "intl", "intl.", "intl.accept_languages.", "intl.accept_language",
        "Intl.accept_languages", "intl.accept_languages ",
        "intl.app_locale", "intl.charset_default",
        "translate_recent_target", "spellcheck.dictionaries"}) {
    EXPECT_FALSE(IsMoltNetFixedLanguagePref(other)) << other;
  }
}

TEST(MoltNetLanguagePolicyTest, OneListForEveryone) {
  // Fingerprinting protection on: en-US whatever the UI language's default.
  for (const char* registered : {"en-US,en", "de-DE,de,en-US,en", "ja,en-US,en",
                                 "", "fr"}) {
    EXPECT_EQ(MoltNetLanguageList(true, registered), "en-US,en") << registered;
  }
  // Off: the registered default, never anything else.
  EXPECT_EQ(MoltNetLanguageList(false, "de-DE,de,en-US,en"),
            "de-DE,de,en-US,en");
  EXPECT_EQ(MoltNetLanguageList(false, ""), "");
  EXPECT_EQ(kMoltNetUniformLanguages, "en-US,en");
}

TEST(MoltNetLanguagePolicyTest, ReduceAcceptLanguagePolicyIgnoredInMoltNet) {
  // MoltNet: as if the policy were unset, whatever it says.
  EXPECT_TRUE(ReduceAcceptLanguageEnabledFor(true, false));
  EXPECT_TRUE(ReduceAcceptLanguageEnabledFor(true, true));
  // Every other profile: Chromium's value.
  EXPECT_FALSE(ReduceAcceptLanguageEnabledFor(false, false));
  EXPECT_TRUE(ReduceAcceptLanguageEnabledFor(false, true));
}

}  // namespace
}  // namespace molt_ai::tor
