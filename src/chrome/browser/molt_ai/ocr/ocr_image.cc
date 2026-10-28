// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/ocr/ocr_image.h"

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <string_view>

namespace molt_ai {
namespace ocr {

namespace {

constexpr size_t kRgbaBytes = 4;

// A channel of a pixel with straight alpha |alpha|, composited over white
// (rounded to the nearest level).
uint8_t OverWhite(char channel, char alpha) {
  const uint32_t c = static_cast<uint8_t>(channel);
  const uint32_t a = static_cast<uint8_t>(alpha);
  return static_cast<uint8_t>((c * a + 255 * (255 - a) + 127) / 255);
}

}  // namespace

OcrImageFormat SniffOcrImageFormat(std::string_view bytes) {
  if (bytes.starts_with(std::string_view("\x89PNG\r\n\x1a\n", 8))) {
    return OcrImageFormat::kPng;
  }
  if (bytes.starts_with("\xff\xd8\xff")) {
    return OcrImageFormat::kJpeg;
  }
  if (bytes.size() >= 12 && bytes.starts_with("RIFF") &&
      bytes.substr(8, 4) == "WEBP") {
    return OcrImageFormat::kWebp;
  }
  // Classic and BigTIFF, either byte order.
  for (std::string_view tiff :
       {std::string_view("II*\0", 4), std::string_view("MM\0*", 4),
        std::string_view("II+\0", 4), std::string_view("MM\0+", 4)}) {
    if (bytes.starts_with(tiff)) {
      return OcrImageFormat::kTiff;
    }
  }
  return OcrImageFormat::kOther;
}

std::string_view DescribeUnreadableImage(OcrImageFormat format) {
  switch (format) {
    case OcrImageFormat::kPng:
    case OcrImageFormat::kJpeg:
    case OcrImageFormat::kWebp:
      return std::string_view();
    case OcrImageFormat::kTiff:
      return "TIFF images can't be read for OCR: save the image as PNG or "
             "JPEG and attach that";
    case OcrImageFormat::kOther:
      return "this file isn't a PNG, JPEG or WebP image";
  }
  return "this image can't be read for OCR";
}

bool IsGrayRow(std::string_view rgba_row) {
  for (size_t at = 0; at + kRgbaBytes <= rgba_row.size(); at += kRgbaBytes) {
    const char alpha = rgba_row[at + 3];
    const uint8_t red = OverWhite(rgba_row[at], alpha);
    if (OverWhite(rgba_row[at + 1], alpha) != red ||
        OverWhite(rgba_row[at + 2], alpha) != red) {
      return false;
    }
  }
  return true;
}

std::string OcrPnmHeader(uint32_t width, uint32_t height, bool gray) {
  if (width == 0 || height == 0 || width > kMaxOcrImageSide ||
      height > kMaxOcrImageSide) {
    return std::string();
  }
  std::string header = gray ? "P5\n" : "P6\n";
  header += std::to_string(width);
  header += ' ';
  header += std::to_string(height);
  header += "\n255\n";
  return header;
}

void AppendOcrPnmRow(std::string_view rgba_row, bool gray, std::string* pnm) {
  for (size_t at = 0; at + kRgbaBytes <= rgba_row.size(); at += kRgbaBytes) {
    const char alpha = rgba_row[at + 3];
    pnm->push_back(static_cast<char>(OverWhite(rgba_row[at], alpha)));
    if (!gray) {
      pnm->push_back(static_cast<char>(OverWhite(rgba_row[at + 1], alpha)));
      pnm->push_back(static_cast<char>(OverWhite(rgba_row[at + 2], alpha)));
    }
  }
}

std::string OcrPnmFromRgba(uint32_t width,
                           uint32_t height,
                           std::string_view rgba) {
  // The sides first: then the sizes below can't overflow.
  if (OcrPnmHeader(width, height, true).empty()) {
    return std::string();
  }
  const size_t row_bytes = size_t{width} * kRgbaBytes;
  if (rgba.size() / row_bytes != height || rgba.size() % row_bytes != 0) {
    return std::string();
  }
  bool gray = true;
  for (size_t y = 0; y < height && gray; ++y) {
    gray = IsGrayRow(rgba.substr(y * row_bytes, row_bytes));
  }
  std::string pnm = OcrPnmHeader(width, height, gray);
  for (size_t y = 0; y < height; ++y) {
    AppendOcrPnmRow(rgba.substr(y * row_bytes, row_bytes), gray, &pnm);
  }
  return pnm;
}

}  // namespace ocr
}  // namespace molt_ai
