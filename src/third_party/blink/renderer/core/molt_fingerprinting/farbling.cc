// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/farbling.h"

#include <bit>
#include <string>

#include "build/build_config.h"
#include "crypto/hmac.h"

// The F16/F32 paths assemble little-endian values byte by byte; Skia and
// every platform MoltBrowser ships on are little-endian.
#if defined(ARCH_CPU_BIG_ENDIAN)
#error "molt_fingerprinting assumes a little-endian CPU"
#endif

namespace blink::molt_fingerprinting {

namespace {

constexpr char kSiteKeyLabel[] = "molt-fp-site-v1";

// PRF domain separators.
constexpr uint64_t kDomainChannel = 1;
constexpr uint64_t kDomainAudio = 4;
constexpr uint64_t kDomainHardwareConcurrency = 5;

// 2^64 / golden ratio, the usual odd multiplier for spreading small inputs.
constexpr uint64_t kGolden = 0x9e3779b97f4a7c15ull;

// One pair in kCollapseOneIn is collapsed (so ~1 value in 2*kCollapseOneIn
// moves). Must be a power of two.
constexpr uint64_t kCollapseOneIn = 8;

// Marks byte contexts that come from AnalyserNode data rather than pixels
// (pixel contexts fit in 32 bits).
constexpr uint64_t kAudioByteContextTag = uint64_t{1} << 40;

// splitmix64's finalizer: a full-avalanche 64-bit mixer.
constexpr uint64_t Mix(uint64_t z) {
  z ^= z >> 30;
  z *= 0xbf58476d1ce4e5b9ull;
  z ^= z >> 27;
  z *= 0x94d049bb133111ebull;
  z ^= z >> 31;
  return z;
}

uint64_t LoadU64LE(base::span<const uint8_t> bytes) {
  uint64_t value = 0;
  for (size_t i = 0; i < 8; ++i) {
    value |= uint64_t{bytes[i]} << (8 * i);
  }
  return value;
}

uint16_t LoadU16LE(base::span<const uint8_t> bytes) {
  return static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));
}

void StoreU16LE(base::span<uint8_t> bytes, uint16_t value) {
  bytes[0] = static_cast<uint8_t>(value & 0xff);
  bytes[1] = static_cast<uint8_t>(value >> 8);
}

uint32_t LoadU32LE(base::span<const uint8_t> bytes) {
  return uint32_t{bytes[0]} | (uint32_t{bytes[1]} << 8) |
         (uint32_t{bytes[2]} << 16) | (uint32_t{bytes[3]} << 24);
}

void StoreU32LE(base::span<uint8_t> bytes, uint32_t value) {
  for (size_t i = 0; i < 4; ++i) {
    bytes[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xff);
  }
}

// Maps a float channel onto the 8-bit grid used for decisions. Values outside
// [0, 1] (wide-gamut / HDR data) clamp for the decision but are never moved.
uint8_t ToUnorm8(float value, bool* in_range) {
  *in_range = value >= 0.0f && value <= 1.0f;
  if (!(value >= 0.0f)) {  // Also catches NaN.
    value = 0.0f;
  } else if (value > 1.0f) {
    value = 1.0f;
  }
  return static_cast<uint8_t>(value * 255.0f + 0.5f);
}

}  // namespace

SiteKey DeriveSiteKey(base::span<const uint8_t> session_seed,
                      std::string_view site,
                      std::string_view partition) {
  // label 0x00 len32(site) site len32(partition) partition: length-prefixed,
  // so no (site, partition) pair can collide with another.
  auto append_field = [](std::string& out, std::string_view field) {
    const uint32_t size = static_cast<uint32_t>(field.size());
    for (int shift = 24; shift >= 0; shift -= 8) {
      out.push_back(static_cast<char>((size >> shift) & 0xff));
    }
    out.append(field);
  };
  std::string message(kSiteKeyLabel);
  message.push_back('\0');
  append_field(message, site);
  append_field(message, partition);
  return crypto::hmac::SignSha256(session_seed, base::as_byte_span(message));
}

