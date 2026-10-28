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

#include <cfloat>
#include <cmath>

#include "build/build_config.h"
#include "media/base/molt_portable_media.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/renderer/platform/audio/denormal_disabler.h"

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

}  // namespace
}  // namespace blink
