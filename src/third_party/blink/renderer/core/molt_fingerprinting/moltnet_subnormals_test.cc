// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: Web Audio's subnormal floats in MoltNet's renderers (the
// portable media process; platform/audio/denormal_disabler.h). There the
// rendering threads' DenormalDisabler leaves the CPU's flush-to-zero modes
// off, so IEEE 754 subnormals are computed the same way on x86 and Arm, and
// the software flush of filter states works at the portable floor (2^-63).
// Normal windows keep Chromium's: the modes on, no software flush where the
// hardware flushes.

#include <array>
#include <cfloat>
#include <cmath>

#include "build/build_config.h"
#include "media/base/molt_portable_media.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/renderer/platform/audio/audio_array.h"
#include "third_party/blink/renderer/platform/audio/denormal_disabler.h"
#include "third_party/blink/renderer/platform/audio/iir_filter.h"

namespace blink {
namespace {

class MoltNetSubnormalsTest : public testing::Test {
 protected:
  void SetUp() override { media::SetPortableMediaProcessForTesting(false); }
  void TearDown() override { media::SetPortableMediaProcessForTesting(false); }
};

// Half of FLT_MIN, computed at run time in the calling thread's mode: a
// subnormal with the flush-to-zero modes off, zero with them on.
float HalfOfFltMin() {
  volatile float smallest_normal = FLT_MIN;
  volatile float half = 0.5f;
  return smallest_normal * half;
}

TEST_F(MoltNetSubnormalsTest, NormalWindowsFlushAsChromiumDoes) {
  EXPECT_EQ(HalfOfFltMin(), FLT_MIN / 2);
#if defined(ARCH_CPU_X86_FAMILY) || defined(ARCH_CPU_ARM_FAMILY)
  {
    DenormalDisabler disabler;
    EXPECT_EQ(HalfOfFltMin(), 0.0f);
    // The hardware flushes: no software flush.
    EXPECT_EQ(DenormalDisabler::FlushDenormalFloatToZero(1e-30f), 1e-30f);
  }
#endif
  EXPECT_EQ(HalfOfFltMin(), FLT_MIN / 2);
}

TEST_F(MoltNetSubnormalsTest, MoltNetComputesSubnormals) {
  media::SetPortableMediaProcessForTesting(true);
  {
    DenormalDisabler disabler;
    EXPECT_EQ(HalfOfFltMin(), FLT_MIN / 2);
  }
  EXPECT_EQ(HalfOfFltMin(), FLT_MIN / 2);
}

// A rendering thread a flush mode was left on (say by a DenormalDisabler
// opened before the process became portable) computes subnormals inside
// MoltNet's scope, and gets its mode back after.
TEST_F(MoltNetSubnormalsTest, MoltNetTurnsAFlushModeOffAndBack) {
#if defined(ARCH_CPU_X86_FAMILY) || defined(ARCH_CPU_ARM_FAMILY)
  DenormalDisabler outer;
  EXPECT_EQ(HalfOfFltMin(), 0.0f);
  media::SetPortableMediaProcessForTesting(true);
  {
    DenormalDisabler inner;
    EXPECT_EQ(HalfOfFltMin(), FLT_MIN / 2);
  }
  EXPECT_EQ(HalfOfFltMin(), 0.0f);
#endif
}

TEST_F(MoltNetSubnormalsTest, MoltNetFlushesStatesAtTheFloor) {
  media::SetPortableMediaProcessForTesting(true);
  EXPECT_EQ(DenormalDisabler::FlushDenormalFloatToZero(1e-30f), 0.0f);
  EXPECT_EQ(DenormalDisabler::FlushDenormalFloatToZero(FLT_MIN / 4), 0.0f);
  const float negative = DenormalDisabler::FlushDenormalFloatToZero(-1e-30f);
  EXPECT_EQ(negative, 0.0f);
  EXPECT_TRUE(std::signbit(negative));
  EXPECT_EQ(DenormalDisabler::FlushDenormalFloatToZero(0x1p-63f), 0x1p-63f);
  EXPECT_EQ(DenormalDisabler::FlushDenormalFloatToZero(-0x1p-63f), -0x1p-63f);
  EXPECT_EQ(DenormalDisabler::FlushDenormalFloatToZero(0.25f), 0.25f);
  EXPECT_TRUE(std::isinf(DenormalDisabler::FlushDenormalFloatToZero(INFINITY)));
}

// An IIRFilterNode's filter: one render quantum of `input` through
// y[n] = b0 x[n] + b1 x[n-1] - a1 y[n-1].
std::array<float, 128> FilterQuantum(double b0,
                                     double b1,
                                     double a1,
                                     const std::array<float, 128>& input) {
  AudioDoubleArray feedforward(2);
  feedforward[0] = b0;
  feedforward[1] = b1;
  AudioDoubleArray feedback(2);
  feedback[0] = 1;
  feedback[1] = a1;
  IIRFilter filter(&feedforward, &feedback);
  std::array<float, 128> output = {};
  filter.Process(input, output);
  return output;
}

// The filter's double-precision history (its outputs and inputs) in MoltNet:
// zero below the portable floor, as the other nodes' states, so an impulse's
// tail settles at zero once it drops below 2^-63; normal windows keep the
// whole tail.
TEST_F(MoltNetSubnormalsTest, MoltNetFloorsTheIirFiltersHistory) {
  std::array<float, 128> impulse = {};
  impulse[0] = 1;
  // y[n] = 0.5^n.
  std::array<float, 128> normal = FilterQuantum(1, 0, -0.5, impulse);
  EXPECT_EQ(normal[63], 0x1p-63f);
  EXPECT_EQ(normal[64], 0x1p-64f);
  EXPECT_EQ(normal[127], 0x1p-127f);

  media::SetPortableMediaProcessForTesting(true);
  std::array<float, 128> moltnet = FilterQuantum(1, 0, -0.5, impulse);
  for (int n = 0; n <= 63; ++n) {
    EXPECT_EQ(moltnet[n], normal[n]) << n;
  }
  for (int n = 64; n < 128; ++n) {
    EXPECT_EQ(moltnet[n], 0.0f) << n;
  }

  // An input below the floor (a sum of nodes' outputs can cancel to one) is
  // kept as zero: y[n] = x[n-1].
  std::array<float, 128> tiny = {};
  tiny[0] = 0x1p-70f;
  EXPECT_EQ(FilterQuantum(0, 1, 0, tiny)[1], 0.0f);
  media::SetPortableMediaProcessForTesting(false);
  EXPECT_EQ(FilterQuantum(0, 1, 0, tiny)[1], 0x1p-70f);
}

// The tail time Web Audio computes for the node (an impulse response down to
// 1/32768) is the same with the floor.
TEST_F(MoltNetSubnormalsTest, IirTailTimeKeepsItsValue) {
  AudioDoubleArray feedforward(1);
  feedforward[0] = 1;
  AudioDoubleArray feedback(2);
  feedback[0] = 1;
  feedback[1] = -0.999;
  IIRFilter normal_filter(&feedforward, &feedback);
  const double normal = normal_filter.TailTime(48000, true, 128);
  media::SetPortableMediaProcessForTesting(true);
  IIRFilter moltnet_filter(&feedforward, &feedback);
  EXPECT_EQ(moltnet_filter.TailTime(48000, true, 128), normal);
  EXPECT_GT(normal, 0.0);
}

}  // namespace
}  // namespace blink
