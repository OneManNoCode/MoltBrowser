// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/services/molt_ai/inference/engine/image_input.h"

#include <algorithm>

namespace molt_ai {

namespace {

static_assert(kMinImageSide >= 1 && kMinImageSide <= kMaxImageSide);
static_assert(kMaxImageAspectRatio >= 1);

// (c * a + pad * (255 - a)) / 255, rounded: c over the pad colour.
uint8_t OverPad(uint32_t c, uint32_t a) {
  return static_cast<uint8_t>(
      (c * a + uint32_t{kImagePadValue} * (255 - a) + 127) / 255);
}

}  // namespace

ImageCheck CheckModelImage(uint32_t width,
                           uint32_t height,
                           size_t byte_length) {
  if (width < kMinImageSide || width > kMaxImageSide ||
      height < kMinImageSide || height > kMaxImageSide) {
    return ImageCheck::kBadSize;
  }
  const uint64_t long_side = std::max(width, height);
  const uint64_t short_side = std::min(width, height);
  if (long_side > short_side * kMaxImageAspectRatio) {
    return ImageCheck::kBadAspectRatio;
  }
  // At most 1024 * 1024 * 3: no overflow.
  const uint64_t expected =
      uint64_t{width} * uint64_t{height} * kImageBytesPerPixel;
  if (static_cast<uint64_t>(byte_length) != expected) {
    return ImageCheck::kBadByteLength;
  }
  return ImageCheck::kOk;
}

std::optional<ImagePlan> PlanModelImage(uint32_t src_width,
                                        uint32_t src_height,
                                        uint32_t max_side) {
  if (src_width == 0 || src_height == 0 || max_side < kMinImageSide ||
      max_side > kMaxImageSide) {
    return std::nullopt;
  }
  const bool landscape = src_width >= src_height;
  const uint64_t src_long = landscape ? src_width : src_height;
  const uint64_t src_short = landscape ? src_height : src_width;

  // Scale down (never up) so the long side fits. src_short * max_side * 2
  // is below 2^43 and src_long * 2 below 2^33: no overflow in 64 bits.
  uint64_t long_side = src_long;
  uint64_t short_side = src_short;
  if (src_long > max_side) {
    long_side = max_side;
    short_side = std::max<uint64_t>(
        1, (src_short * max_side * 2 + src_long) / (src_long * 2));
  }

  // Pad: every side at least kMinImageSide, and the short side at least
  // 1 / kMaxImageAspectRatio of the long one. None of the three terms
  // exceeds canvas_long, so the canvas keeps its orientation.
  const uint64_t canvas_long = std::max<uint64_t>(long_side, kMinImageSide);
  const uint64_t canvas_short = std::max<uint64_t>(
      {short_side,
       (canvas_long + kMaxImageAspectRatio - 1) / kMaxImageAspectRatio,
       uint64_t{kMinImageSide}});

  ImagePlan plan;
  plan.content_width = static_cast<uint32_t>(landscape ? long_side : short_side);
  plan.content_height =
      static_cast<uint32_t>(landscape ? short_side : long_side);
  plan.canvas_width =
      static_cast<uint32_t>(landscape ? canvas_long : canvas_short);
  plan.canvas_height =
      static_cast<uint32_t>(landscape ? canvas_short : canvas_long);
  plan.offset_x = (plan.canvas_width - plan.content_width) / 2;
  plan.offset_y = (plan.canvas_height - plan.content_height) / 2;
  return plan;
}

std::optional<std::vector<uint8_t>> ComposeModelImage(
    const ImagePlan& plan,
    const uint8_t* content,
    size_t row_bytes,
    size_t content_bytes) {
  const uint64_t canvas_bytes = uint64_t{plan.canvas_width} *
                                uint64_t{plan.canvas_height} *
                                kImageBytesPerPixel;
  if (CheckModelImage(plan.canvas_width, plan.canvas_height,
                      static_cast<size_t>(canvas_bytes)) != ImageCheck::kOk) {
    return std::nullopt;
  }
  const uint32_t width = plan.content_width;
  const uint32_t height = plan.content_height;
  if (width == 0 || height == 0 ||
      uint64_t{plan.offset_x} + width > plan.canvas_width ||
      uint64_t{plan.offset_y} + height > plan.canvas_height || !content) {
    return std::nullopt;
  }
  // The last row ends at (height - 1) * row_bytes + width * 4; checked
  // without multiplying row_bytes (it may be anything).
  const uint64_t row_pixels_bytes = uint64_t{width} * 4;
  if (row_bytes < row_pixels_bytes || content_bytes < row_pixels_bytes) {
    return std::nullopt;
  }
  if (height > 1 &&
      row_bytes > (content_bytes - row_pixels_bytes) / (height - 1)) {
    return std::nullopt;
  }

  std::vector<uint8_t> out(static_cast<size_t>(canvas_bytes), kImagePadValue);
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* src = content + static_cast<size_t>(y) * row_bytes;
    uint8_t* dst = out.data() +
                   (static_cast<size_t>(plan.offset_y + y) * plan.canvas_width +
                    plan.offset_x) *
                       kImageBytesPerPixel;
    for (uint32_t x = 0; x < width; ++x) {
      const uint32_t a = src[3];
      dst[0] = OverPad(src[0], a);
      dst[1] = OverPad(src[1], a);
      dst[2] = OverPad(src[2], a);
      src += 4;
      dst += kImageBytesPerPixel;
    }
  }
  return out;
}

}  // namespace molt_ai
