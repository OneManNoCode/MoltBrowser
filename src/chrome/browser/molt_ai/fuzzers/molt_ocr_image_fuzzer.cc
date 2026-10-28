// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// What tesseract reads (ocr/ocr_image.h): which attachments are decoded
// (the first bytes of any file a page or the user attaches), and the PNM
// the browser writes from decoded pixels (whatever an image holds).
//
// Checks: SniffOcrImageFormat() reads only its input and agrees with an
// independent check of the PNG, JPEG, WebP and TIFF signatures; only PNG,
// JPEG and WebP are decoded. OcrPnmFromRgba() refuses a buffer that isn't
// width x height x 4 bytes or a side past kMaxOcrImageSide; otherwise its
// output is exactly "P5" or "P6", "\n<width> <height>\n255\n" and then one
// or three bytes a pixel: each the pixel composited over white (computed
// here independently), P5 exactly when every pixel comes out gray. Writing
// the same image row by row (OcrPnmHeader() + AppendOcrPnmRow()) gives the
// same bytes.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <fuzzer/FuzzedDataProvider.h>

#include "base/check.h"
#include "chrome/browser/molt_ai/ocr/ocr_image.h"

namespace {

using molt_ai::ocr::OcrImageFormat;

bool StartsWith(std::string_view bytes, std::string_view prefix) {
  return bytes.size() >= prefix.size() &&
         bytes.substr(0, prefix.size()) == prefix;
}

OcrImageFormat ReferenceFormat(std::string_view b) {
  if (StartsWith(b, std::string_view("\x89\x50\x4e\x47\x0d\x0a\x1a\x0a", 8))) {
    return OcrImageFormat::kPng;
  }
  if (b.size() >= 3 && static_cast<uint8_t>(b[0]) == 0xff &&
      static_cast<uint8_t>(b[1]) == 0xd8 &&
      static_cast<uint8_t>(b[2]) == 0xff) {
    return OcrImageFormat::kJpeg;
  }
  if (b.size() >= 12 && b.substr(0, 4) == "RIFF" && b.substr(8, 4) == "WEBP") {
    return OcrImageFormat::kWebp;
  }
  if (b.size() >= 4 &&
      ((b.substr(0, 2) == "II" && (b[2] == '*' || b[2] == '+') && b[3] == 0) ||
       (b.substr(0, 2) == "MM" && b[2] == 0 && (b[3] == '*' || b[3] == '+')))) {
    return OcrImageFormat::kTiff;
  }
  return OcrImageFormat::kOther;
}

uint8_t Composite(uint8_t channel, uint8_t alpha) {
  // Over white, rounded to the nearest level.
  const unsigned exact = channel * alpha + 255u * (255u - alpha);
  return static_cast<uint8_t>((exact + 127u) / 255u);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  FuzzedDataProvider p(data, size);
  using namespace molt_ai::ocr;

  if (p.ConsumeBool()) {
    // A file's first bytes.
    const std::string bytes = p.ConsumeRemainingBytesAsString();
    const OcrImageFormat format = SniffOcrImageFormat(bytes);
    CHECK(format == SniffOcrImageFormat(bytes));
    CHECK(format == ReferenceFormat(bytes));
    const bool decoded = format == OcrImageFormat::kPng ||
                         format == OcrImageFormat::kJpeg ||
                         format == OcrImageFormat::kWebp;
    CHECK(DescribeUnreadableImage(format).empty() == decoded);
    return 0;
  }

  // Decoded pixels: a small image, sometimes a wrong size or a side too
  // large for tesseract.
  uint32_t width = p.ConsumeIntegralInRange<uint32_t>(0, 24);
  uint32_t height = p.ConsumeIntegralInRange<uint32_t>(0, 24);
  if (p.ConsumeIntegralInRange<int>(0, 15) == 0) {
    width = p.ConsumeBool() ? kMaxOcrImageSide + 1 : 1u << 31;
    height = 1;
  }
  const bool gray_pixels = p.ConsumeBool();
  std::string rgba;
  const size_t wanted = static_cast<size_t>(width) <= 24
                            ? static_cast<size_t>(width) * height * 4
                            : 0;
  while (rgba.size() < wanted && p.remaining_bytes() > 0) {
    const uint8_t value = p.ConsumeIntegral<uint8_t>();
    const uint8_t alpha = p.ConsumeBool() ? 255 : p.ConsumeIntegral<uint8_t>();
    const uint8_t green = gray_pixels ? value : p.ConsumeIntegral<uint8_t>();
    rgba.push_back(static_cast<char>(value));
    rgba.push_back(static_cast<char>(green));
    rgba.push_back(static_cast<char>(value));
    rgba.push_back(static_cast<char>(alpha));
  }
  if (p.ConsumeBool()) {
    rgba += p.ConsumeRandomLengthString(8);
  }

  const std::string pnm = OcrPnmFromRgba(width, height, rgba);
  const bool fits = width >= 1 && height >= 1 && width <= kMaxOcrImageSide &&
                    height <= kMaxOcrImageSide &&
                    rgba.size() == static_cast<size_t>(width) * height * 4;
  CHECK(pnm.empty() == !fits);
  if (!fits) {
    return 0;
  }

  // The composited pixels, and whether they are all gray, independently.
  std::string rgb;
  bool all_gray = true;
  for (size_t at = 0; at < rgba.size(); at += 4) {
    const uint8_t alpha = static_cast<uint8_t>(rgba[at + 3]);
    const uint8_t r = Composite(static_cast<uint8_t>(rgba[at]), alpha);
    const uint8_t g = Composite(static_cast<uint8_t>(rgba[at + 1]), alpha);
    const uint8_t b = Composite(static_cast<uint8_t>(rgba[at + 2]), alpha);
    all_gray = all_gray && r == g && g == b;
    rgb.push_back(static_cast<char>(r));
    rgb.push_back(static_cast<char>(g));
    rgb.push_back(static_cast<char>(b));
  }
  const std::string header = std::string(all_gray ? "P5" : "P6") + "\n" +
                             std::to_string(width) + " " +
                             std::to_string(height) + "\n255\n";
  CHECK(std::string_view(pnm).substr(0, header.size()) == header);
  const std::string_view samples = std::string_view(pnm).substr(header.size());
  if (all_gray) {
    CHECK(samples.size() == static_cast<size_t>(width) * height);
    for (size_t i = 0; i < samples.size(); ++i) {
      CHECK(samples[i] == rgb[i * 3]);
    }
  } else {
    CHECK(samples == rgb);
  }

  // Row by row, as the browser writes a decoded bitmap.
  bool rows_gray = true;
  const size_t row_bytes = static_cast<size_t>(width) * 4;
  for (size_t y = 0; y < height; ++y) {
    rows_gray = rows_gray && IsGrayRow(std::string_view(rgba).substr(
                                 y * row_bytes, row_bytes));
  }
  CHECK(rows_gray == all_gray);
  std::string by_rows = OcrPnmHeader(width, height, rows_gray);
  for (size_t y = 0; y < height; ++y) {
    AppendOcrPnmRow(std::string_view(rgba).substr(y * row_bytes, row_bytes),
                    rows_gray, &by_rows);
  }
  CHECK(by_rows == pnm);
  return 0;
}
