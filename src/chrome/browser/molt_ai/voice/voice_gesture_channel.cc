// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_gesture_channel.h"

namespace molt_ai {
namespace voice {

WordChannel DecideWordChannel(const WordChannelInputs& inputs) {
  WordChannel channel;
  if (inputs.screen_captured) {
    // Never shown while the screen or a window is captured; said only with
    // the setting (the capture may carry the system's sound).
    channel.available = inputs.say_aloud_anyway;
    channel.said = inputs.say_aloud_anyway;
    return channel;
  }
  channel.available = true;
  channel.shown = true;
  if (inputs.say_aloud_anyway) {
    // Whatever the voice: the user chose to hear it.
    channel.said = true;
    return channel;
  }
  // Said only where nothing but the room hears it: voice mode's own voice,
  // which the echo canceller removes from what the speech helper hears,
  // and no capture of a microphone.
  if (inputs.echo_cancelled && inputs.bundled_voice && !inputs.audio_captured) {
    channel.said = true;
    channel.privately = true;
  }
  return channel;
}

bool WordExposed(const WordChannel& delivered, const WordChannelInputs& now) {
  if (delivered.shown && now.screen_captured) {
    return true;
  }
  return delivered.said && !now.say_aloud_anyway &&
         (now.audio_captured || now.screen_captured);
}

std::string ShownWordText(std::string_view word) {
  if (word.empty() || word.size() > kMaxShownWordChars) {
    return std::string();
  }
  for (char c : word) {
    if (c < 'a' || c > 'z') {
      return std::string();
    }
  }
  return std::string(word);
}

std::string WordChannelName(const WordChannel& channel) {
  if (!channel.available) {
    return "no word";
  }
  std::string name;
  if (channel.said) {
    name = channel.privately ? "said privately" : "said";
  }
  if (channel.shown) {
    name += name.empty() ? "shown" : ", shown";
  }
  return name.empty() ? std::string("neither") : name;
}

}  // namespace voice
}  // namespace molt_ai
