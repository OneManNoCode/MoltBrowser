// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: voice mode's echo canceller tuning (media::
// AudioProcessingSettings::speech_recognizer, molt_speech_recognizer_
// processing.h), on WebRTC's audio processing as
// CreateWebRtcAudioProcessingModule() configures it for voice mode's stream
// (echo cancellation and noise suppression, no gain control; 16 kHz mono).
//
// The far end is the spoken suite's song (tools/voice_bench/audio.py
// synth_music(), ported: chords, bass, a pentatonic melody, drums), its echo
// 6 dB down and 60 ms late, as the video's in the suite's speakers run; a
// talker begins over it after 20 s. With Chromium's tuning the canceller's
// linear filter doesn't settle on the song's steady tones and the suppressor
// takes the talker's start for echo (about 60 dB down); with the speech
// recognizer's it comes through. Voice mode's own voice (a voiced talker as
// the far end) is removed as well with either.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "media/base/audio_processing.h"
#include "media/webrtc/helpers.h"
#include "media/webrtc/molt_speech_recognizer_processing.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/webrtc/api/audio/audio_processing.h"
#include "third_party/webrtc/api/audio/echo_canceller3_config.h"

namespace media {
namespace {

constexpr int kRate = 16000;
constexpr int kFrame = kRate / 100;
constexpr float kPi = 3.14159265f;
constexpr int kSeconds = 24;
constexpr int kTalkFrom = 20 * kRate;
constexpr int kTalkTo = 22 * kRate;
constexpr int kEchoDelay = kRate * 60 / 1000;

// A fixed pseudo-random sequence in [-1, 1) (not <random>'s distributions,
// whose output differs between standard libraries).
class Lcg {
 public:
  float Next() {
    state_ = state_ * 1664525u + 1013904223u;
    return static_cast<float>(state_ >> 8) / 8388608.f - 1.f;
  }
  uint32_t bits() const { return state_ >> 13; }