Farbler::Farbler(const SiteKey& key) {
  const base::span<const uint8_t> bytes(key);
  for (size_t i = 0; i < k_.size(); ++i) {
    k_[i] = LoadU64LE(bytes.subspan(8 * i, 8u));
  }
}

uint64_t Farbler::Prf(uint64_t domain, uint64_t input) const {
  return Mix(Mix(input ^ k_[0] ^ (domain * kGolden)) ^ k_[1]) ^ k_[2];
}

uint8_t Farbler::FarbleByte(uint8_t value, uint64_t context) const {
  // This runs once per distinct pixel, so it is kept to one multiply for the
  // pair alignment (which depends only on the context: the other channels +
  // alpha) and one mix round for the pair decision. Pairs are
  // {2k + align, 2k + 1 + align}; with align == 1 the values 0 and 255 are
  // singletons and never move.
  const unsigned align =
      static_cast<unsigned>(((context ^ k_[3]) * (k_[1] | 1)) >> 63);
  if (value < align) {
    return value;
  }
  const unsigned k = (value - align) >> 1;
  const unsigned lo = 2 * k + align;
  const unsigned hi = lo + 1;
  if (hi > 255) {
    return value;
  }
  const uint64_t r = Mix(context ^ k_[0] ^ ((uint64_t{k} + 1) * kGolden) ^
                         (uint64_t{align} << 63));
  if ((r & (kCollapseOneIn - 1)) != 0) {
    return value;
  }
  return static_cast<uint8_t>((r / kCollapseOneIn) & 1 ? hi : lo);
}

size_t Farbler::ChannelFor(uint8_t alpha) const {
  return static_cast<size_t>(Prf(kDomainChannel, alpha) % 3);
}

std::array<uint8_t, 4> Farbler::FarblePixelInChannel(
    const std::array<uint8_t, 4>& rgba,
    size_t channel) const {
  // The farbled channel depends only on alpha, and the pair only on the
  // other channels, so moving the channel never changes either decision:
  // that is what makes the mapping idempotent.
  const uint64_t other1 = rgba[(channel + 1) % 3];
  const uint64_t other2 = rgba[(channel + 2) % 3];
  const uint64_t context = uint64_t{channel} | (other1 << 8) | (other2 << 16) |
                           (uint64_t{rgba[3]} << 24);
  std::array<uint8_t, 4> out = rgba;
  out[channel] = FarbleByte(rgba[channel], context);
  return out;
}

std::array<uint8_t, 4> Farbler::FarblePixel(
    const std::array<uint8_t, 4>& rgba) const {
  if (rgba[3] == 0) {
    return rgba;
  }
  return FarblePixelInChannel(rgba, ChannelFor(rgba[3]));
}

void Farbler::FarbleRGBA8(base::span<uint8_t> pixels,
                          ChannelOrder order) const {
  const size_t r_index = order == ChannelOrder::kBGRA ? 2 : 0;
  const size_t b_index = 2 - r_index;
  const size_t count = pixels.size() / 4;
  // Canvases are full of runs of one colour and mostly of one alpha: skip
  // the PRFs for repeats. (last_in == 0 is transparent, which is never
  // looked up.)
  uint32_t last_in = 0;
  std::array<uint8_t, 4> last_out = {0, 0, 0, 0};
  bool last_changed = false;
  int last_alpha = -1;
  size_t channel = 0;
  for (size_t p = 0; p < 4 * count; p += 4) {
    const uint8_t alpha = pixels[p + 3];
    if (alpha == 0) {
      continue;
    }
    const std::array<uint8_t, 4> rgba = {pixels[p + r_index], pixels[p + 1],
                                         pixels[p + b_index], alpha};
    const uint32_t packed = uint32_t{rgba[0]} | (uint32_t{rgba[1]} << 8) |
                            (uint32_t{rgba[2]} << 16) | (uint32_t{alpha} << 24);
    if (packed != last_in) {
      last_in = packed;
      if (alpha != last_alpha) {
        last_alpha = alpha;
        channel = ChannelFor(alpha);
      }
      last_out = FarblePixelInChannel(rgba, channel);
      last_changed = last_out != rgba;
    }
    if (!last_changed) {
      continue;
    }
    pixels[p + r_index] = last_out[0];
    pixels[p + 1] = last_out[1];
    pixels[p + b_index] = last_out[2];
  }
}

