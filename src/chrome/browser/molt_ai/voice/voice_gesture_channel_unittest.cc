// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_gesture_channel.h"

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace voice {
namespace {

WordChannelInputs Private() {
  WordChannelInputs inputs;
  inputs.echo_cancelled = true;
  inputs.bundled_voice = true;
  return inputs;
}

// The decision table of TOUCHLESS_BROWSING.md §7.3, row by row.
TEST(VoiceGestureChannelTest,
     SaidOnlyThroughTheBundledVoiceWithAnEchoCanceller) {
  const WordChannel channel = DecideWordChannel(Private());
  EXPECT_TRUE(channel.available);
  EXPECT_TRUE(channel.said);
  EXPECT_TRUE(channel.privately);
  EXPECT_TRUE(channel.shown);
  EXPECT_EQ(WordChannelName(channel), "said privately, shown");
}

TEST(VoiceGestureChannelTest, ShownOnlyWithoutAnEchoCanceller) {
  WordChannelInputs inputs = Private();
  inputs.echo_cancelled = false;
  const WordChannel channel = DecideWordChannel(inputs);
  EXPECT_TRUE(channel.available);
  EXPECT_FALSE(channel.said);
  EXPECT_TRUE(channel.shown);
  EXPECT_EQ(WordChannelName(channel), "shown");
}

TEST(VoiceGestureChannelTest, ShownOnlyWithThePlatformsVoices) {
  WordChannelInputs inputs = Private();
  inputs.bundled_voice = false;
  const WordChannel channel = DecideWordChannel(inputs);
  EXPECT_TRUE(channel.available);
  EXPECT_FALSE(channel.said);
  EXPECT_TRUE(channel.shown);
}

TEST(VoiceGestureChannelTest, ShownOnlyWhileAMicrophoneIsCaptured) {
  WordChannelInputs inputs = Private();
  inputs.audio_captured = true;
  const WordChannel channel = DecideWordChannel(inputs);
  EXPECT_TRUE(channel.available);
  EXPECT_FALSE(channel.said);
  EXPECT_TRUE(channel.shown);
}

TEST(VoiceGestureChannelTest, NoWordWhileTheScreenIsShared) {
  for (bool echo : {false, true}) {
    for (bool bundled : {false, true}) {
      for (bool audio : {false, true}) {
        WordChannelInputs inputs;
        inputs.echo_cancelled = echo;
        inputs.bundled_voice = bundled;
        inputs.audio_captured = audio;
        inputs.screen_captured = true;
        const WordChannel channel = DecideWordChannel(inputs);
        EXPECT_FALSE(channel.available);
        EXPECT_FALSE(channel.said);
        EXPECT_FALSE(channel.shown);
        EXPECT_EQ(WordChannelName(channel), "no word");
      }
    }
  }
}

TEST(VoiceGestureChannelTest, TheSettingSaysItWhateverTheVoice) {
  WordChannelInputs inputs;
  inputs.say_aloud_anyway = true;
  inputs.audio_captured = true;
  WordChannel channel = DecideWordChannel(inputs);
  EXPECT_TRUE(channel.said);
  EXPECT_FALSE(channel.privately);
  EXPECT_TRUE(channel.shown);
  // Never shown while the screen is shared, setting or not; said with it.
  inputs.screen_captured = true;
  channel = DecideWordChannel(inputs);
  EXPECT_TRUE(channel.available);
  EXPECT_TRUE(channel.said);
  EXPECT_FALSE(channel.shown);
  EXPECT_EQ(WordChannelName(channel), "said");
}

TEST(VoiceGestureChannelTest, NeverShownWhileTheScreenIsCaptured) {
  // Every combination of inputs.
  for (int bits = 0; bits < 32; ++bits) {
    WordChannelInputs inputs;
    inputs.echo_cancelled = bits & 1;
    inputs.bundled_voice = bits & 2;
    inputs.audio_captured = bits & 4;
    inputs.screen_captured = bits & 8;
    inputs.say_aloud_anyway = bits & 16;
    const WordChannel channel = DecideWordChannel(inputs);
    if (inputs.screen_captured) {
      EXPECT_FALSE(channel.shown) << bits;
    }
    // Said without the setting only privately, and only where nothing
    // captures.
    if (channel.said && !inputs.say_aloud_anyway) {
      EXPECT_TRUE(channel.privately) << bits;
      EXPECT_TRUE(inputs.echo_cancelled && inputs.bundled_voice) << bits;
      EXPECT_FALSE(inputs.audio_captured || inputs.screen_captured) << bits;
    }
    // A word always has a way to the user.
    if (channel.available) {
      EXPECT_TRUE(channel.said || channel.shown) << bits;
    }
  }
}

TEST(VoiceGestureChannelTest, AWordThatWentOutIsExposedByALaterCapture) {
  const WordChannel said_and_shown = DecideWordChannel(Private());
  WordChannelInputs now = Private();
  EXPECT_FALSE(WordExposed(said_and_shown, now));
  now.audio_captured = true;
  EXPECT_TRUE(WordExposed(said_and_shown, now));
  now.audio_captured = false;
  now.screen_captured = true;
  EXPECT_TRUE(WordExposed(said_and_shown, now));

  // Shown only: a microphone can't hear it; the screen can see it.
  WordChannelInputs platform = Private();
  platform.bundled_voice = false;
  const WordChannel shown = DecideWordChannel(platform);
  platform.audio_captured = true;
  EXPECT_FALSE(WordExposed(shown, platform));
  platform.screen_captured = true;
  EXPECT_TRUE(WordExposed(shown, platform));

  // With the setting, what was said stays the user's choice; what was shown
  // is still exposed by a capture of the screen.
  WordChannelInputs anyway = Private();
  anyway.say_aloud_anyway = true;
  const WordChannel chosen = DecideWordChannel(anyway);
  anyway.audio_captured = true;
  EXPECT_FALSE(WordExposed(chosen, anyway));
  anyway.screen_captured = true;
  EXPECT_TRUE(WordExposed(chosen, anyway));
}

TEST(VoiceGestureChannelTest, OnlyAWordOfLettersIsShown) {
  EXPECT_EQ(ShownWordText("harbor"), "harbor");
  EXPECT_EQ(ShownWordText(""), "");
  EXPECT_EQ(ShownWordText("Harbor"), "");
  EXPECT_EQ(ShownWordText("harbor "), "");
  EXPECT_EQ(ShownWordText("a\xC3\xA9"), "");
  EXPECT_EQ(ShownWordText(std::string(kMaxShownWordChars, 'a')),
            std::string(kMaxShownWordChars, 'a'));
  EXPECT_EQ(ShownWordText(std::string(kMaxShownWordChars + 1, 'a')), "");
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
