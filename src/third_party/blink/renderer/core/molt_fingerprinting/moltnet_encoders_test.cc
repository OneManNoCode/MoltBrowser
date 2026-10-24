// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_encoders.h"

#include <algorithm>

#include "base/system/sys_info.h"
#include "media/base/media_types.h"
#include "media/base/supported_types.h"
#include "media/base/video_codecs.h"
#include "media/base/video_encoder.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/farbling.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/molt_fingerprinting.h"
#include "ui/gfx/geometry/size.h"

namespace blink::molt_fingerprinting {
namespace {

// The video types MediaRecorder's isTypeSupported() checks.
constexpr media::VideoType kTypes[] = {
    {media::VideoCodec::kH264, media::H264PROFILE_BASELINE},
    {media::VideoCodec::kH264, media::H264PROFILE_HIGH},
    {media::VideoCodec::kAV1, media::AV1PROFILE_PROFILE_MAIN},
    {media::VideoCodec::kAV1, media::AV1PROFILE_PROFILE_PRO},
    {media::VideoCodec::kHEVC, media::HEVCPROFILE_MAIN},
};

// kUniform can't be configured here (it pins the process's time zone and
// locale for every later test), so MoltNet is simulated with
// SetMoltNetEncodersForTesting().
class MoltNetEncodersTest : public testing::Test {
 protected:
  void SetUp() override {
    ResetForTesting();
    SetMoltNetEncodersForTesting(false);
  }
  void TearDown() override {
    SetMoltNetEncodersForTesting(false);
    media::SetProcessorCountForSoftwareEncoding(0);
    ResetForTesting();
  }
};

TEST_F(MoltNetEncodersTest, NormalWindowsUseThePlatform) {
  for (const Mode mode : {Mode::kOff, Mode::kFarble}) {
    ResetForTesting();
    Configure(mode);
    EXPECT_TRUE(MediaRecorderUsesPlatformEncoders());
    for (const media::VideoType& type : kTypes) {
      EXPECT_EQ(IsMediaRecorderVideoTypeSupported(type),
                media::IsEncoderSupportedVideoType(type));
    }
  }
}

TEST_F(MoltNetEncodersTest, MoltNetUsesTheBuildsEncoders) {
  SetMoltNetEncodersForTesting(true);
  EXPECT_FALSE(MediaRecorderUsesPlatformEncoders());
  for (const media::VideoType& type : kTypes) {
    EXPECT_EQ(IsMediaRecorderVideoTypeSupported(type),
              media::IsEncoderBuiltInVideoType(type));
  }
  // Never the platform's HEVC encoder.
  EXPECT_FALSE(IsMediaRecorderVideoTypeSupported(
      {media::VideoCodec::kHEVC, media::HEVCPROFILE_MAIN}));
}

TEST_F(MoltNetEncodersTest, SoftwareEncodersCountTheUniformCores) {
  const int real = base::SysInfo::NumberOfProcessors();
  // Below VGA one thread, 720p four, 1080p eight, 2160p sixteen, each at
  // most the processor count.
  EXPECT_EQ(media::GetNumberOfThreadsForSoftwareEncoding(gfx::Size(320, 240)),
            1);
  EXPECT_EQ(
      media::GetNumberOfThreadsForSoftwareEncoding(gfx::Size(3840, 2160)),
      std::min(16, real));

  PinUniformSoftwareEncoding();
  const int uniform = static_cast<int>(kUniformHardwareConcurrency);
  EXPECT_EQ(media::GetNumberOfThreadsForSoftwareEncoding(gfx::Size(320, 240)),
            1);
  EXPECT_EQ(media::GetNumberOfThreadsForSoftwareEncoding(gfx::Size(640, 480)),
            std::min(2, uniform));
  EXPECT_EQ(media::GetNumberOfThreadsForSoftwareEncoding(gfx::Size(1280, 720)),
            std::min(4, uniform));
  EXPECT_EQ(
      media::GetNumberOfThreadsForSoftwareEncoding(gfx::Size(1920, 1080)),
      uniform);
  EXPECT_EQ(
      media::GetNumberOfThreadsForSoftwareEncoding(gfx::Size(3840, 2160)),
      uniform);

  // Back to the machine's.
  media::SetProcessorCountForSoftwareEncoding(0);
  EXPECT_EQ(
      media::GetNumberOfThreadsForSoftwareEncoding(gfx::Size(3840, 2160)),
      std::min(16, real));
}

}  // namespace
}  // namespace blink::molt_fingerprinting
