// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// apm_sim: the pinned Chromium's WebRTC audio processing, built standalone
// (build.sh), run over what the browser plays (the render) and what the
// microphone hears (the capture), both 16 kHz mono 16-bit WAV, as voice
// mode's stream sets it up (media::CreateWebRtcAudioProcessingModule():
// echo cancellation and noise suppression on, gain control off, no system
// loopback reference). Writes the processed capture, and optionally what the
// echo canceller's linear filter alone left.
//
//   apm_sim [--tuning=chromium|voice] [--trials=STRING] [--no-ns]
//           [--delay-ms=N] [--linear=OUT.wav] RENDER.wav CAPTURE.wav OUT.wav
//
// --tuning=voice: the echo canceller as voice mode's stream gets it
//   (media/webrtc/molt_speech_recognizer_processing.cc, compiled in as is).
// --trials: WebRTC field trials; by default the browser's
//   (WebRTC-Aec3TransparentModeHmm/Disabled/, molt_feature_overrides.cc).

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "api/audio/audio_processing.h"
#include "api/audio/builtin_audio_processing_builder.h"
#include "api/audio/echo_canceller3_config.h"
#include "api/environment/environment.h"
#include "api/environment/environment_factory.h"
#include "api/field_trials.h"
#include "media/webrtc/molt_speech_recognizer_processing.h"

namespace {

constexpr int kRate = 16000;
constexpr size_t kFrame = kRate / 100;

bool ReadWav(const char* path, std::vector<int16_t>* out) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    return false;
  }
  std::vector<uint8_t> b;
  uint8_t buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    b.insert(b.end(), buf, buf + n);
  }
  fclose(f);
  if (b.size() < 12 || memcmp(b.data(), "RIFF", 4) ||
      memcmp(b.data() + 8, "WAVE", 4)) {
    return false;
  }
  size_t p = 12;
  while (p + 8 <= b.size()) {
    uint32_t len;
    memcpy(&len, b.data() + p + 4, 4);
    if (!memcmp(b.data() + p, "data", 4)) {
      const size_t end = std::min(b.size(), p + 8 + static_cast<size_t>(len));
      out->resize((end - p - 8) / 2);
      memcpy(out->data(), b.data() + p + 8, out->size() * 2);
      return true;
    }
    p += 8 + len + (len & 1);
  }
  return false;
}

bool WriteWav(const char* path, const std::vector<int16_t>& x) {
  FILE* f = fopen(path, "wb");
  if (!f) {
    return false;
  }
  const uint32_t data = x.size() * 2, riff = 36 + data, fmt = 16,
                 rate = kRate, bps = kRate * 2;
  const uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
  fwrite("RIFF", 1, 4, f);
  fwrite(&riff, 4, 1, f);
  fwrite("WAVEfmt ", 1, 8, f);
  fwrite(&fmt, 4, 1, f);
  fwrite(&pcm, 2, 1, f);
  fwrite(&ch, 2, 1, f);
  fwrite(&rate, 4, 1, f);
  fwrite(&bps, 4, 1, f);
  fwrite(&align, 2, 1, f);
  fwrite(&bits, 2, 1, f);
  fwrite("data", 1, 4, f);
  fwrite(&data, 4, 1, f);
  fwrite(x.data(), 2, x.size(), f);
  return fclose(f) == 0;
}

int Usage() {
  fprintf(stderr,
          "usage: apm_sim [--tuning=chromium|voice] [--trials=STRING] "
          "[--no-ns] [--delay-ms=N] [--linear=OUT.wav] RENDER.wav "
          "CAPTURE.wav OUT.wav\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  std::string tuning = "chromium";
  std::string trials = "WebRTC-Aec3TransparentModeHmm/Disabled/";
  std::string linear_path;
  bool ns = true;
  int delay_ms = 60;
  std::vector<const char*> files;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a.rfind("--tuning=", 0) == 0) {
      tuning = a.substr(9);
    } else if (a.rfind("--trials=", 0) == 0) {
      trials = a.substr(9);
    } else if (a.rfind("--linear=", 0) == 0) {
      linear_path = a.substr(9);
    } else if (a == "--no-ns") {
      ns = false;
    } else if (a.rfind("--delay-ms=", 0) == 0) {
      delay_ms = atoi(a.c_str() + 11);
    } else if (a.rfind("--", 0) == 0) {
      return Usage();
    } else {
      files.push_back(argv[i]);
    }
  }
  if (files.size() != 3 || (tuning != "chromium" && tuning != "voice")) {
    return Usage();
  }
  std::vector<int16_t> render, capture;
  if (!ReadWav(files[0], &render) || !ReadWav(files[1], &capture)) {
    fprintf(stderr, "apm_sim: can't read the inputs\n");
    return 1;
  }

  webrtc::Environment env =
      webrtc::CreateEnvironment(std::make_unique<webrtc::FieldTrials>(trials));
  webrtc::AudioProcessing::Config config;
  config.noise_suppression.enabled = ns;
  config.noise_suppression.level =
      webrtc::AudioProcessing::Config::NoiseSuppression::Level::kHigh;
  config.echo_canceller.enabled = true;
  config.echo_canceller.export_linear_aec_output = !linear_path.empty();
  config.gain_controller1.enabled = false;
  config.gain_controller2.enabled = false;
  webrtc::BuiltinAudioProcessingBuilder builder(config);
  if (tuning == "voice" || !linear_path.empty()) {
    webrtc::EchoCanceller3Config mono;
    webrtc::EchoCanceller3Config multi =
        webrtc::EchoCanceller3Config::CreateDefaultMultichannelConfig();
    if (tuning == "voice") {
      mono = media::SpeechRecognizerEchoCancellerConfig(mono);
      multi = media::SpeechRecognizerEchoCancellerConfig(multi);
    }
    mono.filter.export_linear_aec_output = !linear_path.empty();
    multi.filter.export_linear_aec_output = !linear_path.empty();
    builder.SetEchoCancellerConfig(mono, multi);
  }
  webrtc::scoped_refptr<webrtc::AudioProcessing> apm = builder.Build(env);
  if (!apm) {
    fprintf(stderr, "apm_sim: no audio processing\n");
    return 1;
  }

  const webrtc::StreamConfig mono_16k(kRate, 1);
  const size_t frames = capture.size() / kFrame;
  std::vector<int16_t> out(frames * kFrame), linear(frames * kFrame);
  std::vector<int16_t> r(kFrame), r_out(kFrame);
  std::array<float, kFrame> linear_frame[1];
  for (size_t i = 0; i < frames; ++i) {
    for (size_t k = 0; k < kFrame; ++k) {
      const size_t j = i * kFrame + k;
      r[k] = j < render.size() ? render[j] : 0;
    }
    apm->ProcessReverseStream(r.data(), mono_16k, mono_16k, r_out.data());
    apm->set_stream_delay_ms(delay_ms);
    apm->ProcessStream(capture.data() + i * kFrame, mono_16k, mono_16k,
                       out.data() + i * kFrame);
    if (!linear_path.empty() && apm->GetLinearAecOutput(linear_frame)) {
      for (size_t k = 0; k < kFrame; ++k) {
        const float v = linear_frame[0][k] * 32768.f;
        linear[i * kFrame + k] =
            static_cast<int16_t>(std::clamp(v, -32768.f, 32767.f));
      }
    }
  }
  if (!WriteWav(files[2], out) ||
      (!linear_path.empty() && !WriteWav(linear_path.c_str(), linear))) {
    fprintf(stderr, "apm_sim: can't write the output\n");
    return 1;
  }
  return 0;
}
