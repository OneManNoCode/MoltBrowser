// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/fingerprinting/molt_net_display_capture_policy.h"

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai::fingerprinting {
namespace {

using T = blink::mojom::MediaStreamType;

TEST(MoltNetDisplayCapturePolicyTest, CamerasAndMicrophonesAreNotRefused) {
  for (T type :
       {T::NO_SERVICE, T::DEVICE_AUDIO_CAPTURE, T::DEVICE_VIDEO_CAPTURE}) {
    EXPECT_FALSE(IsDisplayCaptureType(type)) << static_cast<int>(type);
  }
  EXPECT_FALSE(IsDisplayCaptureRequest(T::DEVICE_AUDIO_CAPTURE,
                                       T::DEVICE_VIDEO_CAPTURE));
  EXPECT_FALSE(IsDisplayCaptureRequest(T::NO_SERVICE, T::NO_SERVICE));
}

TEST(MoltNetDisplayCapturePolicyTest, EveryScreenWindowAndTabTypeIs) {
  for (T type :
       {T::GUM_TAB_AUDIO_CAPTURE, T::GUM_TAB_VIDEO_CAPTURE,
        T::GUM_DESKTOP_VIDEO_CAPTURE, T::GUM_DESKTOP_AUDIO_CAPTURE,
        T::DISPLAY_VIDEO_CAPTURE, T::DISPLAY_AUDIO_CAPTURE,
        T::DISPLAY_VIDEO_CAPTURE_THIS_TAB, T::DISPLAY_VIDEO_CAPTURE_SET}) {
    EXPECT_TRUE(IsDisplayCaptureType(type)) << static_cast<int>(type);
  }
  // Either part is enough: getDisplayMedia() with audio, a tab's audio
  // alone, a microphone with a screen.
  EXPECT_TRUE(IsDisplayCaptureRequest(T::DISPLAY_AUDIO_CAPTURE,
                                      T::DISPLAY_VIDEO_CAPTURE));
  EXPECT_TRUE(IsDisplayCaptureRequest(T::GUM_TAB_AUDIO_CAPTURE, T::NO_SERVICE));
  EXPECT_TRUE(IsDisplayCaptureRequest(T::DEVICE_AUDIO_CAPTURE,
                                      T::GUM_DESKTOP_VIDEO_CAPTURE));
  // A value no known type has (a renderer can send any number).
  EXPECT_TRUE(IsDisplayCaptureType(static_cast<T>(99)));
}

TEST(MoltNetDisplayCapturePolicyTest, RefusedAsAUsersCancel) {
  // The picker's answer when the user cancels it, which Blink turns into
  // NotAllowedError "Permission denied by user".
  static_assert(
      kDisplayCaptureRefusal ==
      blink::mojom::MediaStreamRequestResult::PERMISSION_DENIED_BY_USER);
  static_assert(IsDisplayCaptureType(T::DISPLAY_VIDEO_CAPTURE));
  static_assert(!IsDisplayCaptureType(T::DEVICE_VIDEO_CAPTURE));
}

}  // namespace
}  // namespace molt_ai::fingerprinting
