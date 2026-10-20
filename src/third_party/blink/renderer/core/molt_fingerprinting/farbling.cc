// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/farbling.h"

#include <algorithm>
#include <bit>
#include <string>

#include "base/compiler_specific.h"
#include "build/build_config.h"
#include "crypto/hmac.h"

// The F16/F32 paths assemble little-endian values byte by byte; Skia and
// every platform MoltBrowser ships on are little-endian.
#if defined(ARCH_CPU_BIG_ENDIAN)
#error "molt_fingerprinting assumes a little-endian CPU"
#endif

// Speed. The buffer loops run over every pixel or sample a page reads back
// (and over whole video frames where exports are farbled), so:
//  * the pair collapse selects instead of branching on PRF output;
//  * what depends only on alpha (the farbled channel) or on a half's sign
//    (its pair context) is derived once per call, not per pixel or value;
//  * a pixel or value equal to the previous one reuses its result.
// The output is unchanged, bit for bit: farbling_unittest.cc pins it with
// digests recorded before these loops were optimised.

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
// Marks the contexts of float16 magnitudes (the low bit carries the sign).
constexpr uint64_t kHalfContextTag = uint64_t{1} << 41;

// Largest finite float16 magnitude (65504) and the infinity pattern.
constexpr uint32_t kHalfMaxFinite = 0x7bff;
constexpr uint32_t kHalfInfinity = 0x7c00;

// Byte offsets of the canonical R, G and B channels in a 4-byte pixel.
constexpr std::array<size_t, 3> kRGBAOffsets = {0, 1, 2};
constexpr std::array<size_t, 3> kBGRAOffsets = {2, 1, 0};

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

uint16_t LoadU16LE(base::span<const uint8_t, 2> bytes) {
  return static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));
}

void StoreU16LE(base::span<uint8_t, 2> bytes, uint16_t value) {
  bytes[0] = static_cast<uint8_t>(value & 0xff);
  bytes[1] = static_cast<uint8_t>(value >> 8);
}

uint32_t LoadU32LE(base::span<const uint8_t, 4> bytes) {
  return uint32_t{bytes[0]} | (uint32_t{bytes[1]} << 8) |
         (uint32_t{bytes[2]} << 16) | (uint32_t{bytes[3]} << 24);
}

