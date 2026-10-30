// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/speech_helper/speech_events.h"

#include <limits>
#include <string>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_speech {
namespace {

EventTime At(int64_t epoch, int64_t audio_ms) {
  EventTime time;
  time.epoch = epoch;
  time.audio_ms = audio_ms;
  return time;
}

TEST(SpeechEventsTest, WritesTheProtocolsLines) {
  EXPECT_EQ(ReadyEvent("sherpa-onnx 2.0.1, onnxruntime 1.28.2",
                       {"kws", "vad", "asr"}),
            "{\"type\":\"ready\",\"protocol\":1,\"engine\":\"sherpa-onnx "
            "2.0.1, onnxruntime 1.28.2\",\"models\":[\"kws\",\"vad\","
            "\"asr\"],\"sample_rate\":16000}\n");
  EXPECT_EQ(WakeEvent(At(0, 5160)),
            "{\"type\":\"wake\",\"epoch\":0,\"audio_ms\":5160}\n");
  EXPECT_EQ(SpeechStartEvent(At(2, 1)),
            "{\"type\":\"speech_start\",\"epoch\":2,\"audio_ms\":1}\n");
  EXPECT_EQ(PartialEvent(At(0, 6600), "Open You"),
            "{\"type\":\"partial\",\"epoch\":0,\"audio_ms\":6600,"
            "\"text\":\"Open You\"}\n");
  EXPECT_EQ(SpeechEndEvent(At(0, 7420)),
            "{\"type\":\"speech_end\",\"epoch\":0,\"audio_ms\":7420}\n");
  EXPECT_EQ(FinalEvent(At(0, 7420), "Open YouTube", 0.934),
            "{\"type\":\"final\",\"epoch\":0,\"audio_ms\":7420,"
            "\"text\":\"Open YouTube\",\"confidence\":0.93}\n");
  EXPECT_EQ(ErrorEvent("protocol", "frame length 70000 is over 65536"),
            "{\"type\":\"error\",\"code\":\"protocol\",\"message\":\"frame "
            "length 70000 is over 65536\"}\n");
}

TEST(SpeechEventsTest, FormatsConfidence) {
  const auto confidence = [](double value) {
    const std::string line = FinalEvent(At(0, 0), "x", value);
    const size_t at = line.find("\"confidence\":") + 13;
    return line.substr(at, line.size() - at - 2);
  };
  EXPECT_EQ(confidence(0), "0.00");
  EXPECT_EQ(confidence(1), "1.00");
  EXPECT_EQ(confidence(0.005), "0.01");
  EXPECT_EQ(confidence(0.994), "0.99");
  EXPECT_EQ(confidence(0.996), "1.00");
  EXPECT_EQ(confidence(7), "1.00");
  EXPECT_EQ(confidence(-1), "0.00");
  EXPECT_EQ(confidence(std::numeric_limits<double>::quiet_NaN()), "0.00");
  EXPECT_EQ(confidence(std::numeric_limits<double>::infinity()), "1.00");
}

TEST(SpeechEventsTest, EscapesAndBoundsText) {
  EXPECT_EQ(JsonQuote("a\"b\\c\x01"), "\"a\\\"b\\\\c\\u0001\"");
  // Text is cleaned (no control characters) before it is quoted.
  EXPECT_EQ(PartialEvent(At(0, 0), "a\nb\x1b[2Jc"),
            "{\"type\":\"partial\",\"epoch\":0,\"audio_ms\":0,"
            "\"text\":\"a b[2Jc\"}\n");
  // The worst case for escaping still fits a line.
  const std::string quotes(5000, '"');
  const std::string line = FinalEvent(At(INT64_MAX, INT64_MAX), quotes, 1);
  EXPECT_LE(line.size(), kMaxEventLineBytes);
  EXPECT_EQ(line.back(), '\n');
  EXPECT_EQ(line.find('\n'), line.size() - 1);

  // Error messages and codes are printable ASCII, bounded.
  const std::string error =
      ErrorEvent("pro\x01tocol", std::string(500, 'm') + "\xC3\xA9\n");
  EXPECT_EQ(error.find("\\u"), std::string::npos);
  EXPECT_LT(error.size(), 300u);
  EXPECT_NE(error.find("\"code\":\"protocol\""), std::string::npos);
}

}  // namespace
}  // namespace molt_speech
