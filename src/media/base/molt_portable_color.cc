// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "media/base/molt_portable_color.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace media::molt {

namespace {

// Larger than any video plane (Chromium's limit is 16384 per axis).
constexpr int kMaxSize = 1 << 16;

struct TableEntry {
  YuvEncoding from;
  YuvToPortableCameraCoefficients coefficients;
};

// From each encoding to BT.601 at limited range, Q14, rounded half away from
// zero from the exact values (H.273's Kr and Kb as decimals: BT.601 0.299 and
// 0.114, BT.709 0.2126 and 0.0722, FCC 0.30 and 0.11, SMPTE 240M 0.212 and
// 0.087, BT.2020 0.2627 and 0.0593; limited range 219 and 224 steps, full
// range 255). The unit test recomputes them.
constexpr TableEntry kTable[] = {
    {{YuvMatrix::kBt601, YuvRange::kLimited},
     {16, 16384, 0, 0, 16384, 0, 0, 16384}},
    {{YuvMatrix::kBt601, YuvRange::kFull}, {0, 14071, 0, 0, 14392, 0, 0, 14392}},
    {{YuvMatrix::kBt709, YuvRange::kLimited},
     {16, 16384, 1627, 3141, 16218, -1813, -1187, 16112}},
    {{YuvMatrix::kBt709, YuvRange::kFull},
     {0, 14071, 1429, 2759, 14246, -1593, -1043, 14153}},
    {{YuvMatrix::kFcc, YuvRange::kLimited},
     {16, 16384, 130, 12, 16383, -7, -95, 16352}},
    {{YuvMatrix::kFcc, YuvRange::kFull},
     {0, 14071, 114, 10, 14391, -6, -83, 14364}},
    {{YuvMatrix::kSmpte240m, YuvRange::kLimited},
     {16, 16384, 1204, 3067, 16189, -1770, -878, 16180}},
    {{YuvMatrix::kSmpte240m, YuvRange::kFull},
     {0, 14071, 1057, 2694, 14221, -1555, -771, 14213}},
    {{YuvMatrix::kBt2020, YuvRange::kLimited},
     {16, 16384, 1888, 1690, 16306, -976, -1378, 15999}},
    {{YuvMatrix::kBt2020, YuvRange::kFull},
     {0, 14071, 1659, 1485, 14323, -857, -1210, 14054}},
};

// Rows of `row_size` samples, `stride` apart, fit in `available` samples.
bool RowsFit(size_t available, int rows, size_t stride, int row_size) {
  if (rows <= 0 || rows > kMaxSize || row_size <= 0 || row_size > kMaxSize) {
    return false;
  }
  const size_t row = static_cast<size_t>(row_size);
  if (stride < row || available < row) {
    return false;
  }
  // (rows - 1) * stride + row <= available, without overflowing.
  const size_t full_rows = static_cast<size_t>(rows) - 1;
  return full_rows == 0 || stride <= (available - row) / full_rows;
}

// `value` / 2^14, rounded half up (an arithmetic shift: C++20 defines it for
// negative values), clamped to a sample.
uint8_t Sample(int32_t base, int32_t value) {
  const int32_t rounded =
      base + ((value + (YuvToPortableCameraCoefficients::kOne >> 1)) >> 14);
  return static_cast<uint8_t>(std::clamp(rounded, int32_t{0}, int32_t{255}));
}

}  // namespace

YuvToPortableCameraCoefficients PortableCameraCoefficients(YuvEncoding from) {
  for (const TableEntry& entry : kTable) {
    if (entry.from == from) {
      return entry.coefficients;
    }
  }
  // Every encoding is in the table; the first entry is the identity.
  return kTable[0].coefficients;
}

bool ConvertI420ToPortableCameraEncoding(std::span<uint8_t> y,
                                         size_t y_stride,
                                         std::span<uint8_t> u,
                                         size_t u_stride,
                                         std::span<uint8_t> v,
                                         size_t v_stride,
                                         int width,
                                         int height,
                                         YuvEncoding from) {
  if (width <= 0 || height <= 0 || width > kMaxSize || height > kMaxSize) {
    return false;
  }
  const int chroma_width = width / 2 + width % 2;
  const int chroma_height = height / 2 + height % 2;
  if (!RowsFit(y.size(), height, y_stride, width) ||
      !RowsFit(u.size(), chroma_height, u_stride, chroma_width) ||
      !RowsFit(v.size(), chroma_height, v_stride, chroma_width)) {
    return false;
  }
  if (from == kPortableCameraEncoding) {
    return true;
  }
  const YuvToPortableCameraCoefficients c = PortableCameraCoefficients(from);
  const size_t luma_width = static_cast<size_t>(width);
  const size_t chroma_row = static_cast<size_t>(chroma_width);

  // Luma first: it reads the source's chroma, which the second pass rewrites.
  for (int row = 0; row < height; ++row) {
    const size_t r = static_cast<size_t>(row);
    std::span<uint8_t> luma = y.subspan(r * y_stride, luma_width);
    std::span<const uint8_t> cb = u.subspan((r / 2) * u_stride, chroma_row);
    std::span<const uint8_t> cr = v.subspan((r / 2) * v_stride, chroma_row);
    for (size_t x = 0; x < luma_width; ++x) {
      const int32_t yy = int32_t{luma[x]} - c.y_offset;
      const int32_t uu = int32_t{cb[x / 2]} - 128;
      const int32_t vv = int32_t{cr[x / 2]} - 128;
      luma[x] = Sample(16, c.y_y * yy + c.y_u * uu + c.y_v * vv);
    }
  }
  for (int row = 0; row < chroma_height; ++row) {
    const size_t r = static_cast<size_t>(row);
    std::span<uint8_t> cb = u.subspan(r * u_stride, chroma_row);
    std::span<uint8_t> cr = v.subspan(r * v_stride, chroma_row);
    for (size_t x = 0; x < chroma_row; ++x) {
      const int32_t uu = int32_t{cb[x]} - 128;
      const int32_t vv = int32_t{cr[x]} - 128;
      cb[x] = Sample(128, c.u_u * uu + c.u_v * vv);
      cr[x] = Sample(128, c.v_u * uu + c.v_v * vv);
    }
  }
  return true;
}

}  // namespace media::molt
