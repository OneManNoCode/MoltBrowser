// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/recorded_wav.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace molt_ai {
namespace voice {

namespace {

constexpr uint16_t kWaveFormatPcm = 1;
// RIFF header: "RIFF", the size of what follows, "WAVE".
constexpr size_t kRiffHeaderBytes = 12;
// Each chunk: a four-character id and the size of its body.
constexpr size_t kChunkHeaderBytes = 8;
// The PCM fields of a format chunk; longer ones carry extra fields.
constexpr size_t kPcmFormatBytes = 16;
constexpr uint16_t kBlockAlign =
    kRecordedWavChannels * kRecordedWavBitsPerSample / 8;

static_assert(kCanonicalWavHeaderBytes ==
              kRiffHeaderBytes + kChunkHeaderBytes + kPcmFormatBytes +
                  kChunkHeaderBytes);

uint16_t ReadLe16(std::string_view bytes, size_t at) {
  return static_cast<uint16_t>(static_cast<uint8_t>(bytes[at]) |
                               static_cast<uint8_t>(bytes[at + 1]) << 8);
}

uint32_t ReadLe32(std::string_view bytes, size_t at) {
  return static_cast<uint32_t>(static_cast<uint8_t>(bytes[at])) |
         static_cast<uint32_t>(static_cast<uint8_t>(bytes[at + 1])) << 8 |
         static_cast<uint32_t>(static_cast<uint8_t>(bytes[at + 2])) << 16 |
         static_cast<uint32_t>(static_cast<uint8_t>(bytes[at + 3])) << 24;
}

void AppendLe16(std::string* out, uint16_t value) {
  out->push_back(static_cast<char>(value & 0xff));
  out->push_back(static_cast<char>(value >> 8));
}

void AppendLe32(std::string* out, uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out->push_back(static_cast<char>((value >> shift) & 0xff));
  }
}

// Whether the format chunk's body |format| (at least kPcmFormatBytes) is
// the one the page records.
bool IsRecordedFormat(std::string_view format) {
  return ReadLe16(format, 0) == kWaveFormatPcm &&
         ReadLe16(format, 2) == kRecordedWavChannels &&
         ReadLe32(format, 4) == kRecordedWavSampleRate &&
         ReadLe32(format, 8) == kRecordedWavBytesPerSecond &&
         ReadLe16(format, 12) == kBlockAlign &&
         ReadLe16(format, 14) == kRecordedWavBitsPerSample;
}

}  // namespace

RecordedWavError ParseRecordedWav(std::string_view wav,
                                  std::string_view* samples) {
  *samples = std::string_view();
  if (wav.size() > kMaxRecordedWavBytes) {
    return RecordedWavError::kTooLarge;
  }
  if (wav.size() < kRiffHeaderBytes || wav.substr(0, 4) != "RIFF" ||
      wav.substr(8, 4) != "WAVE") {
    return RecordedWavError::kNotWav;
  }
  // The chunks are what the RIFF header claims, within what was sent.
  const uint32_t riff_size = ReadLe32(wav, 4);
  const size_t end =
      riff_size > wav.size() - 8 ? wav.size() : size_t{8} + riff_size;

  bool have_format = false;
  size_t at = kRiffHeaderBytes;
  while (end >= kChunkHeaderBytes && at <= end - kChunkHeaderBytes) {
    const std::string_view id = wav.substr(at, 4);
    const uint32_t size = ReadLe32(wav, at + 4);
    const size_t body = at + kChunkHeaderBytes;
    if (size > end - body) {
      return RecordedWavError::kTruncated;
    }
    if (id == "fmt " && !have_format) {
      if (size < kPcmFormatBytes) {
        return RecordedWavError::kNoFormat;
      }
      if (!IsRecordedFormat(wav.substr(body, kPcmFormatBytes))) {
        return RecordedWavError::kWrongFormat;
      }
      have_format = true;
    } else if (id == "data") {
      if (!have_format) {
        return RecordedWavError::kNoFormat;
      }
      const size_t whole_samples = size - size % kBlockAlign;
      if (whole_samples == 0) {
        return RecordedWavError::kEmpty;
      }
      *samples = wav.substr(body, whole_samples);
      return RecordedWavError::kNone;
    }
    // A chunk's body is padded to an even size. Past |end| the loop ends.
    at = body + size + (size & 1);
  }
  return RecordedWavError::kNoData;
}

std::string CanonicalWav(std::string_view samples) {
  if (samples.size() % kBlockAlign != 0 ||
      samples.size() > kMaxRecordedWavBytes - kCanonicalWavHeaderBytes) {
    return std::string();
  }
  const uint32_t data_bytes = static_cast<uint32_t>(samples.size());
  std::string wav;
  wav.reserve(kCanonicalWavHeaderBytes + samples.size());
  wav += "RIFF";
  AppendLe32(&wav, static_cast<uint32_t>(kCanonicalWavHeaderBytes - 8) +
                       data_bytes);
  wav += "WAVE";
  wav += "fmt ";
  AppendLe32(&wav, static_cast<uint32_t>(kPcmFormatBytes));
  AppendLe16(&wav, kWaveFormatPcm);
  AppendLe16(&wav, kRecordedWavChannels);
  AppendLe32(&wav, kRecordedWavSampleRate);
  AppendLe32(&wav, kRecordedWavBytesPerSecond);
  AppendLe16(&wav, kBlockAlign);
  AppendLe16(&wav, kRecordedWavBitsPerSample);
  wav += "data";
  AppendLe32(&wav, data_bytes);
  wav += samples;
  return wav;
}

std::string_view DescribeRecordedWavError(RecordedWavError error) {
  switch (error) {
    case RecordedWavError::kNone:
      return std::string_view();
    case RecordedWavError::kTooLarge:
      return "the recording is too long";
    case RecordedWavError::kNotWav:
      return "the recording isn't a WAV file";
    case RecordedWavError::kNoFormat:
      return "the recording has no audio format";
    case RecordedWavError::kWrongFormat:
      return "the recording isn't 16 kHz mono 16-bit PCM";
    case RecordedWavError::kNoData:
      return "the recording has no audio data";
    case RecordedWavError::kTruncated:
      return "the recording is cut short";
    case RecordedWavError::kEmpty:
      return "the recording is empty";
  }
  return "the recording can't be read";
}

}  // namespace voice
}  // namespace molt_ai
