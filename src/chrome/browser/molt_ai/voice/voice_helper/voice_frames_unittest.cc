// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_helper/voice_frames.h"

#include <string>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_voice {
namespace {

std::string Header(uint32_t type, uint32_t length) {
  std::string out;
  for (uint32_t v : {type, length}) {
    for (int shift = 0; shift < 32; shift += 8) {
      out.push_back(static_cast<char>((v >> shift) & 0xFF));
    }
  }
  return out;
}

FrameReader::Status Feed(FrameReader* reader,
                         const std::string& bytes,
                         Frame* frame,
                         std::string* error) {
  reader->Append(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
  return reader->Next(frame, error);
}

TEST(VoiceFramesTest, SpeakRoundTrip) {
  FrameReader reader(FrameReader::Direction::kToHelper);
  const std::string bytes = EncodeSpeak(7, 125, "YouTube is open.");
  Frame frame;
  std::string error;
  // Split anywhere: one byte at a time.
  for (size_t i = 0; i + 1 < bytes.size(); ++i) {
    reader.Append(reinterpret_cast<const uint8_t*>(&bytes[i]), 1);
    ASSERT_EQ(FrameReader::Status::kNeedMore, reader.Next(&frame, &error));
  }
  reader.Append(reinterpret_cast<const uint8_t*>(&bytes.back()), 1);
  ASSERT_EQ(FrameReader::Status::kFrame, reader.Next(&frame, &error));
  EXPECT_EQ(static_cast<uint32_t>(ToHelper::kSpeak), frame.type);
  SpeakRequest request;
  ASSERT_TRUE(ParseSpeak(frame.payload, &request, &error)) << error;
  EXPECT_EQ(7u, request.id);
  EXPECT_EQ(125, request.rate_percent);
  EXPECT_EQ("YouTube is open.", request.text);
  EXPECT_EQ(FrameReader::Status::kNeedMore, reader.Next(&frame, &error));
}

TEST(VoiceFramesTest, BadFramesToHelper) {
  const struct {
    std::string bytes;
  } kBad[] = {
      {Header(9, 0)},                 // unknown type
      {Header(0, 0)},                 // unknown type
      {Header(2, 1) + "x"},           // CANCEL with a payload
      {Header(3, 4)},                 // SHUTDOWN with a payload
      {Header(1, 8)},                 // SPEAK without text
      {Header(1, 8 + 4097)},          // SPEAK with too much
      {Header(1, 0xFFFFFFFFu)},       // refused from the header alone
      {Header(2, 0) + Header(4, 0)},  // a good frame, then an unknown one
  };
  for (const auto& bad : kBad) {
    FrameReader reader(FrameReader::Direction::kToHelper);
    reader.Append(reinterpret_cast<const uint8_t*>(bad.bytes.data()),
                  bad.bytes.size());
    Frame frame;
    std::string error;
    FrameReader::Status status;
    while ((status = reader.Next(&frame, &error)) ==
           FrameReader::Status::kFrame) {
    }
    EXPECT_EQ(FrameReader::Status::kError, status);
    EXPECT_FALSE(error.empty());
    // It stays failed.
    EXPECT_FALSE(reader.Append(reinterpret_cast<const uint8_t*>("x"), 1));
  }
}

TEST(VoiceFramesTest, SpeakPayload) {
  std::string error;
  SpeakRequest request;
  // Rate out of range, reserved field set.
  std::string payload = EncodeSpeak(1, 49, "x").substr(kFrameHeaderBytes);
  EXPECT_FALSE(ParseSpeak(payload, &request, &error));
  payload = EncodeSpeak(1, 251, "x").substr(kFrameHeaderBytes);
  EXPECT_FALSE(ParseSpeak(payload, &request, &error));
  payload = EncodeSpeak(1, 100, "x").substr(kFrameHeaderBytes);
  payload[6] = 1;
  EXPECT_FALSE(ParseSpeak(payload, &request, &error));
  // Any bytes as text (cleaned later).
  payload =
      EncodeSpeak(1, 100, std::string("\0\xFF", 2)).substr(kFrameHeaderBytes);
  EXPECT_TRUE(ParseSpeak(payload, &request, &error));
}

TEST(VoiceFramesTest, HelperFrames) {
  FrameReader reader(FrameReader::Direction::kFromHelper);
  const std::vector<int16_t> samples = {0, 1, -1, 32767, -32768};
  const std::string bytes =
      EncodeReady(22050, "piper") + EncodeAudio(3, samples.data(), 5) +
      EncodeDone(DoneInfo{3, DoneStatus::kSpoken, 1500, 80, 120}) +
      EncodeError(ErrorCode::kProtocol, "bad\nframe");
  reader.Append(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
  Frame frame;
  std::string error;

  ASSERT_EQ(FrameReader::Status::kFrame, reader.Next(&frame, &error));
  ReadyInfo ready;
  ASSERT_TRUE(ParseReady(frame.payload, &ready));
  EXPECT_EQ(kProtocolVersion, ready.protocol);
  EXPECT_EQ(22050, ready.sample_rate);
  EXPECT_EQ("piper", ready.voice);

  ASSERT_EQ(FrameReader::Status::kFrame, reader.Next(&frame, &error));
  uint32_t id = 0;
  std::vector<int16_t> got;
  ASSERT_TRUE(ParseAudio(frame.payload, &id, &got));
  EXPECT_EQ(3u, id);
  EXPECT_EQ(samples, got);

  ASSERT_EQ(FrameReader::Status::kFrame, reader.Next(&frame, &error));
  DoneInfo done;
  ASSERT_TRUE(ParseDone(frame.payload, &done));
  EXPECT_EQ(3u, done.id);
  EXPECT_EQ(DoneStatus::kSpoken, done.status);
  EXPECT_EQ(1500u, done.audio_ms);
  EXPECT_EQ(80u, done.first_audio_ms);
  EXPECT_EQ(120u, done.synth_ms);

  ASSERT_EQ(FrameReader::Status::kFrame, reader.Next(&frame, &error));
  ErrorCode code;
  std::string message;
  ASSERT_TRUE(ParseError(frame.payload, &code, &message));
  EXPECT_EQ(ErrorCode::kProtocol, code);
  EXPECT_EQ("bad?frame", message);
}

TEST(VoiceFramesTest, BadHelperFrames) {
  const std::string kBad[] = {
      Header(5, 0),            // unknown type
      Header(2, 5) + "12345",  // AUDIO: odd sample bytes
      Header(2, 4) + "1234",   // AUDIO without samples
      Header(2, 65537),        // AUDIO too long
      Header(3, 19),           // DONE of the wrong size
      Header(1, 7),            // READY too short
      Header(4, 205),          // ERROR too long
  };
  for (const std::string& bad : kBad) {
    FrameReader reader(FrameReader::Direction::kFromHelper);
    Frame frame;
    std::string error;
    EXPECT_EQ(FrameReader::Status::kError, Feed(&reader, bad, &frame, &error))
        << error;
  }
  // Payload checks.
  ReadyInfo ready;
  EXPECT_FALSE(
      ParseReady(EncodeReady(7999, "x").substr(kFrameHeaderBytes), &ready));
  EXPECT_FALSE(
      ParseReady(EncodeReady(48001, "x").substr(kFrameHeaderBytes), &ready));
  std::string payload = EncodeReady(22050, "ab").substr(kFrameHeaderBytes);
  payload[8] = '\x01';
  EXPECT_FALSE(ParseReady(payload, &ready));
  DoneInfo done;
  payload = EncodeDone(DoneInfo()).substr(kFrameHeaderBytes);
  payload[4] = 9;  // status
  EXPECT_FALSE(ParseDone(payload, &done));
  ErrorCode code;
  std::string message;
  payload = EncodeError(ErrorCode::kModel, "x").substr(kFrameHeaderBytes);
  payload[0] = 0;
  EXPECT_FALSE(ParseError(payload, &code, &message));
}

}  // namespace
}  // namespace molt_voice
