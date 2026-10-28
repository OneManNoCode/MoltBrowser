// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: subnormal floats in the audio code MoltNet's microphone and
// playback go through, handled the same way on every CPU
// (media/base/molt_portable_media.h, third_party/webrtc/rtc_base/cpu_info.h).
//
// The CPU's flush-to-zero modes differ between x86 (FTZ decides a result is
// tiny after rounding, DAZ reads subnormal operands as zero) and Arm (FZ
// decides before rounding), and Chromium sets them around these loops
// (cc::ScopedSubnormalFloatDisabler on x86 only; WebRTC's DenormalDisabler
// on both). Portable code computes with the modes off and sets the samples
// below the portable floor (2^-63) to zero where they enter (the audio
// processing then adds its deterministic noise floor of 2^-32), so the fixed
// signals below give the same samples on every CPU, also when the calling
// thread was left in a flush mode (as a platform's audio thread may be).
//
// The signals reach every range the modes treat differently: noise decaying
// one binade at a time through the subnormals to zero, values just above
// FLT_MIN, the largest and smallest subnormals, values around the portable
// floor, and their negatives. Every sample is built from its bits, so the
// signals don't depend on the floating-point mode either. They go through
// media's SincResampler as the audio service mixes a page's 44.1 kHz stream
// for a MoltNet microphone stream's echo canceller (and as a MoltNet
// renderer resamples decoded audio), and through WebRTC's audio processing,
// configured as CreateWebRtcAudioProcessingModule() configures it for
// getUserMedia() in CPU-independent mode, as the microphone and the echo
// reference.
//
// The golden digests were computed with Chromium 156's media and WebRTC code
// built standalone (x86-64 natively and under qemu on Core 2, Nehalem, Sandy
// Bridge, Haswell and EPYC models, and arm64 under qemu on Cortex-A53,
// Cortex-A76 and the max CPU), all of which agree. With Chromium's handling
// (the flush modes on, the input not flushed) the resampled digest differs
// between x86-64 and arm64 (on arm64 it also follows the calling thread's
// mode), and the processed one differs from the golden on both.

#include <cstdint>
#include <cstring>
#include <ios>
#include <vector>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "build/build_config.h"
#include "media/base/audio_processing.h"
#include "media/base/molt_portable_media.h"
#include "media/base/sinc_resampler.h"
#include "media/webrtc/helpers.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/webrtc/api/audio/audio_processing.h"

#if defined(ARCH_CPU_X86_64)
#include <xmmintrin.h>
#endif