void StoreU32LE(base::span<uint8_t, 4> bytes, uint32_t value) {
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

Farbler::PairContext Farbler::PairContextFor(uint64_t context) const {
  // One multiply for the pair alignment, which depends only on the context
  // (for pixels: the other channels + alpha).
  const uint32_t align =
      static_cast<uint32_t>(((context ^ k_[3]) * (k_[1] | 1)) >> 63);
  return {align, context ^ k_[0] ^ (uint64_t{align} << 63)};
}

// static
uint32_t Farbler::CollapsePair(uint32_t value,
                               uint32_t max,
                               const PairContext& pair) {
  // Pairs are {2k + align, 2k + 1 + align}; with align == 1 the values 0 and
  // `max` (when even) are singletons and never move. One mix round decides
  // pair k: it collapses one time in kCollapseOneIn, onto lo or hi.
  const uint32_t k = (value - pair.align) >> 1;  // Unused if value < align.
  const uint32_t lo = 2 * k + pair.align;
  const uint64_t r = Mix(pair.seed ^ ((uint64_t{k} + 1) * kGolden));
  const bool in_pair = value >= pair.align && lo + 1 <= max;
  const bool collapse = in_pair && (r & (kCollapseOneIn - 1)) == 0;
  const uint32_t target = lo + static_cast<uint32_t>((r / kCollapseOneIn) & 1);
  // Both outcomes are coin flips, so this is a select, not a branch: a branch
  // on them is mispredicted so often that it costs more than the mix.
#if HAS_BUILTIN(__builtin_unpredictable)
  return __builtin_unpredictable(collapse) ? target : value;
#else
  return collapse ? target : value;
#endif
}

uint8_t Farbler::FarbleChannel(size_t channel,
                               uint8_t value,
                               uint8_t other1,
                               uint8_t other2,
                               uint8_t alpha) const {
  // The farbled channel depends only on alpha, and the pair only on the
  // other channels, so moving the channel never changes either decision:
  // that is what makes the mapping idempotent.
  const uint64_t context = uint64_t{channel} | (uint64_t{other1} << 8) |
                           (uint64_t{other2} << 16) | (uint64_t{alpha} << 24);
  return static_cast<uint8_t>(
      CollapsePair(value, 255, PairContextFor(context)));
}

size_t Farbler::ChannelFor(uint8_t alpha) const {
  return static_cast<size_t>(Prf(kDomainChannel, alpha) % 3);
}

std::array<uint8_t, 4> Farbler::FarblePixel(
    const std::array<uint8_t, 4>& rgba) const {
  if (rgba[3] == 0) {
    return rgba;
  }
  const size_t channel = ChannelFor(rgba[3]);
  std::array<uint8_t, 4> out = rgba;
  out[channel] = FarbleChannel(channel, rgba[channel], rgba[(channel + 1) % 3],
                               rgba[(channel + 2) % 3], rgba[3]);
  return out;
}

void Farbler::FarbleRGBA8(base::span<uint8_t> pixels,
                          ChannelOrder order) const {
  FarbleRGBA8Impl(pixels, order, /*premultiplied=*/false);
}

void Farbler::FarbleRGBA8Premul(base::span<uint8_t> pixels,
                                ChannelOrder order) const {
  FarbleRGBA8Impl(pixels, order, /*premultiplied=*/true);
}

void Farbler::FarbleRGBA8Impl(base::span<uint8_t> pixels,
                              ChannelOrder order,
                              bool premultiplied) const {
  const std::array<size_t, 3>& offsets =
      order == ChannelOrder::kBGRA ? kBGRAOffsets : kRGBAOffsets;
  // What depends only on alpha, which pixels mostly share: the farbled
  // channel, the byte offsets of it and of the two others, and the largest
  // value it may take.
  int last_alpha = -1;
  size_t channel = 0;
  size_t at = 0;
  size_t other1_at = 0;
  size_t other2_at = 0;
  uint8_t ceiling = 255;
  // ChannelFor() each alpha, derived on first use (anti-aliased edges cycle
  // through many alphas).
  constexpr uint8_t kNotDerived = 0xff;
  std::array<uint8_t, 256> channel_for_alpha;
  channel_for_alpha.fill(kNotDerived);
  // Canvases are full of runs of one colour: a repeat reuses the previous
  // result. (The all-zero pixel is transparent, so it never needs one.)
  uint32_t last_pixel = 0;
  uint8_t last_in = 0;
  uint8_t last_out = 0;
  for (size_t p = 0; p + 4 <= pixels.size(); p += 4) {
    const base::span<uint8_t, 4> px = pixels.subspan(p).first<4u>();
    const uint32_t pixel = LoadU32LE(px);
    if (pixel == last_pixel) {
      if (last_out != last_in) {
        px[at] = last_out;
      }
      continue;
    }
    const uint8_t alpha = px[3];
    if (alpha == 0) {
      continue;
    }
    if (alpha != last_alpha) {
      last_alpha = alpha;
      uint8_t& derived = channel_for_alpha[alpha];
      if (derived == kNotDerived) {
        derived = static_cast<uint8_t>(ChannelFor(alpha));
      }
      channel = derived;
      at = offsets[channel];
      other1_at = offsets[(channel + 1) % 3];
      other2_at = offsets[(channel + 2) % 3];
      ceiling = premultiplied ? alpha : 255;
    }
    last_pixel = pixel;
    last_in = px[at];
    last_out =
        FarbleChannel(channel, last_in, px[other1_at], px[other2_at], alpha);
    if (last_out > ceiling) {
      // Premultiplied colour channels can't exceed alpha. Not moving is
      // still idempotent: the decision for this tuple is the same next
      // time, and it is again refused.
      last_out = last_in;
    }
    px[at] = last_out;
  }
}

void Farbler::FarbleUnorm8(base::span<uint8_t> pixels, size_t channels) const {
  if (channels == 4) {
    FarbleRGBA8(pixels, ChannelOrder::kRGBA);
    return;
  }
  if (channels < 1 || channels > 3) {
    return;
  }
  // Opaque by definition (A = 255): one channel for the whole buffer.
  const size_t channel = ChannelFor(255);
  if (channel >= channels) {
    // The key moves a channel this format doesn't return (e.g. G of a RED
    // read). The RGBA read of the same data moves that channel, not R.
    return;
  }
  // Components the format doesn't return are 0 for the decision.
  const size_t other1 = (channel + 1) % 3;
  const size_t other2 = (channel + 2) % 3;
  // A repeat reuses the previous result (1 << 24 matches no pixel).
  uint32_t last_pixel = uint32_t{1} << 24;
  uint8_t last_out = 0;
  for (size_t p = 0; p + channels <= pixels.size(); p += channels) {
    const base::span<uint8_t> px = pixels.subspan(p, channels);
    const uint8_t value = px[channel];
    const uint8_t value1 = other1 < channels ? px[other1] : 0;
    const uint8_t value2 = other2 < channels ? px[other2] : 0;
    const uint32_t pixel =
        value | (uint32_t{value1} << 8) | (uint32_t{value2} << 16);
    if (pixel != last_pixel) {
      last_pixel = pixel;
      last_out = FarbleChannel(channel, value, value1, value2, 255);
    }
    px[channel] = last_out;
  }
}

void Farbler::FarbleRGBAF16(base::span<uint8_t> pixels) const {
  FarbleRGBAFloat<2>(pixels);
}

void Farbler::FarbleRGBAF32(base::span<uint8_t> pixels) const {
  FarbleRGBAFloat<4>(pixels);
}

template <size_t kComponentSize>
void Farbler::FarbleRGBAFloat(base::span<uint8_t> pixels) const {
  constexpr size_t kPixelSize = 4 * kComponentSize;
  // A component of `px` as a float, and a float stored as a component.
  auto load = [](base::span<const uint8_t> component) {
    if constexpr (kComponentSize == 2) {
      return HalfToFloat(LoadU16LE(component.first<2u>()));
    } else {
      return std::bit_cast<float>(LoadU32LE(component.first<4u>()));
    }
  };
  auto store = [](base::span<uint8_t> component, float value) {
    if constexpr (kComponentSize == 2) {
      StoreU16LE(component.first<2u>(), FloatToHalf(value));
    } else {
      StoreU32LE(component.first<4u>(), std::bit_cast<uint32_t>(value));
    }
  };
  int last_alpha = -1;
  size_t channel = 0;
  // A repeat of the previous pixel (as stored) gets the result the previous
  // pixel got, which sits just before it.
  std::array<uint8_t, kPixelSize> previous = {};
  for (size_t p = 0; p + kPixelSize <= pixels.size(); p += kPixelSize) {
    base::span<uint8_t, kPixelSize> px = pixels.subspan(p).first<kPixelSize>();
    if (p > 0 && std::ranges::equal(px, previous)) {
      px.copy_from(pixels.subspan(p - kPixelSize).first<kPixelSize>());
      continue;
    }
    base::span(previous).copy_from(px);
    std::array<uint8_t, 4> rgba;
    std::array<bool, 4> in_range;
    for (size_t c = 0; c < 4; ++c) {
      bool ok = false;
      rgba[c] = ToUnorm8(load(px.subspan(c * kComponentSize)), &ok);
      in_range[c] = ok;
    }
    if (rgba[3] == 0) {
      continue;
    }
    if (rgba[3] != last_alpha) {
      last_alpha = rgba[3];
      channel = ChannelFor(rgba[3]);
    }
    const uint8_t out =
        FarbleChannel(channel, rgba[channel], rgba[(channel + 1) % 3],
                      rgba[(channel + 2) % 3], rgba[3]);
    if (out != rgba[channel] && in_range[channel]) {
      // Snap to the target 8-bit level so an 8-bit read of the same data,
      // and a second pass, see exactly `out`.
      store(px.subspan(channel * kComponentSize), out / 255.0f);
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
  // A repeat (compared as bits: NaN != NaN, and -0 vs +0 must stay distinct)
  // reuses the previous result. FarbleSample() leaves +0 alone.
  uint32_t last_in = 0;
  float last_out = 0.0f;
  for (float& sample : samples) {
    const uint32_t bits = std::bit_cast<uint32_t>(sample);
    if (bits != last_in) {
      last_in = bits;
      last_out = FarbleSample(sample);
    }
    sample = last_out;
  }
}

void Farbler::FarbleFloat32(base::span<uint8_t> bytes) const {
  // As FarbleAudio().
  uint32_t last_in = 0;
  uint32_t last_out = 0;
  for (size_t i = 0; i + 4 <= bytes.size(); i += 4) {
    const base::span<uint8_t, 4> value = bytes.subspan(i).first<4u>();
    const uint32_t bits = LoadU32LE(value);
    if (bits != last_in) {
      last_in = bits;
      last_out =
          std::bit_cast<uint32_t>(FarbleSample(std::bit_cast<float>(bits)));
    }
    StoreU32LE(value, last_out);
  }
}

// static
uint16_t Farbler::FarbleHalfIn(uint16_t half, const PairContext& pair) {
  const uint32_t sign = half & 0x8000u;
  const uint32_t magnitude = half & 0x7fffu;
  // Adjacent finite non-zero magnitudes are adjacent values, one ulp apart
  // (also across exponent boundaries). Shifting by one keeps 0 out of every
  // pair, and capping at the largest finite magnitude keeps infinity out.
  const uint32_t moved =
      CollapsePair(magnitude - 1, kHalfMaxFinite - 1, pair) + 1;
  // Zero, infinity and NaN never move.
  const bool finite_non_zero = magnitude != 0 && magnitude < kHalfInfinity;
  return finite_non_zero ? static_cast<uint16_t>(sign | moved) : half;
}

uint16_t Farbler::FarbleHalf(uint16_t half) const {
  return FarbleHalfIn(half,
                      PairContextFor(kHalfContextTag | (uint64_t{half} >> 15)));
}

void Farbler::FarbleFloat16(base::span<uint8_t> bytes) const {
  // A half's context is just its sign: derive both once.
  const std::array<PairContext, 2> pairs = {
      PairContextFor(kHalfContextTag), PairContextFor(kHalfContextTag | 1)};
  for (size_t i = 0; i + 2 <= bytes.size(); i += 2) {
    const base::span<uint8_t, 2> value = bytes.subspan(i).first<2u>();
    const uint16_t half = LoadU16LE(value);
    StoreU16LE(value, FarbleHalfIn(half, pairs[size_t{half} >> 15]));
  }
}

void Farbler::FarbleAudioBytes(base::span<uint8_t> bytes,
                               uint8_t neutral) const {
  const PairContext pair = PairContextFor(kAudioByteContextTag | neutral);
  for (uint8_t& byte : bytes) {
    const uint8_t moved = static_cast<uint8_t>(CollapsePair(byte, 255, pair));
    byte = byte == neutral ? byte : moved;
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
