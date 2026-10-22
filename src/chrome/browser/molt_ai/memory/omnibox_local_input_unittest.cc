// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Which omnibox input stays on the device
// (components/omnibox/browser/molt_local_input.h): "@memory ..." and
// "@ai ..." go to their own provider alone, never to the search engine;
// "?" keeps Chromium's meaning (a web search) and is not gated. Standard
// library only. Kept with the @memory tests, whose gate this generalizes.

#include "components/omnibox/browser/molt_local_input.h"

#include <optional>
#include <string>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_omnibox {
namespace {

TEST(MoltLocalInputTest, AiPromptIsTheKeywordAndWhitespace) {
  EXPECT_EQ(AiPromptStart(u"@ai what is gravity"), 4u);
  EXPECT_EQ(AiPromptStart(u"@AI what"), 4u);
  EXPECT_EQ(AiPromptStart(u"@Ai  two spaces"), 4u);  // the rest is the prompt
  EXPECT_EQ(AiPromptStart(u"  @ai x"), 6u);
  EXPECT_EQ(AiPromptStart(u"@ai\u3000\u91CD\u529B"), 4u);  // ideographic
  EXPECT_EQ(AiPromptStart(u"@ai\tx"), 4u);
  EXPECT_EQ(AiPromptStart(u"@ai "), 4u);  // no prompt yet: still local

  EXPECT_EQ(AiPromptStart(u""), std::nullopt);
  EXPECT_EQ(AiPromptStart(u"@ai"), std::nullopt);
  EXPECT_EQ(AiPromptStart(u"@a"), std::nullopt);
  EXPECT_EQ(AiPromptStart(u"@aim high"), std::nullopt);
  EXPECT_EQ(AiPromptStart(u"@ai-x"), std::nullopt);
  EXPECT_EQ(AiPromptStart(u"ai what"), std::nullopt);
  EXPECT_EQ(AiPromptStart(u"x @ai what"), std::nullopt);
  EXPECT_EQ(AiPromptStart(u"duckduckgo.com @ai what"), std::nullopt);
  EXPECT_EQ(AiPromptStart(u"\uFF20ai what"), std::nullopt);  // fullwidth @
}

TEST(MoltLocalInputTest, ClassifiesMemoryAndAiPromptsOnly) {
  EXPECT_EQ(ClassifyLocalInput(u"@memory miso"), LocalInput::kMemoryShortcut);
  EXPECT_EQ(ClassifyLocalInput(u"@memory"), LocalInput::kMemoryShortcut);
  EXPECT_EQ(ClassifyLocalInput(u"@ai what is this"), LocalInput::kAiPrompt);
  EXPECT_EQ(ClassifyLocalInput(u"@AI "), LocalInput::kAiPrompt);

  for (const char16_t* text :
       {u"", u"miso soup", u"@ai", u"@aim", u"@memo", u"@memoryx", u"@",
        u"example.com", u"https://example.com/?q=@ai x"}) {
    EXPECT_EQ(ClassifyLocalInput(text), LocalInput::kNone)
        << std::u16string(text).size();
    EXPECT_FALSE(IsLocalOnlyInput(text));
  }
}

// "?" still means "search the web": typed first, Chromium's edit model
// enters keyword mode for the default search engine and drops the "?";
// pasted, it is ordinary search text. It is never local-only input.
TEST(MoltLocalInputTest, QuestionMarkIsNotGated) {
  for (const char16_t* text : {u"?", u"?what is gravity", u"? x", u"??"}) {
    EXPECT_EQ(ClassifyLocalInput(text), LocalInput::kNone);
    EXPECT_FALSE(IsAiPromptInput(text));
  }
}

}  // namespace
}  // namespace molt_omnibox
