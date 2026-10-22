// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Making model images (model_image.h) from decoded bitmaps and from image
// files. The sandboxed inference service gets raw RGB pixels only; decoding
// happens here, in the browser's own sandboxed decoders: Chromium's data
// decoder process for image files, Blink (the renderer) for a web page's
// images, the compositor for screenshots.

#ifndef CHROME_BROWSER_MOLT_AI_RUNTIME_MODEL_IMAGE_PREP_H_
#define CHROME_BROWSER_MOLT_AI_RUNTIME_MODEL_IMAGE_PREP_H_

#include <cstdint>
#include <optional>

#include "base/containers/span.h"
#include "base/functional/callback.h"
#include "chrome/browser/molt_ai/runtime/model_image.h"

class SkBitmap;

namespace molt_ai {

// A model image from |bitmap| (any size, colour type or alpha type): scaled
// down with Skia so its long side is at most |max_side| (which must be
// within engine/image_input.h's [kMinImageSide, kMaxImageSide]), converted
// to RGB with transparent pixels over a neutral grey, and padded to the
// inference service's bounds (PlanModelImage(), ComposeModelImage()).
// nullopt for an empty or unreadable bitmap. Work grows with the bitmap:
// run it on a ThreadPool worker, never on the UI thread.
std::optional<ModelImage> PrepareImageForModel(const SkBitmap& bitmap,
                                               uint32_t max_side);

using ModelImageCallback =
    base::OnceCallback<void(std::optional<ModelImage> image)>;

// Decodes |encoded| (the bytes of an image file: PNG, JPEG, WebP, GIF's
// first frame, ...) in Chromium's sandboxed data decoder process, which
// shrinks very large images while decoding, then prepares it as above on a
// worker. |callback| runs on the calling sequence, with nullopt if the bytes
// aren't an image the decoder takes. For MoltBrowser's own features that
// hold image files; a web page's images reach the browser decoded by Blink.
void DecodeImageForModel(base::span<const uint8_t> encoded,
                         uint32_t max_side,
                         ModelImageCallback callback);

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_RUNTIME_MODEL_IMAGE_PREP_H_
