// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// What tesseract reads (ocr_service.h). tesseract decodes its input with
// leptonica, and through it libpng, libjpeg, libtiff, giflib, libwebp and
// OpenJPEG, whatever the bytes say. MoltBrowser never hands it a file in
// any of those formats. An attached image, or a page of a scanned PDF
// (which the sandboxed document parser renders to PNG), is decoded by
// Chromium's data decoder, a sandboxed utility process with its own size
// limits, and written here as a binary PNM: the format leptonica reads with
// the least code (a two-byte signature and three decimal numbers, then raw
// samples; pnmio.c, the same from leptonica 1.74 to 1.85). tesseract's
// file-format detection, and every image decoder behind it, sees nothing
// else.
//
//   - SniffOcrImageFormat(): which attachments are decoded at all. PNG,
//     JPEG and WebP, the image types the chat takes; a TIFF, which Chromium
//     can't decode, or anything else is refused with a reason
//     (DescribeUnreadableImage()).
//   - The PNM: P5 (8-bit gray) when every pixel is gray, else P6 (8-bit
//     RGB), the pixel types leptonica gives tesseract for a gray or a
//     colour PNG or JPEG. Transparent pixels are composited over white, as
//     tesseract does with a PNG's alpha channel. At most kMaxOcrImageSide
//     pixels on a side, which is all tesseract takes.
//
// Std-only, so the unit tests, molt_ocr_image_fuzzer and a standalone
// harness that runs the system's tesseract on its output use this code.

#ifndef CHROME_BROWSER_MOLT_AI_OCR_OCR_IMAGE_H_
#define CHROME_BROWSER_MOLT_AI_OCR_OCR_IMAGE_H_

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <string_view>

namespace molt_ai {
namespace ocr {

enum class OcrImageFormat { kPng, kJpeg, kWebp, kTiff, kOther };

// The format |bytes| start with.
OcrImageFormat SniffOcrImageFormat(std::string_view bytes);

// Empty for the formats that are decoded for OCR (PNG, JPEG, WebP);
// otherwise why the image isn't read, for the chat.
std::string_view DescribeUnreadableImage(OcrImageFormat format);

// tesseract's thresholder refuses an image larger than INT16_MAX on a side.
inline constexpr uint32_t kMaxOcrImageSide = 32767;
inline constexpr std::string_view kTooLargeForOcr =
    "the image is too large for OCR (more than 32767 pixels on a side)";

// Whether every pixel of |rgba_row| (4 bytes a pixel, red, green, blue and
// straight alpha) is gray once composited over white.
bool IsGrayRow(std::string_view rgba_row);

// The header of a binary PNM of |width| x |height| 8-bit pixels: "P5" for
// gray, "P6" for RGB, then "\n<width> <height>\n255\n". Empty unless both
// sides are from 1 to kMaxOcrImageSide.
std::string OcrPnmHeader(uint32_t width, uint32_t height, bool gray);

// Appends |rgba_row|'s pixels (4 bytes each; a trailing partial pixel is
// ignored) composited over white: one byte each when |gray|, else three.
void AppendOcrPnmRow(std::string_view rgba_row, bool gray, std::string* pnm);

// The whole file for |rgba|: |width| x |height| pixels, rows one after
// another, 4 bytes a pixel. Gray when every pixel is. Empty if |rgba| isn't
// that size or a side is out of range.
std::string OcrPnmFromRgba(uint32_t width,
                           uint32_t height,
                           std::string_view rgba);

}  // namespace ocr
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_OCR_OCR_IMAGE_H_