namespace media {
namespace {

// BEGIN SUBNORMAL SIGNALS (tools: the standalone harness extracts this block)
constexpr int kStreamRate = 44100;
constexpr int kStreamFrames = 441;  // the renderer's 10 ms buffers
constexpr int kRate = 48000;
constexpr int kFrame = 480;  // 10 ms

float SubFromBits(uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}

struct SubLcg {
  uint32_t s;
  uint32_t Next() {
    s = s * 1664525u + 1013904223u;
    return s;
  }
};

// One channel of the signal, sample `t` of a 1.5 s cycle (66150 samples at
// 44.1 kHz): 0.4 s of noise whose magnitude halves every 64 samples (from
// 0.5 through the subnormal range, where the halving shifts the mantissa
// out, to zero: 149 binades), 0.2 s of values around FLT_MIN, the largest
// and smallest subnormals and the portable floor, and silence.
float SubSample(uint32_t channel, uint32_t t, SubLcg& lcg) {
  const uint32_t cycle = t % 66150u;
  const uint32_t r = lcg.Next();
  const uint32_t sign = (r & 1u) << 31;
  if (cycle < 17640u) {
    const uint32_t binade = cycle / 64u;  // 0 .. 275
    const uint32_t mantissa = (r >> 9) & 0x7fffffu;
    if (binade < 126u) {
      // Normal: 2^-1 down to 2^-126.
      return SubFromBits(sign | ((126u - binade) << 23) | mantissa);
    }
    // Subnormal, then zero: (1.m) * 2^-126 shifted right.
    const uint32_t shift = binade - 125u;
    const uint32_t m = (0x800000u | mantissa);
    return SubFromBits(sign | (shift < 24u ? (m >> shift) : 0u));
  }
  if (cycle < 26460u) {
    const uint32_t k = (cycle + channel * 7u) % 256u;
    switch ((cycle / 256u) % 6u) {
      case 0:  // FLT_MIN * (1 + k 2^-23): products land just below FLT_MIN
        return SubFromBits(sign | (0x00800000u + k));
      case 1:  // the largest subnormals
        return SubFromBits(sign | (0x007fffffu - k));
      case 2:  // the smallest subnormals
        return SubFromBits(sign | (k + 1u));
      case 3:  // around the portable floor 2^-63 (exponent field 64)
        return SubFromBits(sign | ((k & 1u) ? 0x20000000u + k
                                            : 0x1fffffffu - k));
      case 4:  // normal values a little above FLT_MIN
        return SubFromBits(sign | (((1u + (k & 7u)) << 23) | (r >> 9 & 0xffu)));
      default:  // tiny but normal: 2^-100 .. 2^-60
        return SubFromBits(sign | (((27u + (k % 41u)) << 23) |
                                   ((r >> 9) & 0x7fffffu)));
    }
  }
  return 0.0f;
}

void SubFnv(uint64_t& hash, base::span<const float> samples) {
  for (float x : samples) {
    uint32_t b;
    std::memcpy(&b, &x, sizeof(b));
    for (int k = 0; k < 4; ++k) {
      hash = (hash ^ ((b >> (8 * k)) & 255)) * 1099511628211ull;
    }
  }
}

// The stream, 44.1 kHz stereo planar, `seconds` long.
std::vector<std::vector<float>> SubStream(int seconds) {
  std::vector<std::vector<float>> out(2);
  for (uint32_t ch = 0; ch < 2; ++ch) {
    SubLcg lcg{12345u + ch};
    for (uint32_t t = 0; t < static_cast<uint32_t>(seconds * kStreamRate);
         ++t) {
      out[ch].push_back(SubSample(ch, t, lcg));
    }
  }
  return out;
}

// The stream resampled to 48 kHz as the audio service's mixing resamples it
// (a resampler per channel, 441-frame requests, 480-frame callbacks), in a
// ScopedPortableAudioThread, the resamplers built in one. Its digest.
uint64_t ResampledDigest(int seconds) {
  const std::vector<std::vector<float>> stream = SubStream(seconds);
  ScopedPortableAudioThread portable(true);
  uint64_t hash = 1469598103934665603ull;
  for (size_t ch = 0; ch < 2; ++ch) {
    size_t read = 0;
    SincResampler resampler(
        static_cast<double>(kStreamRate) / kRate, kStreamFrames,
        base::BindRepeating(
            [](const std::vector<float>* in, size_t* at,
               base::span<float> dest) {
              for (float& x : dest) {
                x = *at < in->size() ? (*in)[(*at)++] : 0.0f;
              }
            },
            &stream[ch], &read));
    std::vector<float> out(kFrame);
    for (int f = 0; f < seconds * 100; ++f) {
      resampler.Resample(base::span(out));
      SubFnv(hash, out);
    }
  }
  return hash;
}

// `seconds` of the signal resampled to 48 kHz (above) as the microphone,
// processed into mono with a quieter copy of it (a 3 ms echo, at 1/8) as the
// echo reference, analyzed before each 10 ms of capture as Chromium's
// AudioProcessor calls the module (stream delay 30 ms, analog level 255).
// Returns a digest of the processed samples.
uint64_t ProcessedDigest(webrtc::AudioProcessing& apm, int seconds) {
  const std::vector<std::vector<float>> stream = SubStream(seconds);
  const size_t frames = static_cast<size_t>(seconds) * kRate;
  // The 48 kHz microphone: every 12th sample is its own value repeated (a
  // zero-order hold, integer indices only: the samples stay the stream's).
  std::vector<float> cap_l(frames), cap_r(frames), ren_l(frames),
      ren_r(frames);
  for (size_t i = 0; i < frames; ++i) {
    const size_t src = i * kStreamRate / kRate;
    cap_l[i] = stream[0][src];
    cap_r[i] = stream[1][src];
    const size_t echo = i >= 144 ? (i - 144) * kStreamRate / kRate : 0;
    uint32_t b;
    std::memcpy(&b, &stream[0][echo], sizeof(b));
    // 1/8 by the exponent where it is a normal float, else the value itself.
    const uint32_t exp = (b >> 23) & 0xffu;
    ren_l[i] = exp > 3u ? SubFromBits(b - (3u << 23)) : SubFromBits(b);
    ren_r[i] = cap_r[i];
  }
  const webrtc::StreamConfig in_cfg(kRate, 2), out_cfg(kRate, 1),
      ren_cfg(kRate, 2);
  std::vector<float> out_l(kFrame), out_r(kFrame);
  uint64_t hash = 1469598103934665603ull;
  for (size_t at = 0; at + kFrame <= frames; at += kFrame) {
    const float* rp[2] = {&ren_l[at], &ren_r[at]};
    apm.AnalyzeReverseStream(rp, ren_cfg);
    apm.set_stream_delay_ms(30);
    apm.set_stream_analog_level(255);
    const float* cp[2] = {&cap_l[at], &cap_r[at]};
    float* op[2] = {out_l.data(), out_r.data()};
    apm.ProcessStream(cp, in_cfg, out_cfg, op);
    apm.recommended_stream_analog_level();
    SubFnv(hash, out_l);
  }
  return hash;
}
// END SUBNORMAL SIGNALS

constexpr uint64_t kGoldenResampled = 0x5bdae71354ef8eb6ull;
constexpr uint64_t kGoldenProcessed = 0x2f8541cffc1ffd45ull;

// Puts the calling thread in the flush-to-zero mode Chromium's disablers set
// (x86: MXCSR FTZ and DAZ; arm64: FPCR.FZ) while it lives, as a platform's
// audio thread may run, and puts back the mode it found.
class ScopedThreadFlushMode {
 public:
  ScopedThreadFlushMode() {
#if defined(ARCH_CPU_X86_64)
    saved_ = _mm_getcsr();
    _mm_setcsr(saved_ | 0x8040u);
#elif defined(ARCH_CPU_ARM64)
    uint64_t control;
    asm volatile("mrs %x[c], fpcr" : [c] "=r"(control));
    saved_ = control;
    control |= uint64_t{1} << 24;
    asm volatile("msr fpcr, %x[c]" : : [c] "r"(control));
#endif
  }
  ~ScopedThreadFlushMode() {
#if defined(ARCH_CPU_X86_64)
    _mm_setcsr(static_cast<unsigned int>(saved_));
#elif defined(ARCH_CPU_ARM64)
    asm volatile("msr fpcr, %x[c]" : : [c] "r"(saved_));
#endif
  }

