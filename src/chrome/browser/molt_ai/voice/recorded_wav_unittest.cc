// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/recorded_wav.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace voice {
namespace {

void Le16(std::string* out, uint16_t value) {
  out->push_back(static_cast<char>(value & 0xff));
  out->push_back(static_cast<char>(value >> 8));
}

void Le32(std::string* out, uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out->push_back(static_cast<char>((value >> shift) & 0xff));
  }
}

std::string Chunk(std::string_view id, std::string_view body) {
  std::string out(id);
  Le32(&out, static_cast<uint32_t>(body.size()));
  out += body;
  if (body.size() % 2) {
    out.push_back('\0');
  }
  return out;
}

struct Format {
  uint16_t tag = 1;
  uint16_t channels = 1;
  uint32_t rate = 16000;
  uint32_t byte_rate = 32000;
  uint16_t block_align = 2;
  uint16_t bits = 16;
};

std::string FormatBody(const Format& format) {
  std::string body;
  Le16(&body, format.tag);
  Le16(&body, format.channels);
  Le32(&body, format.rate);
  Le32(&body, format.byte_rate);
  Le16(&body, format.block_align);
  Le16(&body, format.bits);
  return body;
}

std::string Riff(std::string_view chunks) {
  std::string out = "RIFF";
  Le32(&out, static_cast<uint32_t>(4 + chunks.size()));
  out += "WAVE";
  out += chunks;
  return out;
}

// What the page's _wavBytesFromPCM() writes for |pcm|.
std::string PageWav(const std::vector<int16_t>& pcm) {
  std::string samples;
  for (int16_t sample : pcm) {
    Le16(&samples, static_cast<uint16_t>(sample));
  }
  return Riff(Chunk("fmt ", FormatBody(Format())) + Chunk("data", samples));
}

std::string Samples(size_t count) {
  std::string samples;
  for (size_t i = 0; i < count; ++i) {
    Le16(&samples, static_cast<uint16_t>(i * 977));
  }
  return samples;
}

RecordedWavError Parse(std::string_view wav) {
  std::string_view samples;
  return ParseRecordedWav(wav, &samples);
}

TEST(RecordedWavTest, AcceptsWhatThePageRecords) {
  const std::string wav = PageWav({0, 1, -1, 32767, -32768, 1234});
  ASSERT_EQ(wav.size(), kCanonicalWavHeaderBytes + 12);
  std::string_view samples;
  ASSERT_EQ(ParseRecordedWav(wav, &samples), RecordedWavError::kNone);
  EXPECT_EQ(samples, std::string_view(wav).substr(44));
  // The page's header is already the canonical one.
  EXPECT_EQ(CanonicalWav(samples), wav);
}

TEST(RecordedWavTest, TheCanonicalWavIsAFixedHeaderAndTheSamples) {
  const std::string samples = Samples(3);
  const std::string wav = CanonicalWav(samples);
  const std::string header(
      "RIFF\x2a\x00\x00\x00WAVEfmt \x10\x00\x00\x00\x01\x00\x01\x00"
      "\x80\x3e\x00\x00\x00\x7d\x00\x00\x02\x00\x10\x00"
      "data\x06\x00\x00\x00",
      44);
  EXPECT_EQ(wav, header + samples);
  std::string_view parsed;
  EXPECT_EQ(ParseRecordedWav(wav, &parsed), RecordedWavError::kNone);
  EXPECT_EQ(parsed, samples);

  // Only whole samples, and only what fits.
  EXPECT_EQ(CanonicalWav("abc"), "");
  EXPECT_EQ(CanonicalWav(std::string(kMaxRecordedWavBytes, 'x')), "");
  EXPECT_EQ(CanonicalWav(std::string(kMaxRecordedWavBytes - 44, 'x')).size(),
            kMaxRecordedWavBytes);
}

TEST(RecordedWavTest, SkipsOtherChunks) {
  const std::string samples = Samples(5);
  // A LIST chunk first, an odd-sized one (with its pad byte) between the
  // format and the data, a second format chunk that is ignored.
  Format other;
  other.channels = 2;
  const std::string wav =
      Riff(Chunk("LIST", "INFOISFT\x05\x00\x00\x00Lavf") +
           Chunk("fmt ", FormatBody(Format())) + Chunk("junk", "odd") +
           Chunk("fmt ", FormatBody(other)) + Chunk("data", samples) +
           Chunk("LIST", "tail"));
  std::string_view parsed;
  ASSERT_EQ(ParseRecordedWav(wav, &parsed), RecordedWavError::kNone);
  EXPECT_EQ(parsed, samples);
  EXPECT_EQ(CanonicalWav(parsed), PageWav({0, 977, 1954, 2931, 3908}));

  // A longer format chunk (an extension size after the PCM fields).
  const std::string extended =
      Riff(Chunk("fmt ", FormatBody(Format()) + std::string("\x00\x00", 2)) +
           Chunk("data", samples));
  EXPECT_EQ(Parse(extended), RecordedWavError::kNone);
}

TEST(RecordedWavTest, RefusesOtherFormats) {
  std::vector<Format> formats(8);
  formats[0].channels = 2;
  formats[0].byte_rate = 64000;
  formats[0].block_align = 4;
  formats[1].rate = 44100;
  formats[1].byte_rate = 88200;
  formats[2].bits = 8;
  formats[2].byte_rate = 16000;
  formats[2].block_align = 1;
  formats[3].tag = 3;  // IEEE float
  formats[3].bits = 32;
  formats[3].byte_rate = 64000;
  formats[3].block_align = 4;
  formats[4].tag = 0xfffe;  // WAVE_FORMAT_EXTENSIBLE
  formats[5].tag = 0x55;    // MPEG layer 3
  formats[6].byte_rate = 16000;
  formats[7].block_align = 4;
  for (size_t i = 0; i < formats.size(); ++i) {
    EXPECT_EQ(Parse(Riff(Chunk("fmt ", FormatBody(formats[i])) +
                         Chunk("data", Samples(4)))),
              RecordedWavError::kWrongFormat)
        << i;
  }
}

