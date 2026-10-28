// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: the echo reference a MoltNet microphone stream's echo
// canceller hears, and what the processing computes from it, the same on
// every CPU and OS.
//
// With Chromium-wide echo cancellation the reference is the audio service's
// mix for the output device (services/audio MixingGraphImpl): each stream
// resampled to the device's rate by media's SincResampler, the first one
// copied and the others added with vector_math::FMAC. While a MoltNet stream
// listens, OutputDeviceMixerImpl mixes inside a media::
// ScopedPortableAudioThread, and the graph's resamplers were built in one
// (their kernels from the portable math functions). The fixed signals below
// go through the same code the same way: a 44.1 kHz stream resampled to the
// device's 48 kHz (one resampler per channel, 441-frame requests, 480-frame
// callbacks) and a 48 kHz stream added; the microphone hears a talker and the
// mix's echo, and WebRTC's audio processing, configured as
// CreateWebRtcAudioProcessingModule() configures it for getUserMedia() in
// CPU-independent mode, cancels it with the mix as its reference.
//
// The golden digests were computed with Chromium 156's media and WebRTC code
// built standalone (x86-64 natively and under qemu on Core 2, Nehalem, Sandy
// Bridge, Haswell and EPYC models, and arm64 under qemu on Cortex-A53,
// Cortex-A76 and the max CPU), all of which agree. Mixed with the CPU's code
// instead, the reference, and with it the processed samples, differ between
// CPUs with and without AVX2 and FMA.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <ios>
#include <memory>
#include <vector>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/memory/aligned_memory.h"
#include "media/base/audio_processing.h"
#include "media/base/molt_portable_media.h"
#include "media/base/sinc_resampler.h"
#include "media/base/vector_math.h"
#include "media/webrtc/helpers.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/webrtc/api/audio/audio_processing.h"

namespace media {
namespace {

// BEGIN REFERENCE SIGNAL (tools: the standalone harness extracts this block)
constexpr int kDeviceRate = 48000;
constexpr int kDeviceFrames = 480;  // 10 ms
constexpr int kStreamARate = 44100;
constexpr int kStreamAFrames = 441;  // the renderer's 10 ms buffers

// A triangle wave from an integer phase accumulator (no C library).
float RefTri(uint32_t phase) {
  int32_t p = static_cast<int32_t>(phase >> 8);   // 24 bits
  int32_t v = p < (1 << 23) ? p : (1 << 24) - p;  // 0 .. 2^23
  return (v - (1 << 22)) / 4194304.0f;            // -1 .. 1
}

// A page's tone, as its renderer delivers it: three harmonics of a slowly
// gliding pitch, stereo.
struct RefTone {
  uint32_t base_inc;
  float gain;
  uint32_t ph[3] = {0, 0, 0};
  int t = 0;
  void Next(float& left, float& right) {
    const uint32_t inc =
        base_inc + static_cast<uint32_t>((t / 97) % 300) * 4000u;
    float s = 0.f;
    for (int h = 0; h < 3; ++h) {
      ph[h] += inc * static_cast<uint32_t>(h + 1);
      s += RefTri(ph[h]) / static_cast<float>(h + 1);
    }
    ++t;
    left = gain * s;
    right = gain * (0.7f * s + 0.2f * RefTri(ph[1] + 0x40000000u));
  }
};

// The device's mix of stream A (44.1 kHz, resampled) and stream B (48 kHz),
// as MixingGraphImpl computes it: A's converter resamples each channel with
// its own SincResampler (441-frame requests); the main converter copies A
// and adds B with FMAC at volume 1, then clamps. `portable`: in a
// ScopedPortableAudioThread, the resamplers built in one too.
class RefMixer {
 public:
  explicit RefMixer(bool portable) : portable_(portable) {
    ScopedPortableAudioThread scope(portable_);
    for (int ch = 0; ch < 2; ++ch) {
      b_[ch] = base::AlignedUninit<float>(kDeviceFrames,
                                          vector_math::kRequiredAlignment);
      out_[ch] = base::AlignedUninit<float>(kDeviceFrames,
                                            vector_math::kRequiredAlignment);
      resamplers_[ch] = std::make_unique<SincResampler>(
          static_cast<double>(kStreamARate) / kDeviceRate, kStreamAFrames,
          base::BindRepeating(&RefMixer::ProvideA, base::Unretained(this), ch));
    }
  }

