// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/services/molt_ai/inference/engine/audio_input.h"

#include <algorithm>
#include <cmath>

namespace molt_ai {

static_assert(kMinAudioSamples >= 201,
              "llama.cpp's Whisper preprocessing reflects 200 samples");
static_assert(kMaxAudioSamples <= static_cast<size_t>(INT32_MAX),
              "llama.cpp counts samples in an int");

AudioCheck CheckModelAudio(size_t byte_length) {
  if (byte_length % kAudioBytesPerSample != 0) {
    return AudioCheck::kBadByteLength;
  }
  const size_t samples = byte_length / kAudioBytesPerSample;
  if (samples < kMinAudioSamples) return AudioCheck::kTooShort;
  if (samples > kMaxAudioSamples) return AudioCheck::kTooLong;
  return AudioCheck::kOk;
}

std::optional<std::vector<int16_t>> PrepareModelAudio(int64_t channel_count,
                                                      int64_t sample_rate,
                                                      int64_t frame_count,
                                                      const float* samples,
                                                      size_t sample_count) {
  if (channel_count != 1 || sample_rate != kAudioSampleRate ||
      frame_count < 1 ||
      static_cast<uint64_t>(frame_count) > kMaxAudioSamples ||
      static_cast<uint64_t>(frame_count) != sample_count || !samples) {
    return std::nullopt;
  }
  std::vector<int16_t> out(std::max(sample_count, kMinAudioSamples), 0);
  for (size_t i = 0; i < sample_count; ++i) {
    float s = samples[i];
    if (!std::isfinite(s)) {
      s = 0.0f;
    } else if (s > 1.0f) {
      s = 1.0f;
    } else if (s < -1.0f) {
      s = -1.0f;
    }
    out[i] = static_cast<int16_t>(std::lrint(s * 32767.0f));
  }
  return out;
}

namespace {

struct AllowedAudioProjector {
  std::string_view type;  // llama.cpp's projector type name, exactly
  uint32_t stack_factor;  // clip.audio.projector.stack_factor, exactly
  uint32_t window_tokens;
};

// Voxtral: a Whisper encoder's 1500 frames per window stacked four by four
// (llama.cpp's converter writes a factor of 4), so 375 tokens; checked end
// to end with the local clip.cpp patch (README.chromium).
constexpr AllowedAudioProjector kAllowedAudioProjectors[] = {
    {"voxtral", 4, 375},
};

}  // namespace

std::optional<uint32_t> AllowedAudioWindowTokens(
    std::string_view projector_type,
    uint32_t stack_factor) {
  for (const AllowedAudioProjector& p : kAllowedAudioProjectors) {
    if (p.type == projector_type && p.stack_factor == stack_factor) {
      return p.window_tokens;
    }
  }
  return std::nullopt;
}

std::vector<float> AudioSamplesToFloat(const int16_t* samples, size_t count) {
  std::vector<float> out(count);
  for (size_t i = 0; i < count; ++i) {
    out[i] = static_cast<float>(samples[i]) / 32768.0f;
  }
  return out;
}

}  // namespace molt_ai