TEST(RecordedWavTest, RefusesWhatIsntWav) {
  EXPECT_EQ(Parse(""), RecordedWavError::kNotWav);
  EXPECT_EQ(Parse("RIFF"), RecordedWavError::kNotWav);
  std::string wav = PageWav({1, 2});
  wav[3] = 'X';  // RIFX: big-endian
  EXPECT_EQ(Parse(wav), RecordedWavError::kNotWav);
  wav = PageWav({1, 2});
  wav.replace(8, 4, "AVI ");
  EXPECT_EQ(Parse(wav), RecordedWavError::kNotWav);
  // What miniaudio would also decode.
  EXPECT_EQ(Parse("ID3\x04\x00\x00\x00\x00\x00\x00\xff\xfb"),
            RecordedWavError::kNotWav);
  EXPECT_EQ(Parse("fLaC\x00\x00\x00\x22\x10\x00\x10\x00"),
            RecordedWavError::kNotWav);
}

TEST(RecordedWavTest, RefusesDamagedFiles) {
  const std::string format = Chunk("fmt ", FormatBody(Format()));
  // The data chunk claims more than there is.
  std::string wav = PageWav({1, 2, 3});
  wav[40] = 8;
  EXPECT_EQ(Parse(wav), RecordedWavError::kTruncated);
  // So does the format chunk.
  wav = Riff(format.substr(0, 4) + std::string("\xff\x00\x00\x00", 4) +
             format.substr(8));
  EXPECT_EQ(Parse(wav), RecordedWavError::kTruncated);
  // The RIFF size ends the chunks: data past it doesn't count.
  wav = PageWav({1, 2, 3});
  wav[4] = 36;
  EXPECT_EQ(Parse(wav), RecordedWavError::kTruncated);
  // A RIFF size past the end of the file: the file's end counts.
  wav = PageWav({1, 2, 3});
  wav[7] = 0x7f;
  EXPECT_EQ(Parse(wav), RecordedWavError::kNone);

  // Data before any format; a short format chunk.
  EXPECT_EQ(Parse(Riff(Chunk("data", Samples(2)) + format)),
            RecordedWavError::kNoFormat);
  EXPECT_EQ(Parse(Riff(Chunk("fmt ", FormatBody(Format()).substr(0, 14)) +
                       Chunk("data", Samples(2)))),
            RecordedWavError::kNoFormat);
  // No data chunk at all.
  EXPECT_EQ(Parse(Riff(format)), RecordedWavError::kNoData);
  EXPECT_EQ(Parse(Riff("")), RecordedWavError::kNoData);
  EXPECT_EQ(Parse(Riff(format + "dat")), RecordedWavError::kNoData);
}

TEST(RecordedWavTest, OnlyWholeSamples) {
  const std::string format = Chunk("fmt ", FormatBody(Format()));
  EXPECT_EQ(Parse(Riff(format + Chunk("data", ""))), RecordedWavError::kEmpty);
  EXPECT_EQ(Parse(Riff(format + Chunk("data", "x"))), RecordedWavError::kEmpty);
  std::string_view samples;
  const std::string wav = Riff(format + Chunk("data", "abc"));
  ASSERT_EQ(ParseRecordedWav(wav, &samples), RecordedWavError::kNone);
  EXPECT_EQ(samples, "ab");
}

TEST(RecordedWavTest, SizeLimit) {
  const std::string format = Chunk("fmt ", FormatBody(Format()));
  const std::string largest =
      Riff(format + Chunk("data", std::string(kMaxRecordedWavBytes - 44, 'x')));
  ASSERT_EQ(largest.size(), kMaxRecordedWavBytes);
  std::string_view samples;
  EXPECT_EQ(ParseRecordedWav(largest, &samples), RecordedWavError::kNone);
  EXPECT_EQ(CanonicalWav(samples).size(), kMaxRecordedWavBytes);
  // One sample more, or anything that long.
  EXPECT_EQ(
      Parse(Riff(format +
                 Chunk("data", std::string(kMaxRecordedWavBytes - 42, 'x')))),
      RecordedWavError::kTooLarge);
  EXPECT_EQ(Parse(std::string(kMaxRecordedWavBytes + 1, 'R')),
            RecordedWavError::kTooLarge);
}

TEST(RecordedWavTest, FailuresLeaveNoSamples) {
  std::string_view samples = "stale";
  EXPECT_EQ(ParseRecordedWav("nonsense", &samples), RecordedWavError::kNotWav);
  EXPECT_TRUE(samples.empty());
}

TEST(RecordedWavTest, DescribesEveryError) {
  EXPECT_TRUE(DescribeRecordedWavError(RecordedWavError::kNone).empty());
  for (RecordedWavError error :
       {RecordedWavError::kTooLarge, RecordedWavError::kNotWav,
        RecordedWavError::kNoFormat, RecordedWavError::kWrongFormat,
        RecordedWavError::kNoData, RecordedWavError::kTruncated,
        RecordedWavError::kEmpty}) {
    EXPECT_FALSE(DescribeRecordedWavError(error).empty());
  }
  EXPECT_EQ(DescribeRecordedWavError(RecordedWavError::kWrongFormat),
            "the recording isn't 16 kHz mono 16-bit PCM");
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
