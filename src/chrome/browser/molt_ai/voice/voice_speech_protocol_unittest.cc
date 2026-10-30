// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_speech_protocol.h"

#include <limits>
#include <string>
#include <vector>

#include "chrome/browser/molt_ai/voice/voice_helper/voice_frames.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace voice {
namespace {

using molt_voice::DoneInfo;
using molt_voice::DoneStatus;
using molt_voice::Frame;

Frame FrameOf(const std::string& encoded) {
  Frame frame;
  frame.type = static_cast<uint8_t>(encoded[0]);
  frame.payload = encoded.substr(molt_voice::kFrameHeaderBytes);
  return frame;
}

Frame Ready(int rate = 22050) {
  return FrameOf(molt_voice::EncodeReady(rate, "piper"));
}

Frame Audio(uint32_t id, size_t samples) {
  const std::vector<int16_t> pcm(samples, 100);
  return FrameOf(molt_voice::EncodeAudio(id, pcm.data(), pcm.size()));
}

Frame Done(uint32_t id,
           uint32_t audio_ms,
           DoneStatus status = DoneStatus::kSpoken) {
  DoneInfo done;
  done.id = id;
  done.status = status;
  done.audio_ms = audio_ms;
  return FrameOf(molt_voice::EncodeDone(done));
}

TEST(VoiceSpeechProtocolTest, Rate) {
  EXPECT_EQ(100, VoiceRatePercent(1.0));
  EXPECT_EQ(125, VoiceRatePercent(1.25));
  EXPECT_EQ(50, VoiceRatePercent(0.1));
  EXPECT_EQ(250, VoiceRatePercent(10.0));
  EXPECT_EQ(100, VoiceRatePercent(std::numeric_limits<double>::quiet_NaN()));
}

TEST(VoiceSpeechProtocolTest, InOrder) {
  VoiceStreamChecker checker;
  VoiceEvent event;
  std::string why;
  ASSERT_TRUE(checker.Accept(Ready(8000), &event, &why)) << why;
  EXPECT_EQ(VoiceEventType::kReady, event.type);
  EXPECT_EQ(8000, checker.sample_rate());
  checker.Sent(1);
  checker.Sent(2);
  EXPECT_EQ(2u, checker.outstanding());
  ASSERT_TRUE(checker.Accept(Audio(1, 4000), &event, &why)) << why;
  EXPECT_EQ(VoiceEventType::kAudio, event.type);
  EXPECT_EQ(1u, event.id);
  EXPECT_EQ(4000u, event.samples.size());
  ASSERT_TRUE(checker.Accept(Audio(1, 4000), &event, &why)) << why;
  // 8,000 samples at 8 kHz: 1,000 ms.
  ASSERT_TRUE(checker.Accept(Done(1, 1000), &event, &why)) << why;
  EXPECT_EQ(VoiceEventType::kDone, event.type);
  EXPECT_EQ(1u, event.id);
  // A request with nothing to say.
  ASSERT_TRUE(
      checker.Accept(Done(2, 0, DoneStatus::kNothingToSay), &event, &why))
      << why;
  EXPECT_EQ(0u, checker.outstanding());
  // An error line may come at any time (the helper exits after it).
  ASSERT_TRUE(checker.Accept(
      FrameOf(molt_voice::EncodeError(molt_voice::ErrorCode::kEngine, "x")),
      &event, &why));
  EXPECT_EQ(VoiceEventType::kError, event.type);
}

TEST(VoiceSpeechProtocolTest, Violations) {
  struct Case {
    const char* name;
    std::vector<Frame> frames;
  };
  const Case cases[] = {
      {"audio before ready", {Audio(1, 10)}},
      {"ready twice", {Ready(), Ready()}},
      {"bad rate", {Ready(7999)}},
      {"audio for no request", {Ready(), Audio(9, 10)}},
      {"audio for the second request", {Ready(), Audio(2, 10)}},
      {"done out of order", {Ready(), Done(2, 0)}},
      {"done with the wrong length", {Ready(), Audio(1, 22050), Done(1, 999)}},
      {"done twice", {Ready(), Done(1, 0), Done(1, 0), Done(1, 0)}},
  };
  for (const Case& c : cases) {
    VoiceStreamChecker checker;
    checker.Sent(1);
    checker.Sent(2);
    VoiceEvent event;
    std::string why;
    bool ok = true;
    for (const Frame& frame : c.frames) {
      ok = checker.Accept(frame, &event, &why);
      if (!ok) {
        break;
      }
    }
    EXPECT_FALSE(ok) << c.name;
    EXPECT_FALSE(why.empty()) << c.name;
  }
  // A protocol this browser doesn't speak.
  VoiceStreamChecker checker;
  Frame ready = Ready();
  ready.payload[0] = 2;
  VoiceEvent event;
  std::string why;
  EXPECT_FALSE(checker.Accept(ready, &event, &why));
}

TEST(VoiceSpeechProtocolTest, AudioLimitPerRequest) {
  VoiceStreamChecker checker;
  VoiceEvent event;
  std::string why;
  ASSERT_TRUE(checker.Accept(Ready(8000), &event, &why));
  checker.Sent(1);
  const size_t max = static_cast<size_t>(kMaxRequestAudioMs) * 8;
  size_t sent = 0;
  bool ok = true;
  while (ok && sent <= max) {
    ok = checker.Accept(Audio(1, molt_voice::kMaxAudioFrameSamples), &event,
                        &why);
    sent += molt_voice::kMaxAudioFrameSamples;
  }
  EXPECT_FALSE(ok);
  EXPECT_GT(sent, max);
  // A new helper starts afresh.
  checker.Reset();
  EXPECT_FALSE(checker.ready());
  EXPECT_EQ(0u, checker.outstanding());
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
