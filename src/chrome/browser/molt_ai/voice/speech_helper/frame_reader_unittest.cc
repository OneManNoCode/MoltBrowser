// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/speech_helper/frame_reader.h"

#include <stdint.h>

#include <algorithm>
#include <string>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_speech {
namespace {

std::vector<uint8_t> Header(uint32_t type, uint32_t length) {
  std::vector<uint8_t> out;
  for (uint32_t v : {type, length}) {
    for (int i = 0; i < 4; ++i) {
      out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
  }
  return out;
}

std::vector<uint8_t> FrameBytes(uint32_t type, const std::string& payload) {
  std::vector<uint8_t> out =
      Header(type, static_cast<uint32_t>(payload.size()));
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

TEST(SpeechFrameReaderTest, ReadsFramesSplitAnyWay) {
  std::vector<uint8_t> stream = FrameBytes(2, "{\"wake_required\":false}");
  const std::vector<uint8_t> audio = FrameBytes(1, std::string(6, '\x01'));
  stream.insert(stream.end(), audio.begin(), audio.end());
  const std::vector<uint8_t> reset = FrameBytes(3, "");
  stream.insert(stream.end(), reset.begin(), reset.end());

  FrameReader reader;
  std::vector<Frame> frames;
  Frame frame;
  std::string error;
  for (uint8_t byte : stream) {  // One byte at a time.
    ASSERT_TRUE(reader.Append(&byte, 1));
    while (true) {
      const FrameReader::Status status = reader.Next(&frame, &error);
      ASSERT_NE(status, FrameReader::Status::kError) << error;
      if (status == FrameReader::Status::kNeedMore) {
        break;
      }
      frames.push_back(frame);
    }
  }
  ASSERT_EQ(frames.size(), 3u);
  EXPECT_EQ(frames[0].type, FrameType::kConfig);
  EXPECT_EQ(frames[0].payload, "{\"wake_required\":false}");
  EXPECT_EQ(frames[1].type, FrameType::kAudio);
  EXPECT_EQ(frames[1].payload.size(), 6u);
  EXPECT_EQ(frames[2].type, FrameType::kReset);
  EXPECT_TRUE(frames[2].payload.empty());
  EXPECT_EQ(reader.pending_bytes(), 0u);

  // All at once.
  FrameReader whole;
  ASSERT_TRUE(whole.Append(stream.data(), stream.size()));
  int count = 0;
  while (whole.Next(&frame, &error) == FrameReader::Status::kFrame) {
    ++count;
  }
  EXPECT_EQ(count, 3);
}

TEST(SpeechFrameReaderTest, RefusesABadHeaderBeforeItsPayload) {
  struct Case {
    uint32_t type;
    uint32_t length;
  } cases[] = {
      {0, 0},           {5, 0},          {0xFFFFFFFF, 2},  {1, 0},
      {1, 3},           {1, 65538},      {2, 1},           {2, 0},
      {3, 1},           {4, 2},          {2, 0xFFFFFFFF},  {1, 65537},
  };
  for (const Case& c : cases) {
    FrameReader reader;
    const std::vector<uint8_t> header = Header(c.type, c.length);
    ASSERT_TRUE(reader.Append(header.data(), header.size()));
    Frame frame;
    std::string error;
    EXPECT_EQ(reader.Next(&frame, &error), FrameReader::Status::kError)
        << c.type << " " << c.length;
    EXPECT_FALSE(error.empty());
    // Nothing more is read after an error.
    EXPECT_EQ(reader.Next(&frame, &error), FrameReader::Status::kError);
    const uint8_t more = 0;
    EXPECT_FALSE(reader.Append(&more, 1));
  }
}

TEST(SpeechFrameReaderTest, TakesTheLargestFrames) {
  FrameReader reader;
  const std::vector<uint8_t> big = FrameBytes(1, std::string(65536, '\x7f'));
  // Fed in reads of at most kMaxReadBytes, draining in between.
  size_t pos = 0;
  Frame frame;
  std::string error;
  int frames = 0;
  while (pos < big.size()) {
    const size_t n = std::min(kMaxReadBytes, big.size() - pos);
    ASSERT_TRUE(reader.Append(big.data() + pos, n));
    pos += n;
    while (reader.Next(&frame, &error) == FrameReader::Status::kFrame) {
      ++frames;
      EXPECT_EQ(frame.payload.size(), 65536u);
    }
  }
  EXPECT_EQ(frames, 1);
  EXPECT_TRUE(CheckFrameHeader(2, 65536).empty());
  EXPECT_TRUE(CheckFrameHeader(4, 0).empty());
}

TEST(SpeechFrameReaderTest, BoundsWhatItHolds) {
  FrameReader reader;
  std::vector<uint8_t> too_much(kMaxReadBytes + 1, 0);
  EXPECT_FALSE(reader.Append(too_much.data(), too_much.size()));

  // A caller that never drains is refused before the buffer grows past one
  // frame and one read.
  FrameReader undrained;
  const std::vector<uint8_t> header = Header(1, 65536);
  ASSERT_TRUE(undrained.Append(header.data(), header.size()));
  std::vector<uint8_t> chunk(kMaxReadBytes, 0);
  ASSERT_TRUE(undrained.Append(chunk.data(), chunk.size()));
  EXPECT_FALSE(undrained.Append(chunk.data(), chunk.size()));
}

TEST(SpeechFrameReaderTest, ReportsAPartialFrame) {
  FrameReader reader;
  const std::vector<uint8_t> frame_bytes = FrameBytes(1, "abcd");
  ASSERT_TRUE(reader.Append(frame_bytes.data(), 10));
  Frame frame;
  std::string error;
  EXPECT_EQ(reader.Next(&frame, &error), FrameReader::Status::kNeedMore);
  EXPECT_EQ(reader.pending_bytes(), 10u);
  ASSERT_TRUE(reader.Append(frame_bytes.data() + 10, 2));
  EXPECT_EQ(reader.Next(&frame, &error), FrameReader::Status::kFrame);
  EXPECT_EQ(frame.payload, "abcd");
  EXPECT_EQ(reader.pending_bytes(), 0u);
}

}  // namespace
}  // namespace molt_speech