 private:
  uint32_t state_ = 12345;
};

// synth_music() at `bpm` beats a minute with its root at `key` Hz, about
// -23 dBFS (a video at 0.7 of the song's -20 dB).
std::vector<float> Song(int bpm, float key) {
  const int n = kSeconds * kRate;
  const int beat = kRate * 60 / bpm;
  const int prog[4][3] = {{0, 4, 7}, {7, 11, 14}, {9, 12, 16}, {5, 9, 12}};
  std::vector<float> out(n, 0.f);
  Lcg lcg;
  for (int bar = 0; bar < n; bar += 4 * beat) {
    const int* chord = prog[(bar / (4 * beat)) % 4];
    const int m = std::min(4 * beat, n - bar);
    const int attack = std::min(m, kRate / 20);
    const int release = std::min(m - attack, kRate * 3 / 10);
    const float bass = key / 2 * std::pow(2.f, chord[0] / 12.f);
    for (int i = 0; i < m; ++i) {
      const float t = static_cast<float>(i) / kRate;
      float v = 0.f;
      for (int k = 0; k < 3; ++k) {
        const float f = key * std::pow(2.f, chord[k] / 12.f);
        v += 0.12f *
             (std::sin(2 * kPi * f * t) + 0.4f * std::sin(4 * kPi * f * t) +
              0.2f * std::sin(6 * kPi * f * t));
      }
      float env = 1.f;
      if (i < attack) {
        env = static_cast<float>(i) / attack;
      }
      if (i >= m - release) {
        env = static_cast<float>(m - i) / release;
      }
      const float tb = static_cast<float>(i % beat) / kRate;
      out[bar + i] +=
          v * env + 0.35f * std::sin(2 * kPi * bass * tb) * std::exp(-tb * 4);
    }
  }
  const int penta[7] = {0, 2, 4, 7, 9, 12, 14};
  for (int s = 0; s < n; s += beat / 2) {
    if (lcg.Next() < -0.3f) {
      continue;  // a rest
    }
    const float f = key * 2 * std::pow(2.f, penta[lcg.bits() % 7] / 12.f);
    for (int i = 0; i < beat / 2 && s + i < n; ++i) {
      const float tb = static_cast<float>(i) / kRate;
      out[s + i] += 0.25f * std::sin(2 * kPi * f * tb) * std::exp(-tb * 6);
    }
  }
  for (int s = 0, b = 0; s < n; s += beat, ++b) {
    for (int i = 0; i < kRate * 3 / 20 && s + i < n; ++i) {
      const float tb = static_cast<float>(i) / kRate;
      out[s + i] +=
          b % 2 == 0
              ? 0.6f * std::sin(2 * kPi * (50 + 80 * std::exp(-tb * 30)) * tb) *
                    std::exp(-tb * 18)  // kick
              : 0.51f * lcg.Next() * std::exp(-tb * 25);
    }
    float prev = lcg.Next();
    for (int i = 0; i < kRate / 25 && s + beat / 2 + i < n; ++i) {
      const float cur = lcg.Next();  // hi-hat
      out[s + beat / 2 + i] +=
          0.204f * (cur - prev) * std::exp(-static_cast<float>(i) / kRate * 80);
      prev = cur;
    }
  }
  double energy = 0;
  for (float v : out) {
    energy += static_cast<double>(v) * v;
  }
  const float gain = static_cast<float>(0.07 / std::sqrt(energy / n + 1e-12));
  for (float& v : out) {
    v *= gain;
  }
  return out;
}

// A voice at 120 Hz through two formants (700 and 1200 Hz), four syllables
// a second, about -26 dBFS.
std::vector<float> Talker(int samples) {
  std::vector<float> x(samples);
  const float formants[2] = {700.f, 1200.f};
  float state[2][2] = {{0, 0}, {0, 0}};
  for (int i = 0; i < samples; ++i) {
    const float pulse = i % (kRate / 120) == 0 ? 1.f : 0.f;
    float y = 0.f;
    for (int k = 0; k < 2; ++k) {
      const float r = 0.97f;
      const float o =
          pulse +
          2 * r * std::cos(2 * kPi * formants[k] / kRate) * state[k][0] -
          r * r * state[k][1];
      state[k][1] = state[k][0];
      state[k][0] = o;
      y += o;
    }
    const float t = static_cast<float>(i) / kRate;
    x[i] = 0.07f * y * (0.5f - 0.5f * std::cos(2 * kPi * 4 * t));
  }
  return x;
}

double Db(const std::vector<float>& x, int from, int to) {
  double energy = 0;
  for (int i = from; i < to; ++i) {
    energy += static_cast<double>(x[i]) * x[i];
  }
  return 10 * std::log10(energy / (to - from) + 1e-20);
}

AudioProcessingSettings VoiceModeSettings(bool speech_recognizer) {
  AudioProcessingSettings settings;
  settings.echo_cancellation = true;
  settings.noise_suppression = true;
  settings.automatic_gain_control = false;
  settings.multi_channel_capture_processing = false;
  settings.speech_recognizer = speech_recognizer;
  return settings;
}

struct Processed {
  std::vector<float> microphone;
  std::vector<float> near_end;
  std::vector<float> out;
};

// `far` played, its echo and `near_end` in the microphone, through the
// processing voice mode's stream gets.
Processed Process(bool speech_recognizer,
                  const std::vector<float>& far,
                  const std::vector<float>& near_end) {
  Processed p;
  const int n = static_cast<int>(far.size());
  p.near_end = near_end;
  p.microphone.resize(n);
  p.out.resize(n);
  for (int i = 0; i < n; ++i) {
    p.microphone[i] =
        (i >= kEchoDelay ? 0.5f * far[i - kEchoDelay] : 0.f) + near_end[i];
  }
  auto apm = CreateWebRtcAudioProcessingModule(
                 VoiceModeSettings(speech_recognizer), nullptr)
                 .first;
  EXPECT_TRUE(apm);
  if (!apm) {
    return p;
  }
  const webrtc::StreamConfig mono(kRate, 1);
  std::vector<float> render_out(kFrame);
  for (int i = 0; i + kFrame <= n; i += kFrame) {
    const float* render = &far[i];
    float* render_dest = render_out.data();
    EXPECT_EQ(apm->ProcessReverseStream(&render, mono, mono, &render_dest), 0);
    apm->set_stream_delay_ms(60);
    const float* capture = &p.microphone[i];
    float* dest = &p.out[i];
    EXPECT_EQ(apm->ProcessStream(&capture, mono, mono, &dest), 0);
  }
  return p;
}

TEST(MoltSpeechRecognizerProcessingTest, SettingsCarryTheFlag) {
  EXPECT_FALSE(AudioProcessingSettings().speech_recognizer);
  EXPECT_FALSE(VoiceModeSettings(true) == VoiceModeSettings(false));
  EXPECT_NE(VoiceModeSettings(true).ToString().find("speech recognizer: yes"),
            std::string::npos);
  EXPECT_NE(VoiceModeSettings(false).ToString().find("speech recognizer: no"),
            std::string::npos);
}

// The delay estimator without its search for an earlier lag; the
// suppressor quicker to its near-end tuning, and that tuning more
// transparent. Nothing else changes.
TEST(MoltSpeechRecognizerProcessingTest, TheTuning) {
  for (const webrtc::EchoCanceller3Config& base :
       {webrtc::EchoCanceller3Config(),
        webrtc::EchoCanceller3Config::CreateDefaultMultichannelConfig()}) {
    ASSERT_TRUE(base.delay.detect_pre_echo);
    const webrtc::EchoCanceller3Config tuned =
        SpeechRecognizerEchoCancellerConfig(base);
    EXPECT_FALSE(tuned.delay.detect_pre_echo);
    const auto& nearend = tuned.suppressor.dominant_nearend_detection;
    EXPECT_EQ(nearend.enr_threshold, 4.f);
    EXPECT_EQ(nearend.snr_threshold, 5.f);
    EXPECT_EQ(nearend.trigger_threshold, 1);
    EXPECT_EQ(nearend.hold_duration, 250);
    EXPECT_EQ(nearend.enr_exit_threshold,
              base.suppressor.dominant_nearend_detection.enr_exit_threshold);
    for (const auto* mask : {&tuned.suppressor.nearend_tuning.mask_lf,
                             &tuned.suppressor.nearend_tuning.mask_hf}) {
      EXPECT_EQ(mask->enr_transparent, 3.f);
      EXPECT_EQ(mask->enr_suppress, 6.f);
    }
    EXPECT_EQ(tuned.suppressor.nearend_tuning.mask_lf.emr_transparent,
              base.suppressor.nearend_tuning.mask_lf.emr_transparent);
    EXPECT_EQ(tuned.suppressor.nearend_tuning.max_inc_factor,
              base.suppressor.nearend_tuning.max_inc_factor);
    // The normal tuning, the filters and the rest of the delay estimator
    // are Chromium's.
    EXPECT_EQ(tuned.suppressor.normal_tuning.mask_lf.enr_transparent,
              base.suppressor.normal_tuning.mask_lf.enr_transparent);
    EXPECT_EQ(tuned.suppressor.normal_tuning.mask_hf.enr_suppress,
              base.suppressor.normal_tuning.mask_hf.enr_suppress);
    EXPECT_EQ(tuned.filter.refined.length_blocks,
              base.filter.refined.length_blocks);
    EXPECT_EQ(tuned.delay.num_filters, base.delay.num_filters);
    EXPECT_EQ(tuned.delay.fixed_capture_delay_samples,
              base.delay.fixed_capture_delay_samples);
    EXPECT_EQ(tuned.delay.use_external_delay_estimator,
              base.delay.use_external_delay_estimator);
  }
  // A config the stream chose keeps its own values.
  webrtc::EchoCanceller3Config chosen;
  chosen.delay.fixed_capture_delay_samples = 480;
  chosen.delay.num_filters = 9;
  const webrtc::EchoCanceller3Config tuned =
      SpeechRecognizerEchoCancellerConfig(chosen);
  EXPECT_EQ(tuned.delay.fixed_capture_delay_samples, 480u);
  EXPECT_EQ(tuned.delay.num_filters, 9u);
  EXPECT_FALSE(tuned.delay.detect_pre_echo);
}

// A talker who begins over a song: its first 300 ms (where "Hey" of "Hey
// Molt" is) come through, and so does the rest; the song's echo is still
// removed before the talker. With Chromium's tuning the start of the talker
// came out 61 dB down over the first of these songs (Chromium 156.0.8078.80)
// and only a few decibels down over others: whether the check for an
// earlier lag misleads the delay estimator depends on the song's details.
TEST(MoltSpeechRecognizerProcessingTest, ATalkerOverASongComesThrough) {
  struct Variant {
    int bpm;
    float key;
  };
  for (const Variant& v : {Variant{110, 220.f}, Variant{96, 196.f},
                           Variant{124, 246.94f}, Variant{132, 174.61f}}) {
    SCOPED_TRACE(testing::Message() << v.bpm << " bpm, " << v.key << " Hz");
    const std::vector<float> far = Song(v.bpm, v.key);
    std::vector<float> near_end(far.size(), 0.f);
    const std::vector<float> talker = Talker(kTalkTo - kTalkFrom);
    std::copy(talker.begin(), talker.end(), near_end.begin() + kTalkFrom);
    const Processed tuned = Process(true, far, near_end);
    const int start_to = kTalkFrom + kRate * 3 / 10;
    EXPECT_GT(
        Db(tuned.out, kTalkFrom, start_to) - Db(near_end, kTalkFrom, start_to),
        -10.0);
    EXPECT_GT(
        Db(tuned.out, kTalkFrom, kTalkTo) - Db(near_end, kTalkFrom, kTalkTo),
        -6.0);
    const int song_from = 15 * kRate;
    EXPECT_LT(Db(tuned.out, song_from, kTalkFrom),
              Db(tuned.microphone, song_from, kTalkFrom) - 20.0);
  }
}

// Voice mode's own voice as the far end, nobody speaking: removed as well as
// with Chromium's tuning (measured 108 dB down with either), for the
// confirmation word's sake (TOUCHLESS_BROWSING.md §7.3).
TEST(MoltSpeechRecognizerProcessingTest, OwnVoiceIsRemovedAsWell) {
  const std::vector<float> far = Talker(kSeconds * kRate);
  const std::vector<float> silence(far.size(), 0.f);
  const Processed tuned = Process(true, far, silence);
  const Processed chromium = Process(false, far, silence);
  const int from = 15 * kRate, to = 20 * kRate;
  EXPECT_LT(Db(tuned.out, from, to), Db(tuned.microphone, from, to) - 40.0);
  EXPECT_LT(Db(tuned.out, from, to), Db(chromium.out, from, to) + 3.0);
}

}  // namespace
}  // namespace media
