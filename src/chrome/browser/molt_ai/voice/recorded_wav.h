// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The recording the AI chat page sends for voice input, checked in the
// browser before whisper-cli sees any of it.
//
// The page's recorder (_wavBytesFromPCM() in molt_ai_chat_ui_js.h) writes a
// 44-byte WAV header and 16 kHz mono 16-bit PCM. But the page is a WebUI
// renderer, which a compromise could make send any bytes, and whisper-cli
// reads its input with a general audio decoder (miniaudio: WAV with any of
// its encodings, FLAC and MP3, picked by what the bytes look like). So the
// browser never hands it what the page sent:
//
//   - ParseRecordedWav() accepts only a RIFF/WAVE file of at most
//     kMaxRecordedWavBytes whose format chunk says PCM, one channel,
//     16000 Hz, 16 bits (with the byte rate and block alignment that
//     implies), followed by a data chunk that fits in the file. Other
//     chunks are skipped. It hands back the samples' bytes.
//   - CanonicalWav() writes the 44-byte header MoltBrowser defines and
//     those samples. That file is all whisper-cli reads.
//
// Std-only (no //base), so the unit tests, a standalone harness and
// molt_recorded_wav_fuzzer run exactly this code.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_RECORDED_WAV_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_RECORDED_WAV_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace molt_ai {
namespace voice {

// What whisper.cpp's models take, and what the page records.
inline constexpr uint32_t kRecordedWavSampleRate = 16000;
inline constexpr uint16_t kRecordedWavChannels = 1;
inline constexpr uint16_t kRecordedWavBitsPerSample = 16;
inline constexpr uint32_t kRecordedWavBytesPerSecond =
    kRecordedWavSampleRate * kRecordedWavChannels * kRecordedWavBitsPerSample /
    8;

// The canonical header: RIFF, WAVE, a 16-byte PCM format chunk, the data
// chunk's header.
inline constexpr size_t kCanonicalWavHeaderBytes = 44;

// The most the page may send: what HandleTranscribeAudio()'s 8 MiB of
// base64 decodes to (about 196 s of audio; the page stops recording at
// 30 s).
inline constexpr size_t kMaxRecordedWavBytes = 6u << 20;

enum class RecordedWavError {
  kNone,
  // More than kMaxRecordedWavBytes.
  kTooLarge,
  // No RIFF/WAVE header.
  kNotWav,
  // No format chunk before the data chunk, or one shorter than 16 bytes.
  kNoFormat,
  // Not 16 kHz mono 16-bit PCM.
  kWrongFormat,
  // No data chunk.
  kNoData,
  // A chunk runs past the end of the file.
  kTruncated,
  // The data chunk holds no whole sample.
  kEmpty,
};

// Checks |wav|, the bytes the page sent. On success sets |*samples| to the
// data chunk's whole 16-bit samples (little-endian; a trailing odd byte is
// left out), a part of |wav|. Otherwise leaves |*samples| empty.
RecordedWavError ParseRecordedWav(std::string_view wav,
                                  std::string_view* samples);

// The file whisper-cli reads: MoltBrowser's 44-byte header for 16 kHz mono
// 16-bit PCM, then |samples|. |samples| is what ParseRecordedWav() returned
// (an even number of bytes, at most kMaxRecordedWavBytes minus the header);
// anything else gives an empty string.
std::string CanonicalWav(std::string_view samples);

// For the chat page, e.g. "the recording isn't 16 kHz mono 16-bit PCM".
// Empty for kNone.
std::string_view DescribeRecordedWavError(RecordedWavError error);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_RECORDED_WAV_H_
