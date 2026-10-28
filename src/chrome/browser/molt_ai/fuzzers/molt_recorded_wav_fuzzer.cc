// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The voice recording the AI chat page sends (voice/recorded_wav.h): any
// bytes, since the page is a WebUI renderer a compromise could control.
// The fuzzer either feeds the bytes as they are, or builds a RIFF file from
// them (chunk ids, sizes and format fields drawn from the input) to reach
// the deeper checks more often.
//
// Checks: ParseRecordedWav() reads only the input (ASan) and gives the same
// verdict every time; an accepted file is at most kMaxRecordedWavBytes, has
// a 16 kHz mono 16-bit PCM format chunk before its data (checked here with
// an independent walk), and its samples are a non-empty, even-sized part of
// the input; CanonicalWav() of them is the fixed 44-byte header followed by
// exactly those samples, parses back to the same samples, and is never
// larger than the limit; a refused file leaves no samples and has a
// description.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <fuzzer/FuzzedDataProvider.h>

#include "base/check.h"
#include "chrome/browser/molt_ai/voice/recorded_wav.h"

namespace {

using molt_ai::voice::CanonicalWav;
using molt_ai::voice::kCanonicalWavHeaderBytes;
using molt_ai::voice::kMaxRecordedWavBytes;
using molt_ai::voice::ParseRecordedWav;
using molt_ai::voice::RecordedWavError;

// Where |part| (a view into |whole|) starts in |whole|; npos if it isn't
// inside it.
size_t OffsetIn(std::string_view whole, std::string_view part) {
  const uintptr_t start = reinterpret_cast<uintptr_t>(whole.data());
  const uintptr_t at = reinterpret_cast<uintptr_t>(part.data());
  if (at < start || at - start > whole.size() ||
      part.size() > whole.size() - (at - start)) {
    return std::string_view::npos;
  }
  return at - start;
}

uint32_t Le32(std::string_view bytes, size_t at) {
  uint32_t value = 0;
  for (size_t i = 0; i < 4; ++i) {
    value |= static_cast<uint32_t>(static_cast<uint8_t>(bytes[at + i]))
             << (8 * i);
  }
  return value;
}

uint16_t Le16(std::string_view bytes, size_t at) {
  return static_cast<uint16_t>(static_cast<uint8_t>(bytes[at]) |
                               static_cast<uint8_t>(bytes[at + 1]) << 8);
}

void PutLe32(std::string* out, uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out->push_back(static_cast<char>((value >> shift) & 0xff));
  }
}

void PutLe16(std::string* out, uint16_t value) {
  out->push_back(static_cast<char>(value & 0xff));
  out->push_back(static_cast<char>(value >> 8));
}

// A RIFF file built from the input: a few chunks with ids, sizes and
// bodies chosen by the fuzzer, often a well-formed format chunk.
std::string BuildRiff(FuzzedDataProvider& p) {
  static constexpr std::array<std::string_view, 7> kIds = {
      "fmt ", "data", "LIST", "fact", "junk", std::string_view("fmt\0", 4),
      "DATA"};
  std::string chunks;
  const size_t count = p.ConsumeIntegralInRange<size_t>(0, 5);
  for (size_t i = 0; i < count && chunks.size() < 65536; ++i) {
    const std::string_view id =
        kIds[p.ConsumeIntegralInRange<size_t>(0, kIds.size() - 1)];
    std::string body;
    if (id == "fmt " && p.ConsumeBool()) {
      PutLe16(&body, p.ConsumeBool() ? 1 : p.ConsumeIntegral<uint16_t>());
      PutLe16(&body, p.ConsumeBool() ? 1 : p.ConsumeIntegral<uint16_t>());
      PutLe32(&body, p.ConsumeBool() ? 16000 : p.ConsumeIntegral<uint32_t>());
      PutLe32(&body, p.ConsumeBool() ? 32000 : p.ConsumeIntegral<uint32_t>());
      PutLe16(&body, p.ConsumeBool() ? 2 : p.ConsumeIntegral<uint16_t>());
      PutLe16(&body, p.ConsumeBool() ? 16 : p.ConsumeIntegral<uint16_t>());
      body += p.ConsumeRandomLengthString(8);
    } else {
      body = p.ConsumeRandomLengthString(4096);
    }
    chunks += id;
    // Usually the true size; sometimes one that lies.
    PutLe32(&chunks, p.ConsumeBool() ? static_cast<uint32_t>(body.size())
                                     : p.ConsumeIntegral<uint32_t>());
    chunks += body;
    if (body.size() % 2 && p.ConsumeBool()) {
      chunks.push_back('\0');
    }
  }
  std::string wav = "RIFF";
  PutLe32(&wav, p.ConsumeBool() ? static_cast<uint32_t>(4 + chunks.size())
                                : p.ConsumeIntegral<uint32_t>());
  wav += "WAVE";
  wav += chunks;
  return wav;
}

