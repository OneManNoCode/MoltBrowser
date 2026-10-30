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

#include <cstddef>
#include <cstdint>
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
