// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: a video-plane scaler that computes the same values on every
// machine. Pure and std-only (tested standalone and in media_unittests).
//
// libyuv's filtered scalers (ScalePlane(), ARGBScale(), NV12Scale(), ...
// with kFilterBox or kFilterBilinear) are bit-exact across the SIMD levels of
// one CPU architecture, but not across architectures: libyuv's own C code
// blends with 7-bit fractions on x86 and 16-bit fractions on Arm, to match
// each architecture's vector code, and some exact ratios (3/8) take vector
// paths that round differently from the C ones. MoltNet's renderers
// (molt_portable_media.h) scale with PortableScalePlane() instead, so a page
// that has a video encoder scale its input reads the same bitstream on an
// Intel and an Apple silicon Mac.
//
// The filter is separable: along an axis that shrinks, every output sample
// is the area average of the source samples it covers (a box filter); along
// an axis that grows, a linear interpolation between the two nearest source
// samples, with centres aligned (a tent filter); an axis that keeps its size
// is copied. Weights are exact integers (Q14, summing to exactly 1 << 14 for
// every output sample), the accumulation is integer, and there is no
// overshoot, so no value leaves the input's range.

#ifndef MEDIA_BASE_MOLT_PORTABLE_SCALE_H_
#define MEDIA_BASE_MOLT_PORTABLE_SCALE_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace media::molt {

// One axis of a scale: for every output position, the first source position
// it reads and the Q14 weights of it and the following ones.
struct PortableScaleAxis {
  static constexpr int32_t kOne = 1 << 14;

  std::vector<int32_t> first;    // per output position
  std::vector<uint32_t> offset;  // into `weights`, per output position
  std::vector<uint32_t> count;   // taps, per output position
  std::vector<int32_t> weights;  // all taps, Q14

  // Empty when either size isn't positive.
  static PortableScaleAxis Make(int src_size, int dst_size);
};

// Scales a plane of `channels` (1 to 4) interleaved samples per pixel.
// `src` holds `src_height` rows of `src_width` pixels, `src_stride` samples
// apart (not bytes); likewise `dst`. Returns false, writing nothing, when a
// size isn't positive, a stride is shorter than its row, or a span is too
// short for its rows.
bool PortableScalePlane(std::span<const uint8_t> src,
                        size_t src_stride,
                        int src_width,
                        int src_height,
                        std::span<uint8_t> dst,
                        size_t dst_stride,
                        int dst_width,
                        int dst_height,
                        int channels);

// The same for 16-bit samples (high bit depth planes, whatever their bit
// depth: the result never exceeds the largest input value).
bool PortableScalePlane16(std::span<const uint16_t> src,
                          size_t src_stride,
                          int src_width,
                          int src_height,
                          std::span<uint16_t> dst,
                          size_t dst_stride,
                          int dst_width,
                          int dst_height,
                          int channels);

}  // namespace media::molt

#endif  // MEDIA_BASE_MOLT_PORTABLE_SCALE_H_
