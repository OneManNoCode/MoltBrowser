// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/runtime/model_image_prep.h"

#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/thread_pool.h"
#include "chrome/services/molt_ai/inference/engine/image_input.h"
#include "services/data_decoder/public/cpp/decode_image.h"
#include "skia/ext/image_operations.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "ui/gfx/geometry/size.h"

namespace molt_ai {

namespace {

void PrepareDecoded(uint32_t max_side,
                    ModelImageCallback callback,
                    const SkBitmap& bitmap) {
  if (bitmap.drawsNothing()) {
    std::move(callback).Run(std::nullopt);
    return;
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(&PrepareImageForModel, bitmap, max_side),
      std::move(callback));
}

}  // namespace

std::optional<ModelImage> PrepareImageForModel(const SkBitmap& bitmap,
                                               uint32_t max_side) {
  if (bitmap.drawsNothing() || bitmap.width() <= 0 || bitmap.height() <= 0) {
    return std::nullopt;
  }
  const std::optional<ImagePlan> plan =
      PlanModelImage(static_cast<uint32_t>(bitmap.width()),
                     static_cast<uint32_t>(bitmap.height()), max_side);
  if (!plan) {
    return std::nullopt;
  }
  const int width = static_cast<int>(plan->content_width);
  const int height = static_cast<int>(plan->content_height);

  // Skia's resampler takes N32 pixels; anything else is converted first.
  SkBitmap source = bitmap;
  if (source.colorType() != kN32_SkColorType) {
    SkBitmap n32;
    if (!n32.tryAllocPixels(
            source.info().makeColorType(kN32_SkColorType)) ||
        !source.readPixels(n32.pixmap(), 0, 0)) {
      return std::nullopt;
    }
    source = std::move(n32);
  }
  SkBitmap scaled = source;
  if (width != source.width() || height != source.height()) {
    // A Lanczos filter: sharp enough for text in screenshots.
    scaled = skia::ImageOperations::Resize(
        source, skia::ImageOperations::RESIZE_BEST, width, height);
    if (scaled.drawsNothing() || scaled.width() != width ||
        scaled.height() != height) {
      return std::nullopt;
    }
  }

  // RGBA with unpremultiplied alpha, whatever the bitmap had; then the
  // canvas (engine/image_input.h does the arithmetic, checked).
  const SkImageInfo rgba_info = SkImageInfo::Make(
      width, height, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
  const size_t row_bytes = rgba_info.minRowBytes();
  std::vector<uint8_t> rgba(rgba_info.computeByteSize(row_bytes));
  if (rgba.empty() ||
      !scaled.readPixels(rgba_info, rgba.data(), row_bytes, 0, 0)) {
    return std::nullopt;
  }
  std::optional<std::vector<uint8_t>> rgb =
      ComposeModelImage(*plan, rgba.data(), row_bytes, rgba.size());
  if (!rgb) {
    return std::nullopt;
  }
  ModelImage image(plan->canvas_width, plan->canvas_height, std::move(*rgb));
  if (!image.IsValid()) {
    return std::nullopt;
  }
  return image;
}

void DecodeImageForModel(base::span<const uint8_t> encoded,
                         uint32_t max_side,
                         ModelImageCallback callback) {
  // shrink_to_fit: the decoder process scales an image over the size cap
  // down itself, so a huge image never reaches the browser at full size.
  data_decoder::DecodeImageIsolated(
      encoded, data_decoder::mojom::ImageCodec::kDefault,
      /*shrink_to_fit=*/true, data_decoder::kDefaultMaxSizeInBytes,
      gfx::Size(),
      base::BindOnce(&PrepareDecoded, max_side, std::move(callback)));
}

}  // namespace molt_ai
