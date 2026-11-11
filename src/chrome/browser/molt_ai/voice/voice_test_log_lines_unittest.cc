// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_test_log_lines.h"

#include <algorithm>
#include <cmath>
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
  EXPECT_EQ(lines.Heard(8, "open \"YouTube\"", true, true, false, "open_site",
                        ""),
            "{\"t\":8,\"w\":2,\"e\":\"heard\",\"text\":\"open "
            "\\\"YouTube\\\"\",\"source\":\"mic\",\"clean\":true,"
            "\"intent\":\"open_site\"}\n");
  EXPECT_EQ(lines.Heard(9, "harbor", false, false, false, "unknown", "harbor"),
            "{\"t\":9,\"w\":2,\"e\":\"heard\",\"text\":\"[word]\","
            "\"source\":\"typed\",\"clean\":false,\"intent\":\"unknown\"}\n");
  // Said over voice mode's own voice: a choice from the list read only.
  EXPECT_EQ(lines.Heard(10, "Repeat.", true, true, true, "repeat", ""),
            "{\"t\":10,\"w\":2,\"e\":\"heard\",\"text\":\"Repeat.\","
            "\"source\":\"mic\",\"clean\":true,\"over_voice\":true,"
            "\"intent\":\"repeat\"}\n");
  EXPECT_EQ(lines.Said(10, "To allow it, say the word on your screen."),
            "{\"t\":10,\"w\":2,\"e\":\"said\",\"text\":\"To allow it, say the "
            "word on your screen.\"}\n");
}

// What the page map held: counts, the selection, the labels masked and
// capped.
TEST(VoiceTestLogLinesTest, AMapLine) {
  TestLogLines lines(1);
  const std::vector<std::string> labels = {"Skip Ad", "harbor",
                                           std::string(100, 'x')};
  TestLogLines::MapSummary map;
  map.items = 3;
  map.lists = 1;
  map.media = 1;
  map.selection = true;
  map.labels = labels;
  EXPECT_EQ(lines.Map(5, map, "harbor"),
            "{\"t\":5,\"w\":1,\"e\":\"map\",\"items\":3,\"lists\":1,"
            "\"media\":1,\"selection\":true,\"selected_text\":false,"
            "\"labels\":[\"Skip Ad\",\"[word]\",\"" +
                std::string(kMaxTestLogLabelBytes, 'x') + "\"]}\n");
  // At most kMaxTestLogLabels labels.
  const std::vector<std::string> many(kMaxTestLogLabels + 5, "a");
  map.labels = many;
  const std::string line = lines.Map(6, map, "");
  EXPECT_EQ(static_cast<size_t>(std::count(line.begin(), line.end(), '"')),
            // the keys and values before the labels, then two per label
            20u + 2 * kMaxTestLogLabels);
}

TEST(VoiceTestLogLinesTest, AMicrophoneLine) {
  TestLogLines lines(2);
  EXPECT_EQ(lines.Microphone(7, 0.1f, 80000, 160, "chrome-wide"),
            "{\"t\":7,\"w\":2,\"e\":\"mic\",\"peak_db\":-20,"
            "\"samples\":80000,\"dropped\":160,\"echo\":\"chrome-wide\"}\n");
  // Silence, no samples at all, and what can't be a level.
  EXPECT_EQ(lines.Microphone(8, 0, 0, 0, "off"),
            "{\"t\":8,\"w\":2,\"e\":\"mic\",\"peak_db\":-120,"
            "\"samples\":0,\"dropped\":0,\"echo\":\"off\"}\n");
  EXPECT_NE(lines.Microphone(9, std::nanf(""), -5, -1, "none")
                .find("\"peak_db\":-120,\"samples\":0,\"dropped\":0,"),
            std::string::npos);
  // Full scale and over it.
  EXPECT_NE(lines.Microphone(10, 3.0f, 1, 0, "none").find("\"peak_db\":0,"),
            std::string::npos);
}

TEST(VoiceTestLogLinesTest, ADroppedLineMasksTheWord) {
  TestLogLines lines(1);
  EXPECT_EQ(lines.Dropped(3, "the word is harbor", "no wake phrase", ""),
            "{\"t\":3,\"w\":1,\"e\":\"dropped\",\"text\":\"the word is "
            "[word]\",\"reason\":\"no wake phrase\"}\n");
}

TEST(VoiceTestLogLinesTest, HelperAndEarconLines) {
  TestLogLines lines(1);
  EXPECT_EQ(lines.Helper(5, "wake", 120, "taken"),
            "{\"t\":5,\"w\":1,\"e\":\"helper\",\"event\":\"wake\","
            "\"lag_ms\":120,\"note\":\"taken\"}\n");
  EXPECT_EQ(lines.Helper(6, "speech_start", -7, ""),
            "{\"t\":6,\"w\":1,\"e\":\"helper\",\"event\":\"speech_"
            "start\",\"lag_ms\":-1}\n");
  EXPECT_EQ(lines.Earcon(7, "listening"),
            "{\"t\":7,\"w\":1,\"e\":\"earcon\",\"kind\":\"listening\"}\n");
  EXPECT_EQ(lines.Duck(8, true, 2),
            "{\"t\":8,\"w\":1,\"e\":\"duck\",\"on\":true,\"tabs\":2}\n");
  EXPECT_EQ(lines.Duck(9, false, -3),
            "{\"t\":9,\"w\":1,\"e\":\"duck\",\"on\":false,\"tabs\":0}\n");
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
