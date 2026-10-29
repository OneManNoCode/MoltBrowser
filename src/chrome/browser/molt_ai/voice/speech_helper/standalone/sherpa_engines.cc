// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/speech_helper/standalone/sherpa_engines.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <utility>
#include <vector>

#include "sherpa-onnx/c-api/c-api.h"

namespace molt_speech {

namespace {

constexpr int kSileroWindow = 512;
// The VAD keeps every sample of the speech it is hearing and copies all of
// it for each 32 ms window, so a stretch of speech is cut at 20 s (as an
// utterance is): memory and work stay bounded under endless audio.
constexpr int64_t kMaxVadSpeechSamples = 20 * kSampleRate;
// Silence fed after the end of speech so that the recognizer's last chunk
// is complete: Kroko's chunks are 1.28 s long, and with less padding it
// drops the last word (docs/VOICE_BENCH.md §2.4).
constexpr int kTailPaddingSamples = kSampleRate * 12 / 10;
// Hotword boost (docs/VOICE_BENCH.md §3.7: no loss anywhere at 1.5).
constexpr float kHotwordsScore = 1.5f;

std::string Join(const std::string& dir, const char* name) {
  if (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) {
    return dir + name;
  }
  return dir + "/" + name;
}

// The geometric mean of the token probabilities from the result's JSON
// ("ys_probs": [log-probabilities]); 0 without tokens.
double ConfidenceFromJson(const char* json) {
  if (!json) {
    return 0;
  }
  const char* p = strstr(json, "\"ys_probs\"");
  if (!p) {
    return 0;
  }
  p = strchr(p, '[');
  if (!p) {
    return 0;
  }
  ++p;
  double sum = 0;
  int count = 0;
  while (*p && *p != ']') {
    char* end = nullptr;
    const double value = strtod(p, &end);
    if (end == p) {
      ++p;
      continue;
    }
    if (isfinite(value)) {
      sum += value;
      ++count;
    }
    p = end;
  }
  if (count == 0) {
    return 0;
  }
  const double confidence = exp(sum / count);
  return confidence > 1 ? 1 : (confidence < 0 ? 0 : confidence);
}

}  // namespace

ModelPaths ModelPathsIn(const std::string& dir) {
  ModelPaths p;
  p.kws_encoder = Join(dir, "kws/encoder.int8.onnx");
  p.kws_decoder = Join(dir, "kws/decoder.onnx");
  p.kws_joiner = Join(dir, "kws/joiner.int8.onnx");
  p.kws_tokens = Join(dir, "kws/tokens.txt");
  p.kws_pieces = Join(dir, "kws/pieces.vocab");
  p.vad = Join(dir, "vad/silero_vad.onnx");
  p.asr_encoder = Join(dir, "asr/encoder.onnx");
  p.asr_decoder = Join(dir, "asr/decoder.onnx");
  p.asr_joiner = Join(dir, "asr/joiner.onnx");
  p.asr_tokens = Join(dir, "asr/tokens.txt");
  p.asr_vocab = Join(dir, "asr/bpe.vocab");
  return p;
}

std::vector<std::string> ModelNames() {
  return {"kws-zipformer-gigaspeech-3.3m-2024-01-01", "silero-vad-v4",
          "kroko-en-2025-08-06"};
}

std::string EngineDescription() {
  std::string out = "sherpa-onnx ";
  const char* version = SherpaOnnxGetVersionStr();
  out += version ? version : "?";
  out += ", onnxruntime ";
  const char* ort = SherpaOnnxGetOnnxruntimeVersionStr();
  out += ort ? ort : "?";
  return out;
}

SherpaEngines::SherpaEngines(const ModelPaths& paths, FatalFunction fatal)
    : paths_(paths), fatal_(std::move(fatal)) {}

SherpaEngines::~SherpaEngines() {
  AbandonUtterance();
  if (asr_) {
    SherpaOnnxDestroyOnlineRecognizer(asr_);
  }
  DestroySpotter();
  DestroyVad();
}

std::unique_ptr<SherpaEngines> SherpaEngines::Create(const ModelPaths& paths,
                                                     int asr_threads,
                                                     FatalFunction fatal,
                                                     std::string* error) {
  std::unique_ptr<SherpaEngines> engines(
      new SherpaEngines(paths, std::move(fatal)));
  SherpaOnnxOnlineRecognizerConfig c;
  memset(&c, 0, sizeof(c));
  c.feat_config.sample_rate = kSampleRate;
  c.feat_config.feature_dim = 80;
  c.model_config.transducer.encoder = engines->paths_.asr_encoder.c_str();
  c.model_config.transducer.decoder = engines->paths_.asr_decoder.c_str();
  c.model_config.transducer.joiner = engines->paths_.asr_joiner.c_str();
  c.model_config.tokens = engines->paths_.asr_tokens.c_str();
  c.model_config.num_threads = asr_threads;
  c.model_config.provider = "cpu";
  // Hotwords are cut into the model's pieces with this vocabulary.
  c.model_config.modeling_unit = "bpe";
  c.model_config.bpe_vocab = engines->paths_.asr_vocab.c_str();
  // Modified beam search: what hotwords need (greedy search ignores them).
  c.decoding_method = "modified_beam_search";
  c.max_active_paths = 4;
  c.hotwords_score = kHotwordsScore;
  // The VAD ends utterances, not the recognizer.
  c.enable_endpoint = 0;
  try {
    engines->asr_ = SherpaOnnxCreateOnlineRecognizer(&c);
  } catch (...) {
    engines->asr_ = nullptr;
  }
  if (!engines->asr_) {
    *error = "the recognizer model did not load";
    return nullptr;
  }
  return engines;
}

void SherpaEngines::Fail(const std::string& what) {
  fatal_(what);
  abort();  // |fatal_| doesn't return.
}

void SherpaEngines::DestroySpotter() {
  if (kws_stream_) {
    SherpaOnnxDestroyOnlineStream(kws_stream_);
    kws_stream_ = nullptr;
  }
  if (kws_) {
    SherpaOnnxDestroyKeywordSpotter(kws_);
    kws_ = nullptr;
  }
}

void SherpaEngines::DestroyVad() {
  if (vad_) {
    SherpaOnnxDestroyVoiceActivityDetector(vad_);
    vad_ = nullptr;
  }
  vad_pending_.clear();
  vad_speech_samples_ = 0;
}

bool SherpaEngines::SetKeywords(const std::vector<std::string>& lines) {
  std::string keywords;
  for (const std::string& line : lines) {
    keywords += line;
    keywords += '\n';
  }
  DestroySpotter();
  keywords_ = keywords;
  SherpaOnnxKeywordSpotterConfig c;
  memset(&c, 0, sizeof(c));
  c.feat_config.sample_rate = kSampleRate;
  c.feat_config.feature_dim = 80;
  c.model_config.transducer.encoder = paths_.kws_encoder.c_str();
  c.model_config.transducer.decoder = paths_.kws_decoder.c_str();
  c.model_config.transducer.joiner = paths_.kws_joiner.c_str();
  c.model_config.tokens = paths_.kws_tokens.c_str();
  c.model_config.num_threads = 1;
  c.model_config.provider = "cpu";
  c.max_active_paths = 4;
  c.num_trailing_blanks = 1;
  c.keywords_score = 1.0f;
  c.keywords_threshold = 0.15f;  // Each line sets its own (#0.15) too.
  c.keywords_buf = keywords_.c_str();
  c.keywords_buf_size = static_cast<int32_t>(keywords_.size());
  try {
    kws_ = SherpaOnnxCreateKeywordSpotter(&c);
    if (kws_) {
      kws_stream_ = SherpaOnnxCreateKeywordStream(kws_);
    }
  } catch (...) {
    kws_stream_ = nullptr;
  }
  if (!kws_ || !kws_stream_) {
    DestroySpotter();
    return false;
  }
  return true;
}

bool SherpaEngines::Spot(const std::vector<float>& samples, size_t* words) {
  if (!kws_stream_ || samples.empty()) {
    return false;
  }
  bool fired = false;
  try {
    SherpaOnnxOnlineStreamAcceptWaveform(kws_stream_, kSampleRate,
                                         samples.data(),
                                         static_cast<int32_t>(samples.size()));
    while (SherpaOnnxIsKeywordStreamReady(kws_, kws_stream_)) {
      SherpaOnnxDecodeKeywordStream(kws_, kws_stream_);
      const SherpaOnnxKeywordResult* r =
          SherpaOnnxGetKeywordResult(kws_, kws_stream_);
      const bool hit = r && r->keyword && r->keyword[0];
      if (hit) {
        // The label is the phrase with '_' between its words.
        *words = 1;
        for (const char* c = r->keyword; *c; ++c) {
          *words += *c == '_' ? 1 : 0;
        }
      }
      if (r) {
        SherpaOnnxDestroyKeywordResult(r);
      }
      if (hit) {
        fired = true;
        SherpaOnnxResetKeywordStream(kws_, kws_stream_);
      }
    }
  } catch (...) {
    Fail("the spotter failed");
  }
  return fired;
}

void SherpaEngines::ResetSpotter() {
  if (!kws_) {
    return;
  }
  try {
    // A fresh stream: the old one's features and state are dropped.
    if (kws_stream_) {
      SherpaOnnxDestroyOnlineStream(kws_stream_);
    }
    kws_stream_ = SherpaOnnxCreateKeywordStream(kws_);
  } catch (...) {
    kws_stream_ = nullptr;
  }
  if (!kws_stream_) {
    Fail("the spotter could not start a stream");
  }
}

void SherpaEngines::SetVadSilenceMs(int ms) {
  DestroyVad();
  SherpaOnnxVadModelConfig c;
  memset(&c, 0, sizeof(c));
  c.silero_vad.model = paths_.vad.c_str();
  c.silero_vad.threshold = 0.5f;
  c.silero_vad.min_silence_duration = static_cast<float>(ms) / 1000.0f;
  c.silero_vad.min_speech_duration = 0.1f;
  c.silero_vad.max_speech_duration = 20.0f;
  c.silero_vad.window_size = kSileroWindow;
  c.sample_rate = kSampleRate;
  c.num_threads = 1;
  c.provider = "cpu";
  try {
    // 30 s of buffer: more than the 20 s a stretch of speech may last.
    vad_ = SherpaOnnxCreateVoiceActivityDetector(&c, 30.0f);
  } catch (...) {
    vad_ = nullptr;
  }
  if (!vad_) {
    Fail("the voice activity detector did not load");
  }
}

VadUpdate SherpaEngines::Detect(const std::vector<float>& samples) {
  VadUpdate update;
  if (!vad_) {
    return update;
  }
  vad_pending_.insert(vad_pending_.end(), samples.begin(), samples.end());
  size_t used = 0;
  try {
    while (vad_pending_.size() - used >= kSileroWindow) {
      SherpaOnnxVoiceActivityDetectorAcceptWaveform(
          vad_, vad_pending_.data() + used, kSileroWindow);
      used += kSileroWindow;
      if (!SherpaOnnxVoiceActivityDetectorEmpty(vad_)) {
        update.segment_ended = true;
        while (!SherpaOnnxVoiceActivityDetectorEmpty(vad_)) {
          SherpaOnnxVoiceActivityDetectorPop(vad_);
        }
      }
      if (SherpaOnnxVoiceActivityDetectorDetected(vad_)) {
        vad_speech_samples_ += kSileroWindow;
        if (vad_speech_samples_ >= kMaxVadSpeechSamples) {
          SherpaOnnxVoiceActivityDetectorReset(vad_);
          vad_speech_samples_ = 0;
          update.segment_ended = true;
        }
      } else {
        vad_speech_samples_ = 0;
      }
    }
    update.in_speech = SherpaOnnxVoiceActivityDetectorDetected(vad_) != 0;
  } catch (...) {
    Fail("the voice activity detector failed");
  }
  vad_pending_.erase(vad_pending_.begin(),
                     vad_pending_.begin() + static_cast<std::ptrdiff_t>(used));
  return update;
}

void SherpaEngines::ResetVad() {
  if (!vad_) {
    return;
  }
  try {
    SherpaOnnxVoiceActivityDetectorReset(vad_);
  } catch (...) {
    Fail("the voice activity detector failed");
  }
  vad_pending_.clear();
  vad_speech_samples_ = 0;
}

void SherpaEngines::BeginUtterance(const std::string& hotwords) {
  AbandonUtterance();
  try {
    asr_stream_ = hotwords.empty()
                      ? SherpaOnnxCreateOnlineStream(asr_)
                      : SherpaOnnxCreateOnlineStreamWithHotwords(
                            asr_, hotwords.c_str());
  } catch (...) {
    asr_stream_ = nullptr;
  }
  if (!asr_stream_) {
    Fail("the recognizer could not start an utterance");
  }
}

void SherpaEngines::Recognize(const std::vector<float>& samples) {
  if (!asr_stream_ || samples.empty()) {
    return;
  }
  try {
    SherpaOnnxOnlineStreamAcceptWaveform(asr_stream_, kSampleRate,
                                         samples.data(),
                                         static_cast<int32_t>(samples.size()));
    while (SherpaOnnxIsOnlineStreamReady(asr_, asr_stream_)) {
      SherpaOnnxDecodeOnlineStream(asr_, asr_stream_);
    }
  } catch (...) {
    Fail("the recognizer failed");
  }
}

RecognizerResult SherpaEngines::Result(bool final_result) {
  RecognizerResult result;
  if (!asr_stream_) {
    return result;
  }
  try {
    const SherpaOnnxOnlineRecognizerResult* r =
        SherpaOnnxGetOnlineStreamResult(asr_, asr_stream_);
    if (r) {
      result.text = r->text ? r->text : "";
      // A word starts with a token that starts with U+2581.
      if (r->timestamps && r->tokens_arr) {
        for (int32_t i = 0; i < r->count; ++i) {
          const char* token = r->tokens_arr[i];
          if (token && strncmp(token, "\xE2\x96\x81", 3) == 0) {
            result.word_seconds.push_back(r->timestamps[i]);
          }
        }
      }
      if (final_result) {
        result.confidence = ConfidenceFromJson(r->json);
      }
      SherpaOnnxDestroyOnlineRecognizerResult(r);
    }
  } catch (...) {
    Fail("the recognizer failed");
  }
  return result;
}

RecognizerResult SherpaEngines::Partial() {
  return Result(false);
}

RecognizerResult SherpaEngines::Finish() {
  if (!asr_stream_) {
    return RecognizerResult();
  }
  const std::vector<float> silence(kTailPaddingSamples, 0.0f);
  try {
    SherpaOnnxOnlineStreamAcceptWaveform(asr_stream_, kSampleRate,
                                         silence.data(),
                                         static_cast<int32_t>(silence.size()));
    SherpaOnnxOnlineStreamInputFinished(asr_stream_);
    while (SherpaOnnxIsOnlineStreamReady(asr_, asr_stream_)) {
      SherpaOnnxDecodeOnlineStream(asr_, asr_stream_);
    }
  } catch (...) {
    Fail("the recognizer failed");
  }
  RecognizerResult result = Result(true);
  AbandonUtterance();
  return result;
}

void SherpaEngines::AbandonUtterance() {
  if (asr_stream_) {
    SherpaOnnxDestroyOnlineStream(asr_stream_);
    asr_stream_ = nullptr;
  }
}

}  // namespace molt_speech