// An independent walk: whether |wav| has a recorded-format "fmt " chunk
// before the chunk whose body starts at |data_at|.
bool FormatPrecedes(std::string_view wav, size_t data_at) {
  size_t at = 12;
  while (at + 8 <= data_at - 8) {
    const uint32_t size = Le32(wav, at + 4);
    if (wav.substr(at, 4) == "fmt ") {
      const size_t body = at + 8;
      return size >= 16 && Le16(wav, body) == 1 && Le16(wav, body + 2) == 1 &&
             Le32(wav, body + 4) == 16000 && Le32(wav, body + 8) == 32000 &&
             Le16(wav, body + 12) == 2 && Le16(wav, body + 14) == 16;
    }
    at += 8 + size + (size & 1);
  }
  return false;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  FuzzedDataProvider p(data, size);
  const std::string wav =
      p.ConsumeBool() ? BuildRiff(p) : p.ConsumeRemainingBytesAsString();

  std::string_view samples = "unset";
  const RecordedWavError error = ParseRecordedWav(wav, &samples);
  std::string_view again;
  CHECK(ParseRecordedWav(wav, &again) == error);
  CHECK(again.data() == samples.data() && again.size() == samples.size());

  if (error != RecordedWavError::kNone) {
    CHECK(samples.empty());
    CHECK(!molt_ai::voice::DescribeRecordedWavError(error).empty());
    return 0;
  }

  // The samples are a non-empty, even-sized part of the input, after a
  // format chunk the page could have written.
  CHECK(wav.size() <= kMaxRecordedWavBytes);
  CHECK(!samples.empty() && samples.size() % 2 == 0);
  const size_t data_at = OffsetIn(wav, samples);
  CHECK(data_at != std::string_view::npos);
  CHECK(data_at >= kCanonicalWavHeaderBytes);
  CHECK(std::string_view(wav).substr(data_at - 8, 4) == "data");
  CHECK(Le32(wav, data_at - 4) / 2 * 2 == samples.size());
  CHECK(FormatPrecedes(wav, data_at));

  // What whisper-cli reads.
  const std::string canonical = CanonicalWav(samples);
  CHECK(canonical.size() == kCanonicalWavHeaderBytes + samples.size());
  CHECK(canonical.size() <= kMaxRecordedWavBytes);
  static constexpr std::string_view kHeader(
      "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x01\0\x80\x3e\0\0\0\x7d\0\0\x02\0"
      "\x10\0data",
      40);
  const std::string_view header = std::string_view(canonical).substr(0, 40);
  CHECK(header.substr(0, 4) == kHeader.substr(0, 4));
  CHECK(header.substr(8) == kHeader.substr(8));
  CHECK(Le32(canonical, 4) == 36 + samples.size());
  CHECK(Le32(canonical, 40) == samples.size());
  CHECK(std::string_view(canonical).substr(kCanonicalWavHeaderBytes) ==
        samples);
  std::string_view reparsed;
  CHECK(ParseRecordedWav(canonical, &reparsed) == RecordedWavError::kNone);
  CHECK(reparsed == samples);
  return 0;
}
