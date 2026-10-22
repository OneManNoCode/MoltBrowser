// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Image geometry for the on-device model (engine/image_input.h): the bounds
// the sandboxed service checks every image against, and how the browser
// brings a decoded image of any size (a web page's, an attachment's) within
// them. Sizes come from images a page controls; the service gets whatever a
// compromised browser-side caller sends.
//
// Checks: every plan PlanModelImage() makes passes CheckModelImage(), keeps
// the content inside the canvas, never scales up, stays within |max_side|
// and keeps the aspect ratio to the pixel; ComposeModelImage() reads only
// the bytes it is given (ASan) for any plan, row stride and buffer size,
// and makes exactly canvas_width * canvas_height * 3 bytes when it accepts.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <vector>

#include <fuzzer/FuzzedDataProvider.h>

#include "base/check.h"
#include "chrome/services/molt_ai/inference/engine/image_input.h"

namespace {

using molt_ai::ImagePlan;

void CheckPlan(uint32_t w, uint32_t h, uint32_t max_side,
               const std::optional<ImagePlan>& plan) {
  using namespace molt_ai;
  if (w == 0 || h == 0 || max_side < kMinImageSide ||
      max_side > kMaxImageSide) {
    CHECK(!plan);
    return;
  }
  CHECK(plan);
  const uint64_t canvas_bytes =
      uint64_t{plan->canvas_width} * plan->canvas_height * kImageBytesPerPixel;
  CHECK(CheckModelImage(plan->canvas_width, plan->canvas_height,
                        static_cast<size_t>(canvas_bytes)) == ImageCheck::kOk);
  CHECK(plan->content_width >= 1 && plan->content_height >= 1);
  CHECK(plan->content_width <= w && plan->content_height <= h);
  CHECK(std::max(plan->content_width, plan->content_height) <= max_side);
  CHECK(uint64_t{plan->offset_x} + plan->content_width <= plan->canvas_width);
  CHECK(uint64_t{plan->offset_y} + plan->content_height <= plan->canvas_height);
  if (std::max(w, h) <= max_side) {
    CHECK(plan->content_width == w && plan->content_height == h);
  } else {
    // The long side is max_side; the short side is the source's ratio,
    // rounded to the nearest pixel (at least 1).
    const bool landscape = w >= h;
    const uint64_t src_long = landscape ? w : h;
    const uint64_t src_short = landscape ? h : w;
    const uint64_t got_long =
        landscape ? plan->content_width : plan->content_height;
    const uint64_t got_short =
        landscape ? plan->content_height : plan->content_width;
    CHECK(got_long == max_side);
    // |got_short - src_short * max_side / src_long| <= 1/2, or 1 for the
    // floor at one pixel.
    const int64_t diff = static_cast<int64_t>(got_short * src_long * 2) -
                         static_cast<int64_t>(src_short * max_side * 2);
    CHECK(std::llabs(diff) <= static_cast<int64_t>(src_long) || got_short == 1);
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace molt_ai;
  FuzzedDataProvider provider(data, size);

  // Any decoded size; the browser's two |max_side| values and others.
  const uint32_t w = provider.ConsumeBool()
                         ? provider.ConsumeIntegral<uint32_t>()
                         : provider.ConsumeIntegralInRange<uint32_t>(0, 5000);
  const uint32_t h = provider.ConsumeBool()
                         ? provider.ConsumeIntegral<uint32_t>()
                         : provider.ConsumeIntegralInRange<uint32_t>(0, 5000);
  const uint32_t max_side = provider.ConsumeIntegralInRange<uint32_t>(0, 1100);
  const std::optional<ImagePlan> plan = PlanModelImage(w, h, max_side);
  CheckPlan(w, h, max_side, plan);

  // The service's check on any size and length.
  const uint32_t cw = provider.ConsumeIntegralInRange<uint32_t>(0, 1100);
  const uint32_t ch = provider.ConsumeIntegralInRange<uint32_t>(0, 1100);
  const size_t len = provider.ConsumeIntegral<size_t>();
  if (CheckModelImage(cw, ch, len) == ImageCheck::kOk) {
    CHECK(cw >= kMinImageSide && cw <= kMaxImageSide);
    CHECK(ch >= kMinImageSide && ch <= kMaxImageSide);
    CHECK(std::max(cw, ch) <= std::min(cw, ch) * kMaxImageAspectRatio);
    CHECK(len == size_t{cw} * ch * kImageBytesPerPixel);
  }

  // Composition: the planned geometry (small sources, so the buffer is
  // cheap) or a hostile one, with any stride and buffer size.
  ImagePlan compose;
  if (provider.ConsumeBool()) {
    const uint32_t sw = provider.ConsumeIntegralInRange<uint32_t>(1, 96);
    const uint32_t sh = provider.ConsumeIntegralInRange<uint32_t>(1, 96);
    std::optional<ImagePlan> p = PlanModelImage(sw, sh, kMinImageSide +
        provider.ConsumeIntegralInRange<uint32_t>(0, 64));
    CHECK(p);
    compose = *p;
  } else {
    compose.content_width = provider.ConsumeIntegralInRange<uint32_t>(0, 300);
    compose.content_height = provider.ConsumeIntegralInRange<uint32_t>(0, 300);
    compose.canvas_width = provider.ConsumeIntegralInRange<uint32_t>(0, 1100);
    compose.canvas_height = provider.ConsumeIntegralInRange<uint32_t>(0, 1100);
    compose.offset_x = provider.ConsumeIntegral<uint32_t>();
    compose.offset_y = provider.ConsumeIntegral<uint32_t>();
  }
  const uint64_t row_pixels = uint64_t{compose.content_width} * 4;
  size_t row_bytes;
  switch (provider.ConsumeIntegralInRange<int>(0, 3)) {
    case 0:
      row_bytes = static_cast<size_t>(row_pixels);
      break;
    case 1:
      row_bytes = static_cast<size_t>(row_pixels) +
                  provider.ConsumeIntegralInRange<size_t>(0, 64);
      break;
    case 2:
      row_bytes = provider.ConsumeIntegral<size_t>();
      break;
    default:
      row_bytes = provider.ConsumeIntegralInRange<size_t>(0, 2048);
      break;
  }
  // Up to 2 MB of pixels: the full image or a short buffer.
  const uint64_t full =
      compose.content_height == 0
          ? 0
          : row_pixels + uint64_t{compose.content_height - 1} *
                             std::min<uint64_t>(row_bytes, 1 << 21);
  size_t buffer_size = static_cast<size_t>(std::min<uint64_t>(full, 1 << 21));
  if (provider.ConsumeBool() && buffer_size > 0) {
    buffer_size -= provider.ConsumeIntegralInRange<size_t>(1, buffer_size);
  }
  std::vector<uint8_t> pixels(buffer_size);
  for (size_t i = 0; i < pixels.size() && provider.remaining_bytes() > 0;
       i += 97) {
    pixels[i] = provider.ConsumeIntegral<uint8_t>();
  }
  const std::optional<std::vector<uint8_t>> out = ComposeModelImage(
      compose, pixels.empty() ? nullptr : pixels.data(), row_bytes,
      pixels.size());
  if (out) {
    CHECK(out->size() == size_t{compose.canvas_width} *
                             compose.canvas_height * kImageBytesPerPixel);
    CHECK(CheckModelImage(compose.canvas_width, compose.canvas_height,
                          out->size()) == ImageCheck::kOk);
  }
  return 0;
}
