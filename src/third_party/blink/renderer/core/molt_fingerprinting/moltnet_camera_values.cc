// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_camera_values.h"

#include <algorithm>
#include <cstdint>
#include <iterator>

namespace blink::molt_fingerprinting::moltnet {

namespace {

constexpr bool MenuIsOrdered() {
  for (size_t i = 1; i < std::size(kUniformCameraMenu); ++i) {
    if (kUniformCameraMenu[i].width < kUniformCameraMenu[i - 1].width ||
        kUniformCameraMenu[i].height < kUniformCameraMenu[i - 1].height) {
      return false;
    }
  }
  return true;
}
static_assert(MenuIsOrdered(),
              "each entry of the menu is at least as wide and as tall as the "
              "one before");
static_assert(kUniformCameraMenu[0].frame_rate == kUniformCameraFrameRate &&
                  kUniformCameraMenu[1].frame_rate == kUniformCameraFrameRate,
              "every entry runs at the menu's rate");

bool HasSize(const CameraMode& mode) {
  return mode.width > 0 && mode.height > 0;
}

// False for NaN.
bool IsFast(float frame_rate) {
  return frame_rate >= kUniformCameraFastFrameRate;
}

bool Reaches(const CameraMode& mode, const CameraMode& entry) {
  return mode.width >= entry.width && mode.height >= entry.height;
}

// The largest entry, which the mode a camera opens in reaches if it can.
constexpr const CameraMode& kLargestEntry =
    kUniformCameraMenu[std::size(kUniformCameraMenu) - 1];

// Whether the modes a camera goes by are its fast ones: it has one.
bool GoesByFastModes(std::span<const CameraMode> modes) {
  return std::ranges::any_of(modes, [](const CameraMode& mode) {
    return HasSize(mode) && IsFast(mode.frame_rate);
  });
}

bool GoesBy(const CameraMode& mode, bool fast_modes) {
  return HasSize(mode) && (!fast_modes || IsFast(mode.frame_rate));
}

// Whether one of the modes gone by reaches the largest entry.
bool ReachesLargestEntry(std::span<const CameraMode> modes, bool fast_modes) {
  return std::ranges::any_of(modes, [fast_modes](const CameraMode& mode) {
    return GoesBy(mode, fast_modes) && Reaches(mode, kLargestEntry);
  });
}

int64_t Area(const CameraMode& mode) {
  return int64_t{mode.width} * mode.height;
}

// Whether `a` is closer than `b` to the menu's rate from above: a fast rate
// beats a slow one, the lower of two fast ones and the higher of two slow
// ones win.
bool IsCloserRate(float a, float b) {
  if (IsFast(a) != IsFast(b)) {
    return IsFast(a);
  }
  return IsFast(a) ? a < b : a > b;
}

}  // namespace

std::span<const CameraMode> ShownUniformCameraModes(
    std::span<const CameraMode> modes) {
  if (std::ranges::none_of(modes, HasSize)) {
    return {};
  }
  return std::span<const CameraMode>(kUniformCameraMenu);
}

std::optional<size_t> UniformCameraOpenMode(
    std::span<const CameraMode> modes) {
  const bool fast_modes = GoesByFastModes(modes);
  const bool reaches_largest = ReachesLargestEntry(modes, fast_modes);
  std::optional<size_t> best;
  for (size_t i = 0; i < modes.size(); ++i) {
    const CameraMode& mode = modes[i];
    if (!GoesBy(mode, fast_modes) ||
        (reaches_largest && !Reaches(mode, kLargestEntry))) {
      continue;
    }
    if (!best) {
      best = i;
      continue;
    }
    const CameraMode& current = modes[*best];
    if (Area(mode) != Area(current)) {
      // The smallest that reaches the largest entry, or the largest of all
      // when none does.
      if ((Area(mode) < Area(current)) == reaches_largest) {
        best = i;
      }
      continue;
    }
    if (IsCloserRate(mode.frame_rate, current.frame_rate)) {
      best = i;
    }
  }
  return best;
}

float UniformCameraOpenFrameRate(const CameraMode& mode) {
  return mode.frame_rate > 0.0f && mode.frame_rate < kUniformCameraFrameRate
             ? mode.frame_rate
             : kUniformCameraFrameRate;
}

}  // namespace blink::molt_fingerprinting::moltnet
