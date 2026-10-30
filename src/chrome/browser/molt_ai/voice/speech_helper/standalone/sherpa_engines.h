// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// molt-speech's engines over sherpa-onnx's C API (sherpa-onnx 2.0.1, ONNX
// Runtime 1.28.2): the GigaSpeech keyword spotter, Silero VAD and the Kroko
// streaming recognizer, configured as docs/VOICE_BENCH.md §6 recommends.
// Built only by scripts/bundle-speech.sh (CMake), never by GN.
//
// sherpa-onnx and ONNX Runtime are built with exceptions and may throw
// through the C API; every call here catches everything and reports it as
// a fatal engine error (the helper then writes an "engine" error line and
// exits), so nothing escapes into the listener.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_SPEECH_HELPER_STANDALONE_SHERPA_ENGINES_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_SPEECH_HELPER_STANDALONE_SHERPA_ENGINES_H_

#include <stdint.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "chrome/browser/molt_ai/voice/speech_helper/listener.h"

struct SherpaOnnxKeywordSpotter;
struct SherpaOnnxOnlineRecognizer;
struct SherpaOnnxOnlineStream;
struct SherpaOnnxVoiceActivityDetector;

namespace molt_speech {

// The model files under --models (scripts/bundle-speech.sh installs them).
struct ModelPaths {
  std::string kws_encoder, kws_decoder, kws_joiner, kws_tokens, kws_pieces;
  std::string vad;
  std::string asr_encoder, asr_decoder, asr_joiner, asr_tokens, asr_vocab;
};
ModelPaths ModelPathsIn(const std::string& models_dir);

// The names `ready` reports.
std::vector<std::string> ModelNames();
// "sherpa-onnx 2.0.1, onnxruntime 1.28.2", from the libraries themselves.
std::string EngineDescription();

class SherpaEngines : public SpeechEngines {
 public:
  // Called once on an engine failure, with what failed; it must not
  // return (the helper writes an error line and exits).
  using FatalFunction = std::function<void(const std::string& message)>;

  // Loads the recognizer (the spotter and the VAD are built by
  // SetKeywords() and SetVadSilenceMs(), which Listener::Start() calls).
  // Null with |error| if it couldn't.
  static std::unique_ptr<SherpaEngines> Create(const ModelPaths& paths,
                                               int asr_threads,
                                               FatalFunction fatal,
                                               std::string* error);

  ~SherpaEngines() override;

  bool SetKeywords(const std::vector<std::string>& keyword_lines) override;
  bool Spot(const std::vector<float>& samples, size_t* words) override;
  void ResetSpotter() override;
  void SetVadSilenceMs(int ms) override;
  VadUpdate Detect(const std::vector<float>& samples) override;
  void ResetVad() override;
  void BeginUtterance(const std::string& hotwords) override;
  void Recognize(const std::vector<float>& samples) override;
  RecognizerResult Partial() override;
  RecognizerResult Finish() override;
  void AbandonUtterance() override;

 private:
  SherpaEngines(const ModelPaths& paths, FatalFunction fatal);

  [[noreturn]] void Fail(const std::string& what);
  RecognizerResult Result(bool final_result);
  void DestroySpotter();
  void DestroyVad();

  ModelPaths paths_;
  FatalFunction fatal_;

  const SherpaOnnxKeywordSpotter* kws_ = nullptr;
  const SherpaOnnxOnlineStream* kws_stream_ = nullptr;
  std::string keywords_;

  const SherpaOnnxVoiceActivityDetector* vad_ = nullptr;
  // Samples waiting for a whole 512-sample Silero window.
  std::vector<float> vad_pending_;
  // How long the VAD has been hearing speech without a pause.
  int64_t vad_speech_samples_ = 0;
  // How long it has heard no speech since it last started fresh.
  int64_t vad_quiet_samples_ = 0;

  const SherpaOnnxOnlineRecognizer* asr_ = nullptr;
  const SherpaOnnxOnlineStream* asr_stream_ = nullptr;
};

}  // namespace molt_speech

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_SPEECH_HELPER_STANDALONE_SHERPA_ENGINES_H_