  // One device callback: the reference, planar stereo.
  void Mix(float* left, float* right) {
    ScopedPortableAudioThread scope(portable_);
    for (int ch = 0; ch < 2; ++ch) {
      resamplers_[ch]->Resample(base::span(out_[ch]));
    }
    for (int i = 0; i < kDeviceFrames; ++i) {
      tone_b_.Next(b_[0][i], b_[1][i]);
    }
    float* dest[2] = {left, right};
    for (int ch = 0; ch < 2; ++ch) {
      vector_math::FMAC(base::span(b_[ch]), 1.0f, base::span(out_[ch]));
      for (int i = 0; i < kDeviceFrames; ++i) {
        dest[ch][i] = std::clamp(out_[ch][i], -1.0f, 1.0f);
      }
    }
  }

 private:
  // Stream A's next frames for one channel's resampler. Each channel's
  // resampler asks for the same chunks in the same order (MultiChannelResampler
  // hands each chunk to both), so each reads its own copy of the stream.
  void ProvideA(int channel, base::span<float> dest) {
    for (float& sample : dest) {
      float left;
      float right;
      tone_a_[channel].Next(left, right);
      sample = channel ? right : left;
    }
  }

  const bool portable_;
  RefTone tone_a_[2] = {{11000000u, 0.30f}, {11000000u, 0.30f}};
  RefTone tone_b_{17000000u, 0.25f};
  base::AlignedHeapArray<float> b_[2];
  base::AlignedHeapArray<float> out_[2];
  std::unique_ptr<SincResampler> resamplers_[2];
};

struct RefLcg {
  uint32_t s;
  uint32_t Next() {
    s = s * 1664525u + 1013904223u;
    return s;
  }
  // [-1, 1)
  float Uniform() { return static_cast<int32_t>(Next()) / 2147483648.0f; }
};

// A "talker": three harmonics of a gliding pitch, gated into syllables.
struct RefTalker {
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
      s += RefTri(ph[h]) / static_cast<float>(h + 1);
    }
    ++t;
    return 0.35f * env * s;
  }
};

float RefQuantize(float x) {
  float v = x * 32767.f;
  v = v > 32767.f ? 32767.f : (v < -32768.f ? -32768.f : v);
  return static_cast<float>(static_cast<int>(v >= 0 ? v + 0.5f : v - 0.5f)) /
         32768.f;
}

void RefFnv(uint64_t& hash, const float* p, int n) {
  for (int i = 0; i < n; ++i) {
    uint32_t b;
    std::memcpy(&b, &p[i], sizeof(b));
    for (int k = 0; k < 4; ++k) {
      hash = (hash ^ ((b >> (8 * k)) & 255)) * 1099511628211ull;
    }
  }
}

// `seconds` of the device's mix: its digest.
uint64_t MixDigest(bool portable, int seconds) {
  RefMixer mixer(portable);
  std::vector<float> left(kDeviceFrames), right(kDeviceFrames);
  uint64_t hash = 1469598103934665603ull;
  for (int f = 0; f < seconds * 100; ++f) {
    mixer.Mix(left.data(), right.data());
    RefFnv(hash, left.data(), kDeviceFrames);
    RefFnv(hash, right.data(), kDeviceFrames);
  }
  return hash;
}

