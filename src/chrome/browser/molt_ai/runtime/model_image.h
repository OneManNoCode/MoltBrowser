// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Images for the on-device model (docs/design/SANDBOXED_INFERENCE.md,
// section 10). The sandboxed inference service never decodes a compressed
// image: the browser decodes it with Chromium's sandboxed decoders (or takes
// a bitmap that already is one: a page's ImageBitmap, a screenshot), scales
// it down, and sends raw RGB pixels within the bounds of
// chrome/services/molt_ai/inference/engine/image_input.h. The service checks
// them again.
//
// ModelImage itself is std-only (the web AI session context, which keeps a
// conversation's images, is unit-tested standalone); making one needs Skia
// (model_image_prep.h).

#ifndef CHROME_BROWSER_MOLT_AI_RUNTIME_MODEL_IMAGE_H_
#define CHROME_BROWSER_MOLT_AI_RUNTIME_MODEL_IMAGE_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace molt_ai {

// The longest side an image gets when a web page sends it (the Prompt API,
// docs/design/WEB_AI_APIS.md): 768 pixels, at most 576 tokens on a vision
// encoder with a 32-pixel token grid (Qwen-VL). The same for every user and
// model.
inline constexpr uint32_t kWebImageMaxSide = 768;

// An image ready for the model: 8-bit RGB, row-major, no row padding,
// width * height * 3 bytes, within the inference service's bounds. Untrusted
// content, like page text.
struct ModelImage {
  ModelImage();
  ModelImage(uint32_t width, uint32_t height, std::vector<uint8_t> rgb);
  ~ModelImage();
  ModelImage(ModelImage&&);
  ModelImage& operator=(ModelImage&&);
  ModelImage(const ModelImage&);
  ModelImage& operator=(const ModelImage&);

  // Within the bounds (engine/image_input.h CheckModelImage()).
  bool IsValid() const;

  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgb;
};

// Which images a chat request sends, at most kMaxImagesPerRequest
// (engine/image_input.h): all |user_images| of the new message (nullopt if
// they alone are more), then the newest history turns' while there is room.
// |turn_images| holds each history turn's image count, oldest turn first;
// the result holds, per turn, how many of its newest images are sent.
std::optional<std::vector<size_t>> PlanRequestImages(
    size_t user_images,
    const std::vector<size_t>& turn_images);

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_RUNTIME_MODEL_IMAGE_H_
