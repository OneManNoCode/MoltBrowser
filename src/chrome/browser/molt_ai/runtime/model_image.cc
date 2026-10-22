// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/runtime/model_image.h"

#include <algorithm>
#include <utility>

#include "chrome/services/molt_ai/inference/engine/image_input.h"

namespace molt_ai {

static_assert(kWebImageMaxSide >= kMinImageSide &&
              kWebImageMaxSide <= kMaxImageSide);

ModelImage::ModelImage() = default;
ModelImage::ModelImage(uint32_t w, uint32_t h, std::vector<uint8_t> pixels)
    : width(w), height(h), rgb(std::move(pixels)) {}
ModelImage::~ModelImage() = default;
ModelImage::ModelImage(ModelImage&&) = default;
ModelImage& ModelImage::operator=(ModelImage&&) = default;
ModelImage::ModelImage(const ModelImage&) = default;
ModelImage& ModelImage::operator=(const ModelImage&) = default;

bool ModelImage::IsValid() const {
  return CheckModelImage(width, height, rgb.size()) == ImageCheck::kOk;
}

std::optional<std::vector<size_t>> PlanRequestImages(
    size_t user_images,
    const std::vector<size_t>& turn_images) {
  if (user_images > kMaxImagesPerRequest) {
    return std::nullopt;
  }
  std::vector<size_t> sent(turn_images.size(), 0);
  size_t room = kMaxImagesPerRequest - user_images;
  for (size_t i = turn_images.size(); i-- > 0 && room > 0;) {
    sent[i] = std::min(room, turn_images[i]);
    room -= sent[i];
  }
  return sent;
}

}  // namespace molt_ai