// `seconds` of a talker with the mix's echo (its left channel through a short
// filter and 50 ms of delay), 48 kHz stereo, processed into mono with the mix
// analyzed as the reference before each 10 ms of capture, as Chromium's
// AudioProcessor calls the module (stream delay 40 ms, analog level 255).
// Returns a digest of the processed samples.
uint64_t ProcessWithReference(webrtc::AudioProcessing& apm,
                              bool portable,
                              int seconds) {
  RefMixer mixer(portable);
  RefTalker near{9000000u, 9000, 5000};
  RefLcg noise{4242};
  std::vector<float> echo_line(2400, 0.f);  // 50 ms
  int echo_pos = 0;
  const float fir[5] = {0.12f, 0.31f, -0.08f, 0.05f, -0.02f};
  float fir_state[5] = {0, 0, 0, 0, 0};
  std::vector<float> ren_l(kDeviceFrames), ren_r(kDeviceFrames),
      cap_l(kDeviceFrames), cap_r(kDeviceFrames), out_l(kDeviceFrames),
      out_r(kDeviceFrames);
  const webrtc::StreamConfig in_cfg(kDeviceRate, 2), out_cfg(kDeviceRate, 1),
      ren_cfg(kDeviceRate, 2);
  uint64_t hash = 1469598103934665603ull;
  for (int f = 0; f < seconds * 100; ++f) {
    mixer.Mix(ren_l.data(), ren_r.data());
    for (int i = 0; i < kDeviceFrames; ++i) {
      for (int k = 4; k > 0; --k) {
        fir_state[k] = fir_state[k - 1];
      }
      fir_state[0] = echo_line[echo_pos];
      float e = 0.f;
      for (int k = 0; k < 5; ++k) {
        e += fir[k] * fir_state[k];
      }
      echo_line[echo_pos] = ren_l[i];
      echo_pos = (echo_pos + 1) % static_cast<int>(echo_line.size());
      const float m = near.Sample() + 0.6f * e + 0.004f * noise.Uniform();
      cap_l[i] = RefQuantize(m);
      cap_r[i] = RefQuantize(0.9f * m + 0.002f * noise.Uniform());
    }
    const float* rp[2] = {ren_l.data(), ren_r.data()};
    apm.AnalyzeReverseStream(rp, ren_cfg);
    apm.set_stream_delay_ms(40);
    apm.set_stream_analog_level(255);
    const float* cp[2] = {cap_l.data(), cap_r.data()};
    float* op[2] = {out_l.data(), out_r.data()};
    apm.ProcessStream(cp, in_cfg, out_cfg, op);
    apm.recommended_stream_analog_level();
    RefFnv(hash, out_l.data(), kDeviceFrames);
  }
  return hash;
}
// END REFERENCE SIGNAL

constexpr uint64_t kGoldenMix = 0xb7ebaa29303bc2fbull;
constexpr uint64_t kGoldenProcessed = 0x074b61a8868b53caull;

webrtc::scoped_refptr<webrtc::AudioProcessing> CreateCpuIndependent() {
  AudioProcessingSettings settings;
  settings.echo_cancellation = true;
  settings.noise_suppression = true;
  settings.automatic_gain_control = true;
  settings.cpu_independent = true;
  return CreateWebRtcAudioProcessingModule(settings, nullptr).first;
}

TEST(MoltCpuIndependentEchoReferenceTest, GoldenMix) {
  const bool portable_before = IsPortableAudioThread();
  const uint64_t digest = MixDigest(/*portable=*/true, 2);
  EXPECT_EQ(digest, kGoldenMix) << std::hex << digest;
  // The mode is the scope's: the thread is left as it was.
  EXPECT_EQ(IsPortableAudioThread(), portable_before);
}

TEST(MoltCpuIndependentEchoReferenceTest, GoldenProcessedWithTheReference) {
  auto apm = CreateCpuIndependent();
  ASSERT_TRUE(apm);
  const uint64_t digest = ProcessWithReference(*apm, /*portable=*/true, 4);
  EXPECT_EQ(digest, kGoldenProcessed) << std::hex << digest;
}

}  // namespace
}  // namespace media