 private:
  uint64_t saved_ = 0;
};

webrtc::scoped_refptr<webrtc::AudioProcessing> CreateCpuIndependent() {
  AudioProcessingSettings settings;
  settings.echo_cancellation = true;
  settings.noise_suppression = true;
  settings.automatic_gain_control = true;
  settings.cpu_independent = true;
  return CreateWebRtcAudioProcessingModule(settings, nullptr).first;
}

TEST(MoltPortableSubnormalsTest, GoldenResampled) {
  const uint64_t digest = ResampledDigest(3);
  EXPECT_EQ(digest, kGoldenResampled) << std::hex << digest;
}

TEST(MoltPortableSubnormalsTest, GoldenProcessed) {
  auto apm = CreateCpuIndependent();
  ASSERT_TRUE(apm);
  const uint64_t digest = ProcessedDigest(*apm, 3);
  EXPECT_EQ(digest, kGoldenProcessed) << std::hex << digest;
}

// A flush mode the calling thread was left in changes nothing: the portable
// code turns it off for its loops, and puts it back after.
TEST(MoltPortableSubnormalsTest, CallersFlushModeChangesNothing) {
  const uint64_t resampled = ResampledDigest(2);
  auto apm = CreateCpuIndependent();
  ASSERT_TRUE(apm);
  const uint64_t processed = ProcessedDigest(*apm, 2);
  {
    ScopedThreadFlushMode flush;
    EXPECT_EQ(ResampledDigest(2), resampled);
    auto apm_in_flush_mode = CreateCpuIndependent();
    ASSERT_TRUE(apm_in_flush_mode);
    EXPECT_EQ(ProcessedDigest(*apm_in_flush_mode, 2), processed);
#if defined(ARCH_CPU_X86_64) || defined(ARCH_CPU_ARM64)
    EXPECT_TRUE(ScopedPortableSubnormals::FlushModeIsOn());
#endif
  }
  EXPECT_FALSE(ScopedPortableSubnormals::FlushModeIsOn());
}

}  // namespace
}  // namespace media
