// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_display.h"

#include <atomic>

#include "third_party/blink/renderer/core/molt_fingerprinting/farbling.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/fonts.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/molt_fingerprinting.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_values.h"

namespace blink::molt_fingerprinting {

namespace {

// The media queries' colour agrees with screen.colorDepth (Screen::colorDepth
// reports kUniformColorDepth in MoltNet).
static_assert(3 * moltnet::kDisplayColorBitsPerComponent ==
              static_cast<int>(kUniformColorDepth));

std::atomic<bool> g_display_for_testing{false};

bool UniformMode() {
  return IsUniform() || g_display_for_testing.load(std::memory_order_relaxed);
}

ColorSpaceGamut ToColorSpaceGamut(moltnet::DisplayGamut gamut) {
  switch (gamut) {
    case moltnet::DisplayGamut::kSRGB:
      return ColorSpaceGamut::SRGB;
    case moltnet::DisplayGamut::kP3:
      return ColorSpaceGamut::P3;
    case moltnet::DisplayGamut::kRec2020:
      return ColorSpaceGamut::BT2020;
  }
  return ColorSpaceGamut::SRGB;
}

}  // namespace

std::optional<ColorSpaceGamut> UniformDisplayColorGamut() {
  if (!UniformMode()) {
    return std::nullopt;
  }
  return ToColorSpaceGamut(moltnet::kDisplayGamut);
}

std::optional<bool> UniformDisplaySupportsHDR() {
  if (!UniformMode()) {
    return std::nullopt;
  }
  return moltnet::kDisplayHighDynamicRange;
}

std::optional<int> UniformDisplayColorBitsPerComponent() {
  if (!UniformMode()) {
    return std::nullopt;
  }
  return moltnet::kDisplayColorBitsPerComponent;
}

std::optional<int> UniformDisplayMonochromeBitsPerComponent() {
  if (!UniformMode()) {
    return std::nullopt;
  }
  return moltnet::kDisplayMonochromeBitsPerComponent;
}

bool UniformOverlayScrollbars(bool requested) {
  if (!UniformMode()) {
    return requested;
  }
  return moltnet::UsesOverlayScrollbars(CurrentFontPlatform());
}

void SetMoltNetDisplayForTesting(bool uniform) {
  g_display_for_testing.store(uniform, std::memory_order_relaxed);
}

}  // namespace blink::molt_fingerprinting
