// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_pixels.h"

#include "media/base/molt_portable_media.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/molt_fingerprinting.h"

namespace blink::molt_fingerprinting {
namespace {

// kUniform can't be configured here (it pins the process's time zone and
// locale for every later test), so MoltNet's part is called directly. What
// the portable mode changes is tested in media_unittests
// (molt_portable_media_unittest.cc).
class MoltNetPixelsTest : public testing::Test {
 protected:
  void SetUp() override {
    ResetForTesting();
    media::SetPortableMediaProcessForTesting(false);
  }
  void TearDown() override {
    media::SetPortableMediaProcessForTesting(false);
    ResetForTesting();
  }
};

TEST_F(MoltNetPixelsTest, NormalWindowsKeepTheMachinesMedia) {
  for (const Mode mode : {Mode::kOff, Mode::kFarble}) {
    ResetForTesting();
    Configure(mode);
    EXPECT_FALSE(media::IsPortableMediaProcess());
  }
}

TEST_F(MoltNetPixelsTest, MoltNetMakesTheProcessPortable) {
  PinUniformMediaPixels();
  EXPECT_TRUE(media::IsPortableMediaProcess());
  // Idempotent: Configure() runs once, but nothing breaks if it ran again.
  PinUniformMediaPixels();
  EXPECT_TRUE(media::IsPortableMediaProcess());
}

}  // namespace
}  // namespace blink::molt_fingerprinting
