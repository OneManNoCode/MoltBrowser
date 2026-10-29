// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: YUV samples of one encoding converted to another's with integer
// arithmetic, the same values on every machine. Pure and std-only but for
// media's export macro (tested standalone and in media_unittests).
//
// A MoltNet camera's frames carry one colour space whatever the camera's
// (third_party/blink/renderer/modules/mediastream/moltnet_camera.h): BT.601
// at limited range, the encoding of libyuv's I420 and the one Chromium
// assumes for YUV of unknown encoding. A camera's frames come in its own: a
// webcam's MJPEG decodes to full-range BT.601 (JPEG), an HD camera on macOS
// delivers BT.709, and so on. ConvertI420() rewrites their samples in place:
// the samples of an encoding (a matrix and a range) stand for R'G'B' values,
// which another encoding stands for with other samples, by an affine map that
// keeps a grey grey. The luma of the result depends on the source's luma and
// chroma, its chroma on the source's chroma only, so a 4:2:0 frame converts
// without resampling its chroma: each luma sample takes the chroma of its 2x2
// block. The coefficients are fixed integers (Q14), computed from the
// matrices' Kr and Kb (ITU-T H.273) and checked against that by the unit
// test, and the arithmetic is integer, so no CPU's rounding enters.
//
// Only the matrix and the range are converted. The primaries and the transfer
// function are taken as they are: those of BT.709 and SMPTE 170M differ
// little, and their transfer functions are the same curve.

#ifndef MEDIA_BASE_MOLT_PORTABLE_COLOR_H_
#define MEDIA_BASE_MOLT_PORTABLE_COLOR_H_

#include <cstddef>
#include <cstdint>
#include <span>

#include "media/base/media_export.h"

namespace media::molt {

// The matrices a camera's YUV may come in (gfx::ColorSpace::MatrixID's).
enum class YuvMatrix : uint8_t {
  kBt601,  // BT.470BG and SMPTE 170M
  kBt709,
  kFcc,
  kSmpte240m,
  kBt2020,  // non-constant luminance
};

enum class YuvRange : uint8_t {
  kLimited,  // luma 16 to 235, chroma 16 to 240
  kFull,     // 0 to 255
};

struct YuvEncoding {
  YuvMatrix matrix = YuvMatrix::kBt601;
  YuvRange range = YuvRange::kLimited;

  bool operator==(const YuvEncoding&) const = default;
};

// BT.601 at limited range: what every MoltNet camera frame is in.
inline constexpr YuvEncoding kPortableCameraEncoding = {YuvMatrix::kBt601,
                                                        YuvRange::kLimited};

// The affine map from 8-bit samples of one encoding to BT.601 at limited
// range, Q14:
//   Y' = 16 + (y_y * (Y - y_offset) + y_u * (U - 128) + y_v * (V - 128)) / 2^14
//   U' = 128 + (u_u * (U - 128) + u_v * (V - 128)) / 2^14
//   V' = 128 + (v_u * (U - 128) + v_v * (V - 128)) / 2^14
// each rounded half up and clamped to 0..255.
struct YuvToPortableCameraCoefficients {
  static constexpr int32_t kOne = 1 << 14;

  int32_t y_offset;  // the source's black: 16 at limited range, 0 at full
  int32_t y_y;
  int32_t y_u;
  int32_t y_v;
  int32_t u_u;
  int32_t u_v;
  int32_t v_u;
  int32_t v_v;
};

// The coefficients for samples in `from`. Identity for
// kPortableCameraEncoding.
MEDIA_EXPORT YuvToPortableCameraCoefficients
PortableCameraCoefficients(YuvEncoding from);

// Converts 8-bit 4:2:0 planes of `width` x `height` pixels, in `from`, to
// kPortableCameraEncoding in place. `y` holds `height` rows of `width`
// samples, `y_stride` samples apart; `u` and `v` hold (height + 1) / 2 rows
// of (width + 1) / 2 samples, `u_stride` and `v_stride` apart. Does nothing
// and returns true when `from` already is kPortableCameraEncoding. Returns
// false, writing nothing, when a size isn't positive, a stride is shorter
// than its row, or a span is too short for its rows.
MEDIA_EXPORT bool ConvertI420ToPortableCameraEncoding(std::span<uint8_t> y,
                                                       size_t y_stride,
                                                       std::span<uint8_t> u,
                                                       size_t u_stride,
                                                       std::span<uint8_t> v,
                                                       size_t v_stride,
                                                       int width,
                                                       int height,
                                                       YuvEncoding from);

}  // namespace media::molt

#endif  // MEDIA_BASE_MOLT_PORTABLE_COLOR_H_