void Farbler::FarbleRGBAF16(base::span<uint8_t> pixels) const {
  const size_t count = pixels.size() / 8;
  for (size_t i = 0; i < count; ++i) {
    base::span<uint8_t> px = pixels.subspan(8 * i, 8u);
    std::array<uint8_t, 4> rgba;
    std::array<bool, 4> in_range;
    for (size_t c = 0; c < 4; ++c) {
      bool ok = false;
      rgba[c] = ToUnorm8(HalfToFloat(LoadU16LE(px.subspan(2 * c, 2u))), &ok);
      in_range[c] = ok;
    }
    if (rgba[3] == 0) {
      continue;
    }
    const std::array<uint8_t, 4> out = FarblePixel(rgba);
    for (size_t c = 0; c < 3; ++c) {
      if (out[c] != rgba[c] && in_range[c]) {
        // Snap to the target 8-bit level so an 8-bit read of the same data,
        // and a second pass, see exactly `out[c]`.
        StoreU16LE(px.subspan(2 * c, 2u), FloatToHalf(out[c] / 255.0f));
      }
    }
  }
}

void Farbler::FarbleRGBAF32(base::span<uint8_t> pixels) const {
  const size_t count = pixels.size() / 16;
  for (size_t i = 0; i < count; ++i) {
    base::span<uint8_t> px = pixels.subspan(16 * i, 16u);
    std::array<uint8_t, 4> rgba;
    std::array<bool, 4> in_range;
    for (size_t c = 0; c < 4; ++c) {
      bool ok = false;
      rgba[c] =
          ToUnorm8(std::bit_cast<float>(LoadU32LE(px.subspan(4 * c, 4u))), &ok);
      in_range[c] = ok;
    }
    if (rgba[3] == 0) {
      continue;
    }
    const std::array<uint8_t, 4> out = FarblePixel(rgba);
    for (size_t c = 0; c < 3; ++c) {
      if (out[c] != rgba[c] && in_range[c]) {
        StoreU32LE(px.subspan(4 * c, 4u),
                   std::bit_cast<uint32_t>(out[c] / 255.0f));
      }
    }
  }
}

float Farbler::FarbleSample(float sample) const {
  const uint32_t bits = std::bit_cast<uint32_t>(sample);
  const uint32_t exponent = (bits >> 23) & 0xff;
  if (exponent == 0 || exponent == 0xff) {
    // Zero, subnormal, infinity, NaN.
    return sample;
  }
  constexpr uint32_t kLowMask = (uint32_t{1} << kAudioNoiseBits) - 1;
  const uint32_t high = bits & ~kLowMask;
  const uint32_t low =
      static_cast<uint32_t>(Prf(kDomainAudio, high)) & kLowMask;
  return std::bit_cast<float>(high | low);
}

void Farbler::FarbleAudio(base::span<float> samples) const {
  float last_in = 0.0f;
  float last_out = 0.0f;
  bool have_last = false;
  for (size_t i = 0; i < samples.size(); ++i) {
    const float sample = samples[i];
    // Compare bit patterns: NaN != NaN, and -0 vs +0 must stay distinct.
    if (!have_last ||
        std::bit_cast<uint32_t>(sample) != std::bit_cast<uint32_t>(last_in)) {
      last_in = sample;
      last_out = FarbleSample(sample);
      have_last = true;
    }
    samples[i] = last_out;
  }
}

