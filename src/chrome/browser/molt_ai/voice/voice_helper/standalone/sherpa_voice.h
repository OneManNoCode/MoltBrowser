// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// molt-voice's engine: a Piper (VITS) voice through sherpa-onnx's C API,
// fed phonemes the helper made itself (GenerationConfig's
// phoneme_codepoints), never text: sherpa-onnx 2.x's English front end
// would skip the words its lexicon lacks. Built only by
// scripts/bundle-speech.sh (CMake).

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_HELPER_STANDALONE_SHERPA_VOICE_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_HELPER_STANDALONE_SHERPA_VOICE_H_

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "chrome/browser/molt_ai/voice/voice_helper/voice_synth.h"

struct SherpaOnnxOfflineTts;

namespace molt_voice {

// The files in the model directory (VOICE_HELPER_PROTOCOL.md §7.2).
struct VoiceModelPaths {
  std::string model;       // en_US-ljspeech-medium.onnx
  std::string tokens;      // tokens.txt
  std::string lexicon;     // lexicon-en-us.txt
  std::string model_card;  // MODEL_CARD
};
VoiceModelPaths VoiceModelPathsIn(const std::string& dir);

// READY's voice name.
const char* VoiceName();

class SherpaVoice : public VoiceEngine {
 public:
  // Null, with |error|, if the model didn't load.
  static std::unique_ptr<SherpaVoice> Create(const VoiceModelPaths& paths,
                                             int threads,
                                             std::string* error);
  SherpaVoice(const SherpaVoice&) = delete;
  SherpaVoice& operator=(const SherpaVoice&) = delete;
  ~SherpaVoice() override;

  int sample_rate() const override;
  bool Synthesize(const std::vector<int32_t>& phonemes,
                  float length_scale,
                  const AudioCallback& on_audio) override;

 private:
  explicit SherpaVoice(const SherpaOnnxOfflineTts* tts);

  const SherpaOnnxOfflineTts* tts_;
  int sample_rate_ = 0;
};

}  // namespace molt_voice

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_HELPER_STANDALONE_SHERPA_VOICE_H_
