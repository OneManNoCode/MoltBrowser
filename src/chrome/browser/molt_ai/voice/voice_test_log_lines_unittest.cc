// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_test_log_lines.h"

#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/voice/spoken_text.h"
#include "chrome/browser/molt_ai/voice/voice_gesture.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace voice {
namespace {

TEST(VoiceTestLogLinesTest, MasksEveryConfirmationWord) {
  EXPECT_EQ(MaskConfirmationWords("harbor", ""), "[word]");
  EXPECT_EQ(MaskConfirmationWords("Yes, Harbor.", ""), "Yes, [word].");
  EXPECT_EQ(MaskConfirmationWords("hey molt say maple please", ""),
            "hey molt say [word] please");
  // A plural, a long word heard one letter off, and the word split in two
  // by the recognizer.
  EXPECT_EQ(MaskConfirmationWords("tigers", ""), "[word]");
  EXPECT_EQ(MaskConfirmationWords("harbour", ""), "[word]");
  EXPECT_EQ(MaskConfirmationWords("Harber!", ""), "[word]!");
  EXPECT_EQ(MaskConfirmationWords("lanter", ""), "[word]");
  EXPECT_EQ(MaskConfirmationWords("the harb or one", ""), "the [word] one");
  // Spanish words too, accents and all.
  const std::string_view spanish =
      ConfirmationWords(Language::kSpanish).front();
  EXPECT_EQ(
      MaskConfirmationWords(std::string("di ") + std::string(spanish), ""),
      "di [word]");
  // Every word of both lists, alone.
  for (Language language : {Language::kEnglish, Language::kSpanish}) {
    for (std::string_view word : ConfirmationWords(language)) {
      EXPECT_EQ(MaskConfirmationWords(word, ""), "[word]") << word;
    }
  }
}

TEST(VoiceTestLogLinesTest, KeepsOtherWords) {
  EXPECT_EQ(MaskConfirmationWords("open YouTube", ""), "open YouTube");
  EXPECT_EQ(MaskConfirmationWords("Pressed Add to cart.", ""),
            "Pressed Add to cart.");
  EXPECT_EQ(MaskConfirmationWords("play the fourth one", ""),
            "play the fourth one");
  EXPECT_EQ(
      MaskConfirmationWords("To allow it, say the word on your screen.", ""),
      "To allow it, say the word on your screen.");
  // Short words are masked only as they are (or as plurals).
  EXPECT_EQ(MaskConfirmationWords("an apple, a tiler", ""),
            "an apple, a tiler");
  EXPECT_EQ(MaskConfirmationWords("", ""), "");
}

TEST(VoiceTestLogLinesTest, MasksThePendingWord) {
  // A word outside the lists (a future list's) is masked while it waits.
  EXPECT_EQ(MaskConfirmationWords("say zeppelin now", "zeppelin"),
            "say [word] now");
  EXPECT_EQ(MaskConfirmationWords("say zeppelin now", ""), "say zeppelin now");
}

TEST(VoiceTestLogLinesTest, CleansAndCaps) {
  EXPECT_EQ(MaskConfirmationWords("a\x01\xff"
                                  "b\n c",
                                  ""),
            "ab c");
  const std::string long_text(3 * kMaxTestLogTextBytes, 'x');
  EXPECT_LE(MaskConfirmationWords(long_text, "").size(), kMaxTestLogTextBytes);
  // A long text whose last word is a confirmation word is cut, not left
  // with the word unmasked.
  std::string words;
  while (words.size() < kMaxTestLogTextBytes - 10) {
    words.append("go ");
  }
  words.append("harbor");
  const std::string masked = MaskConfirmationWords(words, "");
  EXPECT_EQ(masked.find("harbor"), std::string::npos);
}

TEST(VoiceTestLogLinesTest, EscapesJson) {
  EXPECT_EQ(TestLogJsonEscape("say \"hi\" \\ there"),
            "say \\\"hi\\\" \\\\ there");
  EXPECT_EQ(TestLogJsonEscape("a\nb\tc\x01"), "a\\nb\\tc\\u0001");
  EXPECT_EQ(TestLogJsonEscape("Beyonc\xC3\xA9"), "Beyonc\xC3\xA9");
}

TEST(VoiceTestLogLinesTest, Lines) {
  TestLogLines lines(2);
  const std::vector<std::string_view> kinds = {"unknown", "open_site"};
  EXPECT_EQ(lines.Start(7, kinds),
            "{\"t\":7,\"w\":2,\"e\":\"start\",\"kinds\":[\"unknown\","
            "\"open_site\"]}\n");
  EXPECT_EQ(lines.Heard(8, "open \"YouTube\"", true, true, "open_site", ""),
            "{\"t\":8,\"w\":2,\"e\":\"heard\",\"text\":\"open "
            "\\\"YouTube\\\"\",\"source\":\"mic\",\"clean\":true,"
            "\"intent\":\"open_site\"}\n");
  EXPECT_EQ(lines.Heard(9, "harbor", false, false, "unknown", "harbor"),
            "{\"t\":9,\"w\":2,\"e\":\"heard\",\"text\":\"[word]\","
            "\"source\":\"typed\",\"clean\":false,\"intent\":\"unknown\"}\n");
  EXPECT_EQ(lines.Said(10, "To allow it, say the word on your screen."),
            "{\"t\":10,\"w\":2,\"e\":\"said\",\"text\":\"To allow it, say the "
            "word on your screen.\"}\n");
}

TEST(VoiceTestLogLinesTest, WritesWhatChanged) {
  TestLogLines lines(1);
  TestLogState state;
  // Nothing changed from the start.
  EXPECT_EQ(lines.Changed(1, state), "");
  state.activity = "listening";
  state.mic = true;
  state.waiting = true;
  EXPECT_EQ(lines.Changed(2, state),
            "{\"t\":2,\"w\":1,\"e\":\"activity\",\"activity\":\"listening\","
            "\"mic\":true,\"waiting\":true}\n");
  EXPECT_EQ(lines.Changed(3, state), "");
  // A prompt opens; another takes its place; it closes.
  state.prompt_id = 4;
  state.prompt_kind = "permission";
  EXPECT_EQ(lines.Changed(4, state),
            "{\"t\":4,\"w\":1,\"e\":\"prompt\",\"op\":\"open\",\"id\":4,"
            "\"kind\":\"permission\"}\n");
  state.prompt_id = 5;
  state.prompt_kind = "confirm";
  EXPECT_EQ(lines.Changed(5, state),
            "{\"t\":5,\"w\":1,\"e\":\"prompt\",\"op\":\"close\",\"id\":4}\n"
            "{\"t\":5,\"w\":1,\"e\":\"prompt\",\"op\":\"open\",\"id\":5,"
            "\"kind\":\"confirm\"}\n");
  state.prompt_id = 0;
  EXPECT_EQ(lines.Changed(6, state),
            "{\"t\":6,\"w\":1,\"e\":\"prompt\",\"op\":\"close\",\"id\":5}\n");
  // The loop's own question, with what it is about (masked too).
  state.confirming = true;
  state.confirm_what = "press Buy now";
  EXPECT_EQ(lines.Changed(7, state),
            "{\"t\":7,\"w\":1,\"e\":\"confirm\",\"op\":\"open\",\"what\":"
            "\"press Buy now\"}\n");
  state.confirming = false;
  EXPECT_EQ(lines.Changed(8, state),
            "{\"t\":8,\"w\":1,\"e\":\"confirm\",\"op\":\"close\"}\n");
  state.confirming = true;
  state.confirm_what = "keep lemon.exe";
  EXPECT_EQ(lines.Changed(9, state),
            "{\"t\":9,\"w\":1,\"e\":\"confirm\",\"op\":\"open\",\"what\":"
            "\"keep [word].exe\"}\n");
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