void Farbler::FarbleAudioBytes(base::span<uint8_t> bytes,
                               uint8_t neutral) const {
  const uint64_t context = kAudioByteContextTag | neutral;
  for (size_t i = 0; i < bytes.size(); ++i) {
    if (bytes[i] != neutral) {
      bytes[i] = FarbleByte(bytes[i], context);
    }
  }
}

unsigned Farbler::HardwareConcurrency(unsigned real) const {
  if (real <= 2) {
    return real;
  }
  return 2 + static_cast<unsigned>(Prf(kDomainHardwareConcurrency, 0) %
                                   (uint64_t{real} - 1));
}

LetterboxSize LetterboxBucket(int viewport_width, int viewport_height) {
  auto bucket = [](int value, int step, int max) {
    if (value < step) {
      return step;
    }
    value -= value % step;
    return value > max ? max : value;
  };
  return {bucket(viewport_width, kLetterboxStepWidth, kLetterboxMaxWidth),
          bucket(viewport_height, kLetterboxStepHeight, kLetterboxMaxHeight)};
}

float HalfToFloat(uint16_t half) {
  const uint32_t sign = uint32_t{half & 0x8000u} << 16;
  uint32_t exponent = (half >> 10) & 0x1f;
  uint32_t mantissa = half & 0x3ffu;
  uint32_t bits;
  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else {
      // Subnormal half: normalise into a float.
      exponent = 127 - 15 + 1;
      while ((mantissa & 0x400u) == 0) {
        mantissa <<= 1;
        --exponent;
      }
      mantissa &= 0x3ffu;
      bits = sign | (exponent << 23) | (mantissa << 13);
    }
  } else if (exponent == 0x1f) {
    bits = sign | 0x7f800000u | (mantissa << 13);
  } else {
    bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
  }
  return std::bit_cast<float>(bits);
}

uint16_t FloatToHalf(float value) {
  const uint32_t bits = std::bit_cast<uint32_t>(value);
  const uint32_t sign = (bits >> 16) & 0x8000u;
  const uint32_t exponent = (bits >> 23) & 0xff;
  uint32_t mantissa = bits & 0x7fffffu;
  if (exponent == 0xff) {
    // Infinity stays infinity; NaN stays a (quiet) NaN.
    return static_cast<uint16_t>(sign | 0x7c00u | (mantissa ? 0x200u : 0u));
  }
  const int half_exponent = static_cast<int>(exponent) - 127 + 15;
  if (half_exponent >= 0x1f) {
    return static_cast<uint16_t>(sign | 0x7c00u);  // Overflow: infinity.
  }
  if (half_exponent <= 0) {
    if (half_exponent < -10) {
      return static_cast<uint16_t>(sign);  // Rounds to (signed) zero.
    }
    // Subnormal half: shift the full 24-bit significand into place.
    mantissa |= 0x800000u;
    const int shift = 14 - half_exponent;
    uint32_t half_mantissa = mantissa >> shift;
    const uint32_t remainder = mantissa & ((uint32_t{1} << shift) - 1);
    const uint32_t halfway = uint32_t{1} << (shift - 1);
    if (remainder > halfway || (remainder == halfway && (half_mantissa & 1))) {
      ++half_mantissa;
    }
    return static_cast<uint16_t>(sign | half_mantissa);
  }
  uint32_t out =
      sign | (static_cast<uint32_t>(half_exponent) << 10) | (mantissa >> 13);
  const uint32_t remainder = mantissa & 0x1fffu;
  // Round to nearest even; a carry out of the mantissa correctly bumps the
  // exponent (up to infinity).
  if (remainder > 0x1000u || (remainder == 0x1000u && (out & 1))) {
    ++out;
  }
  return static_cast<uint16_t>(out);
}

}  // namespace blink::molt_fingerprinting
