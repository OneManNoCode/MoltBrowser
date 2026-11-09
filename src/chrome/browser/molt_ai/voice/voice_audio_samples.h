// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Voice mode's audio as numbers (docs/design/TOUCHLESS_BROWSING.md §5.1,
// §6.1): the microphone's samples cut into the helper's AUDIO frames, and
// the earcons, synthesized here so no sound file ships. voice_audio.h
// moves them through Chromium's audio service: the microphone in, the
// earcons out (where the echo canceller hears them as the browser's own
// sound).
//
// Std-only, unit-tested.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_AUDIO_SAMPLES_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_AUDIO_SAMPLES_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "chrome/browser/molt_ai/voice/voice_session_logic.h"

namespace molt_ai {
namespace voice {

// The helper's audio frames: 50 ms of 16 kHz mono (an utterance's end is
// heard at most this much later).
inline constexpr size_t kAudioFrameSamples = 800;

// A float sample (-1..1, as media::AudioBus holds them) as signed 16-bit:
// clamped, rounded; NaN is silence.
int16_t FloatToS16(float sample);

// Collects samples into frames of exactly |frame_samples|.
class AudioFrameChunker {
 public:
  explicit AudioFrameChunker(size_t frame_samples = kAudioFrameSamples);
  ~AudioFrameChunker();
  AudioFrameChunker(const AudioFrameChunker&) = delete;
  AudioFrameChunker& operator=(const AudioFrameChunker&) = delete;

  // Takes |samples| and appends each frame they complete to |frames|.
  void Add(std::span<const float> samples,
           std::vector<std::vector<int16_t>>* frames);
  // Drops a partial frame (the stream restarted).
  void Reset();

 private:
  const size_t frame_samples_;
  std::vector<int16_t> pending_;
};

// How loud the microphone was lately, in slots of kLevelSlotUs of capture
// time: the loudest sample (0 to 1) of each, for the last kLevelSlots
// slots. One thread notes (the capture thread), any reads: whether what
// was heard over the browser's own voice stood out of what the echo
// canceller leaves of it (voice_session_logic.h).
inline constexpr int64_t kLevelSlotUs = 50'000;
inline constexpr size_t kLevelSlots = 256; // 12.8 s

class MicrophoneLevelHistory {
public:
  MicrophoneLevelHistory();
  ~MicrophoneLevelHistory();
  MicrophoneLevelHistory(const MicrophoneLevelHistory &) = delete;
  MicrophoneLevelHistory &operator=(const MicrophoneLevelHistory &) = delete;

  // A buffer captured from |start_us| for |duration_us| (microseconds of
  // the capture clock, from 0), whose loudest sample is |peak|. Not for a
  // time before the last one noted: the history is the capture's, in order.
  void Note(int64_t start_us, int64_t duration_us, float peak);
  // The loudest sample captured from |from_us| to |to_us|; nullopt when
  // nothing captured in that span is still held (older than kLevelSlots
  // slots before the last noted, or never captured), or the span is empty.
  std::optional<float> LoudestBetween(int64_t from_us, int64_t to_us) const;

private:
  // Each slot: its number (start / kLevelSlotUs) above, its loudest
  // sample in millionths in the low kPeakBits.
  static constexpr int kPeakBits = 24;
  std::array<std::atomic<uint64_t>, kLevelSlots> slots_;
  std::atomic<int64_t> latest_slot_{-1};
};

// The earcons' sample rate (mono).
inline constexpr int kEarconSampleRate = 24000;
// No earcon is longer, nor louder.
inline constexpr size_t kMaxEarconSamples = kEarconSampleRate * 3 / 10;
inline constexpr int16_t kMaxEarconLevel = 32767 * 3 / 10;

// The earcon of |kind| (§5.1), at kEarconSampleRate: short tones with
// smooth edges, each kind its own shape:
//   off        three notes falling (G5 E5 C5)
//   waiting    two notes falling (E5 C5), soft
//   listening  two notes rising (C5 G5)
//   thinking   one short tick (A5)
//   speaking   one very short, quiet blip (E6)
std::vector<int16_t> MakeEarcon(EarconKind kind);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_AUDIO_SAMPLES_H_
