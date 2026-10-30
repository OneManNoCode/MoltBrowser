// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_helper/standalone/sherpa_voice.h"

#include <string.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "sherpa-onnx/c-api/c-api.h"

namespace molt_voice {

namespace {

// The voice's own inference settings (en_US-ljspeech-medium.onnx.json).
constexpr float kNoiseScale = 0.667f;
constexpr float kNoiseScaleW = 0.333f;

std::string Join(const std::string& dir, const char* name) {
  if (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) {
    return dir + name;
  }
  return dir + "/" + name;
}

struct CallbackState {
  const VoiceEngine::AudioCallback* on_audio = nullptr;
  bool stopped = false;
};

int32_t OnAudio(const float* samples, int32_t n, float progress, void* arg) {
  (void)progress;
  auto* state = static_cast<CallbackState*>(arg);
  if (state->stopped || n <= 0) {
    return state->stopped ? 0 : 1;
  }
  if (!(*state->on_audio)(samples, static_cast<size_t>(n))) {
    state->stopped = true;
    return 0;
  }
  return 1;
}

}  // namespace

VoiceModelPaths VoiceModelPathsIn(const std::string& dir) {
  VoiceModelPaths paths;
  paths.model = Join(dir, "en_US-ljspeech-medium.onnx");
  paths.tokens = Join(dir, "tokens.txt");
  paths.lexicon = Join(dir, "lexicon-en-us.txt");
  paths.model_card = Join(dir, "MODEL_CARD");
  return paths;
}

const char* VoiceName() {
  return "piper-en_US-ljspeech-medium (sherpa-onnx 2.0.1, onnxruntime 1.28.2)";
}

// static
std::unique_ptr<SherpaVoice> SherpaVoice::Create(const VoiceModelPaths& paths,
                                                 int threads,
                                                 std::string* error) {
  SherpaOnnxOfflineTtsConfig config;
  memset(&config, 0, sizeof(config));
  config.model.vits.model = paths.model.c_str();
  config.model.vits.tokens = paths.tokens.c_str();
  // No lexicon: the helper hands over phonemes (voice_phonemes.h).
  config.model.vits.noise_scale = kNoiseScale;
  config.model.vits.noise_scale_w = kNoiseScaleW;
  config.model.vits.length_scale = 1.0f;
  config.model.num_threads = threads;
  config.model.provider = "cpu";
  config.max_num_sentences = 1;
  const SherpaOnnxOfflineTts* tts = SherpaOnnxCreateOfflineTts(&config);
  if (!tts) {
    *error = "the voice model didn't load";
    return nullptr;
  }
  std::unique_ptr<SherpaVoice> voice(new SherpaVoice(tts));
  voice->sample_rate_ = SherpaOnnxOfflineTtsSampleRate(tts);
  if (voice->sample_rate_ < kMinSampleRate ||
      voice->sample_rate_ > kMaxSampleRate) {
    *error = "the voice has an unexpected sample rate";
    return nullptr;
  }
  return voice;
}

SherpaVoice::SherpaVoice(const SherpaOnnxOfflineTts* tts) : tts_(tts) {}

SherpaVoice::~SherpaVoice() {
  SherpaOnnxDestroyOfflineTts(tts_);
}

int SherpaVoice::sample_rate() const {
  return sample_rate_;
}

bool SherpaVoice::Synthesize(const std::vector<int32_t>& phonemes,
                             float length_scale,
                             const AudioCallback& on_audio) {
  if (phonemes.empty()) {
    return true;
  }
  const int32_t length = static_cast<int32_t>(phonemes.size());
  SherpaOnnxGenerationConfig generation;
  memset(&generation, 0, sizeof(generation));
  generation.silence_scale = 0.2f;
  generation.speed = length_scale > 0 ? 1.0f / length_scale : 1.0f;
  generation.sid = 0;
  generation.phoneme_codepoints = phonemes.data();
  generation.phoneme_codepoints_lens = &length;
  generation.phoneme_codepoints_num_sentences = 1;
  CallbackState state;
  state.on_audio = &on_audio;
  // The text is not read when phonemes are given.
  const SherpaOnnxGeneratedAudio* audio =
      SherpaOnnxOfflineTtsGenerateWithConfig(tts_, "", &generation, OnAudio,
                                             &state);
  if (!audio) {
    return state.stopped;
  }
  SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
  return true;
}

}  // namespace molt_voice
