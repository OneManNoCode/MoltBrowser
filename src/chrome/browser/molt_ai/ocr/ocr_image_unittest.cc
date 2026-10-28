// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/ocr/ocr_image.h"

#include <stdint.h>

#include <string>
#include <string_view>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace ocr {
namespace {

std::string Rgba(std::initializer_list<uint8_t> bytes) {
  return std::string(bytes.begin(), bytes.end());
}

TEST(OcrImageTest, SniffsWhatTheChatTakes) {
  EXPECT_EQ(
      SniffOcrImageFormat(std::string("\x89PNG\r\n\x1a\n\0\0\0\rIHDR", 16)),
      OcrImageFormat::kPng);
  EXPECT_EQ(SniffOcrImageFormat("\xff\xd8\xff\xe0\x00\x10JFIF"),
            OcrImageFormat::kJpeg);
  EXPECT_EQ(SniffOcrImageFormat(std::string("RIFF\x24\0\0\0WEBPVP8 ", 16)),
            OcrImageFormat::kWebp);
  EXPECT_EQ(SniffOcrImageFormat(std::string("II*\0\x08\0\0\0", 8)),
            OcrImageFormat::kTiff);
  EXPECT_EQ(SniffOcrImageFormat(std::string("MM\0*\0\0\0\x08", 8)),
            OcrImageFormat::kTiff);
  EXPECT_EQ(SniffOcrImageFormat(std::string("II+\0\x08\0\0\0", 8)),
            OcrImageFormat::kTiff);
  for (std::string_view other :
       {std::string_view(""), std::string_view("\x89PNG"),
        std::string_view("GIF89a"), std::string_view("BM6\0\0\0", 6),
        std::string_view("P6\n1 1\n255\n"),
        std::string_view("RIFF\0\0\0\0WAVE", 12),
        std::string_view("RIFF\0\0\0\0WEB", 11), std::string_view("%PDF-1.7"),
        std::string_view("\xff\xd8"),
        std::string_view("\0\0\0\x0cjP  \r\n\x87\n", 12)}) {
    EXPECT_EQ(SniffOcrImageFormat(other), OcrImageFormat::kOther) << other;
  }

  for (OcrImageFormat ok :
       {OcrImageFormat::kPng, OcrImageFormat::kJpeg, OcrImageFormat::kWebp}) {
    EXPECT_TRUE(DescribeUnreadableImage(ok).empty());
  }
  EXPECT_NE(DescribeUnreadableImage(OcrImageFormat::kTiff).find("TIFF"),
            std::string_view::npos);
  EXPECT_NE(DescribeUnreadableImage(OcrImageFormat::kTiff).find("PNG or JPEG"),
            std::string_view::npos);
  EXPECT_EQ(DescribeUnreadableImage(OcrImageFormat::kOther),
            "this file isn't a PNG, JPEG or WebP image");
}

TEST(OcrImageTest, Headers) {
  EXPECT_EQ(OcrPnmHeader(640, 480, true), "P5\n640 480\n255\n");
  EXPECT_EQ(OcrPnmHeader(1, 32767, false), "P6\n1 32767\n255\n");
  EXPECT_EQ(OcrPnmHeader(0, 1, true), "");
  EXPECT_EQ(OcrPnmHeader(1, 0, true), "");
  EXPECT_EQ(OcrPnmHeader(32768, 1, false), "");
  EXPECT_EQ(OcrPnmHeader(1, 32768, false), "");
}

TEST(OcrImageTest, GrayImagesAreP5) {
  // Black, white and a gray, opaque, in one row; one pixel a row below.
  const std::string pnm = OcrPnmFromRgba(
      3, 2, Rgba({0, 0, 0, 255, 255, 255, 255, 255, 128, 128, 128, 255,
                  9, 9, 9, 255, 10,  10,  10,  255, 11,  11,  11,  255}));
  EXPECT_EQ(pnm, std::string("P5\n3 2\n255\n\x00\xff\x80\x09\x0a\x0b", 17));
}

TEST(OcrImageTest, ColourImagesAreP6) {
  const std::string pnm =
      OcrPnmFromRgba(2, 1, Rgba({255, 0, 0, 255, 1, 2, 3, 255}));
  EXPECT_EQ(pnm, std::string("P6\n2 1\n255\n\xff\x00\x00\x01\x02\x03", 17));
  // One coloured pixel anywhere makes the whole image RGB.
  const std::string late =
      OcrPnmFromRgba(1, 3, Rgba({7, 7, 7, 255, 7, 7, 7, 255, 7, 7, 8, 255}));
  EXPECT_EQ(
      late,
      std::string("P6\n1 3\n255\n\x07\x07\x07\x07\x07\x07\x07\x07\x08", 20));
}

TEST(OcrImageTest, TransparencyIsOverWhite) {
  // Fully transparent: white whatever its colour; half: halfway to white.
  EXPECT_EQ(OcrPnmFromRgba(1, 1, Rgba({0, 0, 0, 0})),
            std::string("P5\n1 1\n255\n\xff", 12));
  EXPECT_EQ(OcrPnmFromRgba(1, 1, Rgba({12, 200, 99, 0})),
            std::string("P5\n1 1\n255\n\xff", 12));
  EXPECT_EQ(OcrPnmFromRgba(1, 1, Rgba({0, 0, 0, 128})),
            std::string("P5\n1 1\n255\n\x7f", 12));
  // Red at half opacity: (255, 128, 128) over white is still colour.
  EXPECT_EQ(OcrPnmFromRgba(1, 1, Rgba({255, 0, 0, 128})),
            std::string("P6\n1 1\n255\n\xff\x7f\x7f", 14));
  EXPECT_TRUE(IsGrayRow(Rgba({255, 0, 0, 0, 3, 3, 3, 255})));
  EXPECT_FALSE(IsGrayRow(Rgba({255, 0, 0, 1})));
  EXPECT_TRUE(IsGrayRow(""));
}

TEST(OcrImageTest, RowsAppendAsTheyCome) {
  std::string pnm = OcrPnmHeader(2, 1, false);
  AppendOcrPnmRow(Rgba({1, 2, 3, 255, 4, 5, 6, 255, 9}), false, &pnm);
  EXPECT_EQ(pnm, std::string("P6\n2 1\n255\n\x01\x02\x03\x04\x05\x06", 17));
  pnm = OcrPnmHeader(2, 1, true);
  AppendOcrPnmRow(Rgba({1, 1, 1, 255, 4, 4, 4, 255}), true, &pnm);
  EXPECT_EQ(pnm, std::string("P5\n2 1\n255\n\x01\x04", 13));
}

TEST(OcrImageTest, RefusesWrongSizes) {
  EXPECT_EQ(OcrPnmFromRgba(2, 2, std::string(15, '\0')), "");
  EXPECT_EQ(OcrPnmFromRgba(2, 2, std::string(17, '\0')), "");
  EXPECT_EQ(OcrPnmFromRgba(0, 0, ""), "");
  EXPECT_EQ(OcrPnmFromRgba(32768, 1, std::string(32768 * 4, '\0')), "");
  EXPECT_EQ(OcrPnmFromRgba(32767, 1, std::string(32767 * 4, '\0')).size(),
            std::string("P5\n32767 1\n255\n").size() + 32767);
}

}  // namespace
}  // namespace ocr
}  // namespace molt_ai
