// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Image input for the on-device model (docs/design/SANDBOXED_INFERENCE.md,
// section 10). An image reaches the sandboxed inference service only as
// already-decoded 8-bit RGB pixels within fixed bounds; nothing in the
// service decodes a compressed image. This file is the one definition of
// those bounds and of how an image of any size is brought within them:
//
//  - the browser (chrome/browser/molt_ai/runtime/model_image.h) decodes with
//    Chromium's sandboxed decoders, scales the image down with Skia to
//    PlanModelImage()'s content size and composes the canvas with
//    ComposeModelImage();
//  - the service checks every image again with CheckModelImage() (a
//    violation is a bad message), and so does LlamaEngine before handing the
//    pixels to llama.cpp's multimodal library.
//
// The bounds keep llama.cpp's image preprocessing (resizing to the vision
// encoder's input size, tiling) away from degenerate shapes: no side under
// kMinImageSide, none over kMaxImageSide, no aspect ratio beyond
// kMaxImageAspectRatio:1.
//
// Std-only, unit-tested in image_input_unittest.cc.

#ifndef CHROME_SERVICES_MOLT_AI_INFERENCE_ENGINE_IMAGE_INPUT_H_
#define CHROME_SERVICES_MOLT_AI_INFERENCE_ENGINE_IMAGE_INPUT_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace molt_ai {

// Must match molt_inference.mojom (static_asserts in molt_model_impl.cc).
inline constexpr uint32_t kMaxImageSide = 1024;
inline constexpr uint32_t kMinImageSide = 32;
// The long side is at most this many times the short side.
inline constexpr uint32_t kMaxImageAspectRatio = 8;
// Images in one request (the new message and the history together).
inline constexpr size_t kMaxImagesPerRequest = 4;
inline constexpr size_t kImageBytesPerPixel = 3;  // R, G, B
inline constexpr size_t kMaxImageBytes =
    size_t{kMaxImageSide} * kMaxImageSide * kImageBytesPerPixel;

// The canvas around an image that doesn't fill it, and what a transparent
// pixel is composited over: neutral grey.
inline constexpr uint8_t kImagePadValue = 128;

enum class ImageCheck {
  kOk,
  // A side outside [kMinImageSide, kMaxImageSide].
  kBadSize,
  // The long side is more than kMaxImageAspectRatio times the short side.
  kBadAspectRatio,
  // The pixel buffer isn't exactly width * height * 3 bytes.
  kBadByteLength,
};

// Whether an RGB image of |width| x |height| in a buffer of |byte_length|
// bytes (row-major, no row padding) may be given to the model.
ImageCheck CheckModelImage(uint32_t width,
                           uint32_t height,
                           size_t byte_length);

// How a decoded image becomes a model image.
struct ImagePlan {
  // The image scaled down (never up) so its long side is at most the
  // caller's |max_side|; the aspect ratio is kept, each side at least 1.
  uint32_t content_width = 0;
  uint32_t content_height = 0;
  // The model image: the content centred on a kImagePadValue background,
  // padded to at least kMinImageSide a side and to at most
  // kMaxImageAspectRatio:1. Always passes CheckModelImage().
  uint32_t canvas_width = 0;
  uint32_t canvas_height = 0;
  // Where the content goes on the canvas.
  uint32_t offset_x = 0;
  uint32_t offset_y = 0;
};

// The plan for a decoded image of |src_width| x |src_height| (any size of at
// least 1 x 1), with the long side of the result at most |max_side|, which
// must be within [kMinImageSide, kMaxImageSide]. nullopt for an empty source
// or a |max_side| outside that range.
std::optional<ImagePlan> PlanModelImage(uint32_t src_width,
                                        uint32_t src_height,
                                        uint32_t max_side);

// Composes the model image (canvas_width * canvas_height * 3 bytes of RGB)
// from |content|: plan.content_width x plan.content_height pixels of 8-bit
// RGBA with unpremultiplied alpha, |row_bytes| apart, in a buffer of
// |content_bytes| bytes. Transparent pixels are composited over
// kImagePadValue. nullopt if the buffer doesn't hold the pixels the plan
// says (row_bytes too small, or the buffer too short).
std::optional<std::vector<uint8_t>> ComposeModelImage(
    const ImagePlan& plan,
    const uint8_t* content,
    size_t row_bytes,
    size_t content_bytes);

}  // namespace molt_ai

#endif  // CHROME_SERVICES_MOLT_AI_INFERENCE_ENGINE_IMAGE_INPUT_H_
