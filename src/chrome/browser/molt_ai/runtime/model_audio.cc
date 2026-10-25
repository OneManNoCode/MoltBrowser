// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/runtime/model_audio.h"

#include <algorithm>
#include <utility>

#include "chrome/services/molt_ai/inference/engine/audio_input.h"

namespace molt_ai {

ModelAudio::ModelAudio() = default;
ModelAudio::ModelAudio(std::vector<int16_t> pcm) : samples(std::move(pcm)) {}
ModelAudio::~ModelAudio() = default;
ModelAudio::ModelAudio(ModelAudio&&) = default;
ModelAudio& ModelAudio::operator=(ModelAudio&&) = default;
ModelAudio::ModelAudio(const ModelAudio&) = default;
ModelAudio& ModelAudio::operator=(const ModelAudio&) = default;

bool ModelAudio::IsValid() const {
  return CheckModelAudio(samples.size() * kAudioBytesPerSample) ==
         AudioCheck::kOk;
}

uint32_t ModelAudio::Seconds() const {
  return static_cast<uint32_t>((samples.size() + kAudioSampleRate - 1) /
                               kAudioSampleRate);
}

std::optional<ModelAudio> ModelAudioFromPage(int64_t channel_count,
                                             int64_t sample_rate,
                                             int64_t frame_count,
                                             const float* samples,
                                             size_t sample_count) {
  std::optional<std::vector<int16_t>> pcm = PrepareModelAudio(
      channel_count, sample_rate, frame_count, samples, sample_count);
  if (!pcm) {
    return std::nullopt;
  }
  return ModelAudio(std::move(*pcm));
}

std::optional<std::vector<size_t>> PlanRequestAudio(
    size_t user_audio,
    const std::vector<size_t>& turn_audio) {
  if (user_audio > kMaxAudioPerRequest) {
    return std::nullopt;
  }
  std::vector<size_t> sent(turn_audio.size(), 0);
  size_t room = kMaxAudioPerRequest - user_audio;
  for (size_t i = turn_audio.size(); i-- > 0 && room > 0;) {
    sent[i] = std::min(room, turn_audio[i]);
    room -= sent[i];
  }
  return sent;
}

}  // namespace molt_ai
