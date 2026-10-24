// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "media/base/molt_portable_scale.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <span>
#include <vector>

namespace media::molt {

namespace {

// Larger than any video plane (Chromium's limit is 16384 per axis), and
// small enough that no product below overflows 64 bits.
constexpr int kMaxSize = 1 << 16;

// Rows of `row_size` samples, `stride` apart, fit in `available` samples.
bool RowsFit(size_t available, int rows, size_t stride, size_t row_size) {
  if (rows <= 0 || stride < row_size || available < row_size) {
    return false;
  }
  // (rows - 1) * stride + row_size <= available, without overflowing.
  const size_t full_rows = static_cast<size_t>(rows) - 1;
  return full_rows == 0 || stride <= (available - row_size) / full_rows;
}

template <typename T>
bool ScalePlane(std::span<const T> src,
                size_t src_stride,
                int src_width,
                int src_height,
                std::span<T> dst,
                size_t dst_stride,
                int dst_width,
                int dst_height,
                int channels) {
  if (channels < 1 || channels > 4 || src_width <= 0 || src_height <= 0 ||
      dst_width <= 0 || dst_height <= 0 || src_width > kMaxSize ||
      src_height > kMaxSize || dst_width > kMaxSize || dst_height > kMaxSize) {
    return false;
  }
  const size_t ch = static_cast<size_t>(channels);
  const size_t src_row = static_cast<size_t>(src_width) * ch;
  const size_t dst_row = static_cast<size_t>(dst_width) * ch;
  if (!RowsFit(src.size(), src_height, src_stride, src_row) ||
      !RowsFit(dst.size(), dst_height, dst_stride, dst_row)) {
    return false;
  }

  const PortableScaleAxis hx = PortableScaleAxis::Make(src_width, dst_width);
  const PortableScaleAxis vy = PortableScaleAxis::Make(src_height, dst_height);
  const std::span<const int32_t> hweights(hx.weights);
  const std::span<const int32_t> vweights(vy.weights);

  // Source row `r` scaled horizontally, Q14 (at most 65535 << 14: fits).
  auto horizontal = [&](int r, std::vector<int32_t>& out) {
    out.assign(dst_row, 0);
    const std::span<const T> row =
        src.subspan(static_cast<size_t>(r) * src_stride, src_row);
    for (size_t x = 0; x < static_cast<size_t>(dst_width); ++x) {
      const size_t first = static_cast<size_t>(hx.first[x]);
      const std::span<const int32_t> taps =
          hweights.subspan(hx.offset[x], hx.count[x]);
      for (size_t c = 0; c < ch; ++c) {
        int32_t sum = 0;
        for (size_t k = 0; k < taps.size(); ++k) {
          sum += taps[k] * static_cast<int32_t>(row[(first + k) * ch + c]);
        }
        out[x * ch + c] = sum;
      }
    }
  };

  // The source rows the current output row reads, scaled horizontally. The
  // window only moves forward: both ends of every axis' taps are
  // non-decreasing.
  std::deque<std::vector<int32_t>> window;
  int window_first = 0;
  std::vector<int64_t> acc(dst_row);
  constexpr int64_t kMax = std::numeric_limits<T>::max();
  for (size_t y = 0; y < static_cast<size_t>(dst_height); ++y) {
    const int first = vy.first[y];
    const int count = static_cast<int>(vy.count[y]);
    if (first < window_first) {
      window.clear();
    }
    while (!window.empty() && window_first < first) {
      window.pop_front();
      ++window_first;
    }
    if (window.empty()) {
      window_first = first;
    }
    while (window_first + static_cast<int>(window.size()) < first + count) {
      window.emplace_back();
      horizontal(window_first + static_cast<int>(window.size()) - 1,
                 window.back());
    }

    std::fill(acc.begin(), acc.end(), 0);
    const std::span<const int32_t> taps =
        vweights.subspan(vy.offset[y], vy.count[y]);
    for (size_t k = 0; k < taps.size(); ++k) {
      const std::vector<int32_t>& h =
          window[static_cast<size_t>(first - window_first) + k];
      const int64_t w = taps[k];
      for (size_t i = 0; i < dst_row; ++i) {
        acc[i] += w * h[i];
      }
    }
    const std::span<T> out = dst.subspan(y * dst_stride, dst_row);
    for (size_t i = 0; i < dst_row; ++i) {
      const int64_t v = (acc[i] + (int64_t{1} << 27)) >> 28;
      out[i] = static_cast<T>(std::clamp<int64_t>(v, 0, kMax));
    }
  }
  return true;
}

}  // namespace

// static
PortableScaleAxis PortableScaleAxis::Make(int src_size, int dst_size) {
  PortableScaleAxis axis;
  if (src_size <= 0 || dst_size <= 0 || src_size > kMaxSize ||
      dst_size > kMaxSize) {
    return axis;
  }
  const int64_t s = src_size;
  const int64_t d = dst_size;
  const size_t n = static_cast<size_t>(dst_size);
  axis.first.resize(n);
  axis.offset.resize(n);
  axis.count.resize(n);
  for (int64_t o = 0; o < d; ++o) {
    const size_t index = static_cast<size_t>(o);
    axis.offset[index] = static_cast<uint32_t>(axis.weights.size());
    if (s == d) {
      axis.first[index] = static_cast<int32_t>(o);
      axis.weights.push_back(kOne);
    } else if (d < s) {
      // Box: output `o` covers [o * s, (o + 1) * s) in units of 1/d source
      // samples. Each source sample's weight is the rounded cumulative
      // coverage minus the previous one, so the weights sum to exactly kOne.
      const int64_t lo = o * s;
      const int64_t hi = (o + 1) * s;
      const int64_t first = lo / d;
      const int64_t last = (hi - 1) / d;
      int64_t previous = 0;
      for (int64_t i = first; i <= last; ++i) {
        const int64_t covered = std::min((i + 1) * d, hi) - lo;
        const int64_t cumulative = (covered * kOne + s / 2) / s;
        axis.weights.push_back(static_cast<int32_t>(cumulative - previous));
        previous = cumulative;
      }
      axis.first[index] = static_cast<int32_t>(first);
    } else {
      // Tent: output `o`'s centre is at ((2o + 1) * s - d) / (2d) source
      // samples; edges replicate.
      const int64_t num = (2 * o + 1) * s - d;
      const int64_t den = 2 * d;
      int64_t first = 0;
      int64_t next_weight = 0;
      if (num > 0) {
        first = num / den;
        if (first >= s - 1) {
          first = s - 1;
        } else {
          next_weight = ((num % den) * kOne + d) / den;
        }
      }
      if (next_weight == kOne) {
        axis.first[index] = static_cast<int32_t>(first + 1);
        axis.weights.push_back(kOne);
      } else {
        axis.first[index] = static_cast<int32_t>(first);
        axis.weights.push_back(static_cast<int32_t>(kOne - next_weight));
        if (next_weight != 0) {
          axis.weights.push_back(static_cast<int32_t>(next_weight));
        }
      }
    }
    axis.count[index] =
        static_cast<uint32_t>(axis.weights.size()) - axis.offset[index];
  }
  return axis;
}

bool PortableScalePlane(std::span<const uint8_t> src,
                        size_t src_stride,
                        int src_width,
                        int src_height,
                        std::span<uint8_t> dst,
                        size_t dst_stride,
                        int dst_width,
                        int dst_height,
                        int channels) {
  return ScalePlane<uint8_t>(src, src_stride, src_width, src_height, dst,
                             dst_stride, dst_width, dst_height, channels);
}

bool PortableScalePlane16(std::span<const uint16_t> src,
                          size_t src_stride,
                          int src_width,
                          int src_height,
                          std::span<uint16_t> dst,
                          size_t dst_stride,
                          int dst_width,
                          int dst_height,
                          int channels) {
  return ScalePlane<uint16_t>(src, src_stride, src_width, src_height, dst,
                              dst_stride, dst_width, dst_height, channels);
}

}  // namespace media::molt
