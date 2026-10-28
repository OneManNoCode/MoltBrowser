// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: microphone processing that computes the same samples on every
// CPU (MoltNet's streams; media::AudioProcessingSettings::cpu_independent,
// third_party/webrtc/rtc_base/cpu_info.h ScopedCpuIndependent).
//
// The golden digests are what WebRTC's audio processing, configured as
// CreateWebRtcAudioProcessingModule() configures it for getUserMedia(),
// produces in CPU-independent mode for the fixed signals below: a near-end
// talker with the far end's echo (the far end analyzed as the playout
// reference), and the talker alone. Every CPU and OS must reproduce them;
// they were computed with Chromium 156's WebRTC built standalone (x86-64
// natively and under qemu on Core 2, Nehalem, Sandy Bridge, Haswell and EPYC
// models, and arm64 under qemu on Cortex-A53, Cortex-A76 and the max CPU),
// all of which agree. Without the mode the digests differ between CPUs with
// and without AVX2, so the default instance is only checked for running.

#include <cstdint>
#include <cstring>
#include <ios>
#include <vector>

#include "media/base/audio_processing.h"
#include "media/webrtc/helpers.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/webrtc/api/audio/audio_processing.h"

namespace media {
namespace {

// BEGIN SIGNAL (tools: the standalone harness extracts this block)
constexpr int kRate = 48000;
constexpr int kFrame = 480;  // 10 ms

struct Lcg {
  uint32_t s;
  uint32_t Next() {
    s = s * 1664525u + 1013904223u;
    return s;
  }
  // [-1, 1)
  float Uniform() { return static_cast<int32_t>(Next()) / 2147483648.0f; }
};

// A triangle wave from an integer phase accumulator (no C library).
float Tri(uint32_t phase) {
  int32_t p = static_cast<int32_t>(phase >> 8);   // 24 bits
  int32_t v = p < (1 << 23) ? p : (1 << 24) - p;  // 0 .. 2^23
  return (v - (1 << 22)) / 4194304.0f;            // -1 .. 1
}

// A "talker": three harmonics of a gliding pitch, gated into syllables.
struct Talker {
  uint32_t base_inc;
  int syll_len;
  int gap_len;
  uint32_t ph[3] = {0, 0, 0};
  int t = 0;
  float Sample() {
    const int cycle = syll_len + gap_len;
    const int pos = t % cycle;
    float env = 0.f;
    if (pos < syll_len) {
      const int up = syll_len / 8;
      env = pos < up ? pos / static_cast<float>(up)
                     : (syll_len - pos) / static_cast<float>(syll_len - up);
    }
    const uint32_t glide =
        base_inc + static_cast<uint32_t>((t / 64) % 400) * 3000u;
    float s = 0.f;
    for (int h = 0; h < 3; ++h) {
      ph[h] += glide * static_cast<uint32_t>(h + 1);
      s += Tri(ph[h]) / static_cast<float>(h + 1);
    }
    ++t;
    return 0.35f * env * s;
  }
};

float Quantize(float x) {
  float v = x * 32767.f;
  v = v > 32767.f ? 32767.f : (v < -32768.f ? -32768.f : v);
  return static_cast<float>(static_cast<int>(v >= 0 ? v + 0.5f : v - 0.5f)) /
         32768.f;
}

void Fnv(uint64_t& hash, const float* p, int n) {
  for (int i = 0; i < n; ++i) {
    uint32_t b;
    std::memcpy(&b, &p[i], sizeof(b));
    for (int k = 0; k < 4; ++k) {
      hash = (hash ^ ((b >> (8 * k)) & 255)) * 1099511628211ull;
    }
  }
}

// Processes `seconds` of 48 kHz stereo capture into mono, as Chromium's
// AudioProcessor::ProcessData() calls the module (stream delay, analog level
// 255 of a device at full volume), with the far end analyzed first when
// `echo`. Returns a digest of the processed samples.
uint64_t ProcessFixedSignal(webrtc::AudioProcessing& apm,
                            bool echo,
                            int seconds) {
  Talker near{9000000u, 9000, 5000};
  Talker far{13000000u, 11000, 7000};
  Lcg noise{12345};
  std::vector<float> echo_line(2400, 0.f);  // 50 ms
  int echo_pos = 0;
  const float fir[5] = {0.12f, 0.31f, -0.08f, 0.05f, -0.02f};
  float fir_state[5] = {0, 0, 0, 0, 0};
  std::vector<float> cap_l(kFrame), cap_r(kFrame), ren_l(kFrame),
      ren_r(kFrame), out_l(kFrame), out_r(kFrame);
  const webrtc::StreamConfig in_cfg(kRate, 2), out_cfg(kRate, 1),
      ren_cfg(kRate, 2);
  uint64_t hash = 1469598103934665603ull;
  for (int f = 0; f < seconds * 100; ++f) {
    for (int i = 0; i < kFrame; ++i) {
      float r = 0.f;
      float e = 0.f;
      if (echo) {
        r = far.Sample();
        ren_l[i] = Quantize(r);
        ren_r[i] = Quantize(0.8f * r);
        for (int k = 4; k > 0; --k) {
          fir_state[k] = fir_state[k - 1];
        }
        fir_state[0] = echo_line[echo_pos];
        for (int k = 0; k < 5; ++k) {
          e += fir[k] * fir_state[k];
        }
        echo_line[echo_pos] = r;
        echo_pos = (echo_pos + 1) % static_cast<int>(echo_line.size());
      }
      const float m = near.Sample() + 0.6f * e + 0.004f * noise.Uniform();
      cap_l[i] = Quantize(m);
      cap_r[i] = Quantize(0.9f * m + 0.002f * noise.Uniform());
    }
    if (echo) {
      const float* rp[2] = {ren_l.data(), ren_r.data()};
      apm.AnalyzeReverseStream(rp, ren_cfg);
    }
    apm.set_stream_delay_ms(40);
    apm.set_stream_analog_level(255);
    const float* cp[2] = {cap_l.data(), cap_r.data()};
    float* op[2] = {out_l.data(), out_r.data()};
    apm.ProcessStream(cp, in_cfg, out_cfg, op);
    apm.recommended_stream_analog_level();
    Fnv(hash, out_l.data(), kFrame);
  }
  return hash;
}
// END SIGNAL

constexpr uint64_t kGoldenEcho = 0xc9d01531b05d1f69ull;
constexpr uint64_t kGoldenTalker = 0x25b2f3f2e9fa4b69ull;

AudioProcessingSettings Settings(bool cpu_independent) {
  AudioProcessingSettings settings;
  settings.echo_cancellation = true;
  settings.noise_suppression = true;
  settings.automatic_gain_control = true;
  settings.cpu_independent = cpu_independent;
  return settings;
}

webrtc::scoped_refptr<webrtc::AudioProcessing> Create(bool cpu_independent) {
  return CreateWebRtcAudioProcessingModule(Settings(cpu_independent), nullptr)
      .first;
}

TEST(MoltCpuIndependentProcessingTest, SettingsCarryTheMode) {
  EXPECT_FALSE(AudioProcessingSettings().cpu_independent);
  EXPECT_FALSE(Settings(true) == Settings(false));
  EXPECT_NE(Settings(true).ToString().find("cpu independent: yes"),
            std::string::npos);
  EXPECT_NE(Settings(false).ToString().find("cpu independent: no"),
            std::string::npos);
}

TEST(MoltCpuIndependentProcessingTest, ModuleIsBuiltInTheMode) {
  auto independent = Create(true);
  auto fast = Create(false);
  ASSERT_TRUE(independent);
  ASSERT_TRUE(fast);
  EXPECT_TRUE(independent->GetConfig().moltbrowser_cpu_independent);
  EXPECT_FALSE(fast->GetConfig().moltbrowser_cpu_independent);
  // Fixed when the module is built: a later config can't switch it.
  webrtc::AudioProcessing::Config config = independent->GetConfig();
  config.moltbrowser_cpu_independent = false;
  independent->ApplyConfig(config);
  EXPECT_TRUE(independent->GetConfig().moltbrowser_cpu_independent);
  config = fast->GetConfig();
  config.moltbrowser_cpu_independent = true;
  fast->ApplyConfig(config);
  EXPECT_FALSE(fast->GetConfig().moltbrowser_cpu_independent);
}

TEST(MoltCpuIndependentProcessingTest, GoldenDigests) {
  auto echo = Create(true);
  ASSERT_TRUE(echo);
  EXPECT_EQ(ProcessFixedSignal(*echo, /*echo=*/true, 4), kGoldenEcho)
      << std::hex << ProcessFixedSignal(*Create(true), true, 4);
  auto talker = Create(true);
  ASSERT_TRUE(talker);
  EXPECT_EQ(ProcessFixedSignal(*talker, /*echo=*/false, 3), kGoldenTalker)
      << std::hex << ProcessFixedSignal(*Create(true), false, 3);
}

// The mode belongs to the instance: a CPU-independent module and one with
// the CPU's fast paths, processing in turn on one thread (as the audio
// service may run MoltNet's and another window's streams), each give what
// they give alone.
TEST(MoltCpuIndependentProcessingTest, InstancesDontShareTheMode) {
  // Each module alone: two calls of one second (each call starts the signal
  // again, on the module's state from the first).
  uint64_t alone[2][2];
  for (int mode = 0; mode < 2; ++mode) {
    auto apm = Create(mode == 0);
    ASSERT_TRUE(apm);
    for (int call = 0; call < 2; ++call) {
      alone[mode][call] = ProcessFixedSignal(*apm, /*echo=*/true, 1);
    }
  }
  // The two in turn on this thread.
  auto independent = Create(true);
  auto fast = Create(false);
  ASSERT_TRUE(independent);
  ASSERT_TRUE(fast);
  for (int call = 0; call < 2; ++call) {
    EXPECT_EQ(ProcessFixedSignal(*independent, true, 1), alone[0][call]);
    EXPECT_EQ(ProcessFixedSignal(*fast, true, 1), alone[1][call]);
  }
}

}  // namespace
}  // namespace media
