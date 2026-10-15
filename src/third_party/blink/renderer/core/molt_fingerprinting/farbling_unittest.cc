// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/farbling.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace blink::molt_fingerprinting {
namespace {

SiteKey KeyFromByte(uint8_t first) {
  SiteKey key{};
  for (size_t i = 0; i < key.size(); ++i) {
    key[i] = static_cast<uint8_t>(first + 31 * i);
  }
  return key;
}

std::array<uint8_t, kSessionSeedSize> TestSeed() {
  std::array<uint8_t, kSessionSeedSize> seed;
  for (size_t i = 0; i < seed.size(); ++i) {
    seed[i] = static_cast<uint8_t>(i);
  }
  return seed;
}

// Deterministic xorshift for test data (not the farbling PRF).
class TestRng {
 public:
  explicit TestRng(uint64_t seed) : state_(seed ? seed : 1) {}
  uint64_t Next() {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 7;
    state_ ^= state_ << 17;
    return state_;
  }
  uint8_t Byte() { return static_cast<uint8_t>(Next() & 0xff); }

 private:
  uint64_t state_;
};

std::string Hex(const SiteKey& key) {
  constexpr std::string_view kDigits = "0123456789abcdef";
  std::string out;
  for (uint8_t b : key) {
    out.push_back(kDigits[b >> 4]);
    out.push_back(kDigits[b & 0xf]);
  }
  return out;
}

// --- Key derivation ----------------------------------------------------------

TEST(MoltFarblingTest, DeriveSiteKeyIsHmacSha256OfLabelledSite) {
  // HMAC-SHA256(key = 00 01 .. 1f,
  //             msg = "molt-fp-site-v1\0" 00000013 "https://example.com"
  //                   00000000), computed independently with Python's hmac.
  EXPECT_EQ(Hex(DeriveSiteKey(TestSeed(), "https://example.com", "")),
            "f8c3873776d4562856fffde8abcd4752157712495af4f70180d2b67de4d48d18");
  // ... and with a partition (00000005 "nonce" at the end).
  EXPECT_EQ(Hex(DeriveSiteKey(TestSeed(), "https://example.com", "nonce")),
            "9b4c7da4b82e3e1a63898078245074edc522eb08b8c516b6b914f3b24dd45fe4");
}

TEST(MoltFarblingTest, DeriveSiteKeyIsSiteSeedAndPartitionSpecific) {
  const auto seed = TestSeed();
  auto other_seed = seed;
  other_seed[0] ^= 1;
  const SiteKey a = DeriveSiteKey(seed, "https://a.example", "");
  EXPECT_EQ(a, DeriveSiteKey(seed, "https://a.example", ""));
  EXPECT_NE(a, DeriveSiteKey(seed, "https://b.example", ""));
  EXPECT_NE(a, DeriveSiteKey(seed, "http://a.example", ""));
  EXPECT_NE(a, DeriveSiteKey(other_seed, "https://a.example", ""));
  EXPECT_NE(a, DeriveSiteKey(seed, "https://a.example", "p"));
  // Length prefixes keep "site" + "partition" from colliding with a longer
  // site name.
  const std::string site_nul_x = std::string("https://a.example") + '\0' + "x";
  EXPECT_NE(DeriveSiteKey(seed, "https://a.example", "x"),
            DeriveSiteKey(seed, site_nul_x, ""));
}

// --- PRF ---------------------------------------------------------------------

TEST(MoltFarblingTest, PrfIsDeterministicKeyedAndDomainSeparated) {
  const Farbler f(KeyFromByte(1));
  // Golden values, cross-checked against an independent Python model of
  // Farbler::Prf, so the noise is identical on every platform and compiler.
  EXPECT_EQ(f.Prf(1, 0), 3344717085477591678ull);
  EXPECT_EQ(f.Prf(3, 0x12345678), 13262757542376053265ull);
  EXPECT_EQ(f.Prf(1, 0), Farbler(KeyFromByte(1)).Prf(1, 0));
  EXPECT_NE(f.Prf(1, 0), Farbler(KeyFromByte(2)).Prf(1, 0));
  EXPECT_NE(f.Prf(1, 0), f.Prf(2, 0));
  EXPECT_NE(f.Prf(1, 0), f.Prf(1, 1));
}

// --- Canvas pixels -----------------------------------------------------------

TEST(MoltFarblingTest, TransparentPixelsAreNeverTouched) {
  for (uint8_t k = 0; k < 16; ++k) {
    const Farbler f(KeyFromByte(k));
    for (int v = 0; v < 256; ++v) {
      const std::array<uint8_t, 4> px = {static_cast<uint8_t>(v),
                                         static_cast<uint8_t>(255 - v),
                                         static_cast<uint8_t>(v * 7), 0};
      EXPECT_EQ(f.FarblePixel(px), px);
    }
  }
}

TEST(MoltFarblingTest, GoldenValuesMatchTheReferenceModel) {
  // From an independent Python model of this file, so pixels, samples and
  // hardwareConcurrency farble identically on every platform and compiler.
  const Farbler f(KeyFromByte(1));
  using Px = std::array<uint8_t, 4>;
  EXPECT_EQ(f.FarblePixel(Px{35, 183, 51, 255}), (Px{35, 182, 51, 255}));
  EXPECT_EQ(f.FarblePixel(Px{215, 250, 119, 255}), (Px{215, 251, 119, 255}));
  EXPECT_EQ(f.FarblePixel(Px{166, 99, 59, 255}), (Px{166, 98, 59, 255}));
  EXPECT_EQ(f.FarblePixel(Px{66, 137, 12, 255}), (Px{66, 137, 12, 255}));
  EXPECT_EQ(f.FarblePixel(Px{214, 83, 206, 255}), (Px{214, 83, 206, 255}));
  EXPECT_EQ(std::bit_cast<uint32_t>(
                f.FarbleSample(std::bit_cast<float>(uint32_t{0x3dfcd6ea}))),
            0x3dfcd61du);
  EXPECT_EQ(f.HardwareConcurrency(16), 6u);
  EXPECT_EQ(f.HardwareConcurrency(8), 7u);
}

TEST(MoltFarblingTest, PixelNoiseIsSmallSparseAndIdempotent) {
  const Farbler f(KeyFromByte(7));
  TestRng rng(42);
  int changed = 0;
  constexpr int kSamples = 200000;
  for (int i = 0; i < kSamples; ++i) {
    std::array<uint8_t, 4> px = {rng.Byte(), rng.Byte(), rng.Byte(),
                                 rng.Byte()};
    if (px[3] == 0) {
      px[3] = 1;
    }
    const std::array<uint8_t, 4> out = f.FarblePixel(px);
    EXPECT_EQ(out[3], px[3]);  // Alpha is never changed.
    int channels_changed = 0;
    for (size_t c = 0; c < 3; ++c) {
      const int delta = int{out[c]} - int{px[c]};
      EXPECT_LE(std::abs(delta), 1);
      channels_changed += delta != 0;
    }
    EXPECT_LE(channels_changed, 1);
    changed += channels_changed;
    // Farbling farbled data changes nothing.
    EXPECT_EQ(f.FarblePixel(out), out);
  }
  // About 1 colour in 16 moves (1/8 of pairs collapse, half of each moves).
  const double rate = static_cast<double>(changed) / kSamples;
  EXPECT_GT(rate, 0.045);
  EXPECT_LT(rate, 0.080);
}

TEST(MoltFarblingTest, PixelNoiseCrossesQuantisationBoundaries) {
  // Some moves cross a ">> 1" boundary (e.g. 3 -> 4), so dropping the
  // lowest bit does not undo the noise.
  const Farbler f(KeyFromByte(9));
  TestRng rng(5);
  int crossings = 0;
  for (int i = 0; i < 50000; ++i) {
    const std::array<uint8_t, 4> px = {rng.Byte(), rng.Byte(), rng.Byte(), 255};
    const std::array<uint8_t, 4> out = f.FarblePixel(px);
    for (size_t c = 0; c < 3; ++c) {
      crossings += (out[c] >> 1) != (px[c] >> 1);
    }
  }
  EXPECT_GT(crossings, 0);
}

TEST(MoltFarblingTest, RGBA8MatchesFarblePixelInBothChannelOrders) {
  const Farbler f(KeyFromByte(3));
  TestRng rng(7);
  std::vector<uint8_t> rgba(4 * 4096);
  for (uint8_t& b : rgba) {
    b = rng.Byte();
  }
  std::vector<uint8_t> bgra(rgba.size());
  for (size_t i = 0; i < rgba.size(); i += 4) {
    bgra[i] = rgba[i + 2];
    bgra[i + 1] = rgba[i + 1];
    bgra[i + 2] = rgba[i];
    bgra[i + 3] = rgba[i + 3];
  }
  const std::vector<uint8_t> original = rgba;
  f.FarbleRGBA8(rgba, ChannelOrder::kRGBA);
  f.FarbleRGBA8(bgra, ChannelOrder::kBGRA);
  for (size_t i = 0; i < rgba.size(); i += 4) {
    const std::array<uint8_t, 4> expected = f.FarblePixel(
        {original[i], original[i + 1], original[i + 2], original[i + 3]});
    EXPECT_EQ(rgba[i], expected[0]);
    EXPECT_EQ(rgba[i + 1], expected[1]);
    EXPECT_EQ(rgba[i + 2], expected[2]);
    EXPECT_EQ(rgba[i + 3], expected[3]);
    EXPECT_EQ(bgra[i + 2], expected[0]);
    EXPECT_EQ(bgra[i + 1], expected[1]);
    EXPECT_EQ(bgra[i], expected[2]);
    EXPECT_EQ(bgra[i + 3], expected[3]);
  }
}

TEST(MoltFarblingTest, NoiseDoesNotDependOnPositionOrNeighbours) {
  // The same colour farbles the same way wherever it is, whatever surrounds
  // it: sub-rectangle reads and repeated drawings agree.
  const Farbler f(KeyFromByte(11));
  TestRng rng(9);
  std::vector<uint8_t> a(4 * 1000);
  for (uint8_t& b : a) {
    b = rng.Byte();
  }
  std::vector<uint8_t> whole = a;
  f.FarbleRGBA8(whole, ChannelOrder::kRGBA);
  // A "sub-rectangle": pixels 100..299 farbled on their own.
  std::vector<uint8_t> part(a.begin() + 400, a.begin() + 1200);
  f.FarbleRGBA8(part, ChannelOrder::kRGBA);
  for (size_t i = 0; i < part.size(); ++i) {
    EXPECT_EQ(part[i], whole[400 + i]);
  }
}

TEST(MoltFarblingTest, RGBA8IgnoresTrailingPartialPixel) {
  const Farbler f(KeyFromByte(5));
  std::vector<uint8_t> buffer = {10, 20, 30, 255, 40, 50, 60};
  const std::vector<uint8_t> original = buffer;
  f.FarbleRGBA8(buffer, ChannelOrder::kRGBA);
  EXPECT_EQ(buffer[4], original[4]);
  EXPECT_EQ(buffer[5], original[5]);
  EXPECT_EQ(buffer[6], original[6]);
}

TEST(MoltFarblingTest, FingerprintLikeBuffersDifferAcrossSites) {
  // A busy image (like a fingerprinting canvas) almost surely farbles
  // differently under two site keys.
  TestRng rng(77);
  std::vector<uint8_t> image(4 * 2000);
  for (size_t i = 0; i < image.size(); i += 4) {
    image[i] = rng.Byte();
    image[i + 1] = rng.Byte();
    image[i + 2] = rng.Byte();
    image[i + 3] = 255;
  }
  std::vector<uint8_t> site_a = image;
  std::vector<uint8_t> site_b = image;
  Farbler(DeriveSiteKey(TestSeed(), "https://a.example", ""))
      .FarbleRGBA8(site_a, ChannelOrder::kRGBA);
  Farbler(DeriveSiteKey(TestSeed(), "https://b.example", ""))
      .FarbleRGBA8(site_b, ChannelOrder::kRGBA);
  EXPECT_NE(site_a, image);
  EXPECT_NE(site_b, image);
  EXPECT_NE(site_a, site_b);
}

// --- Float pixels ------------------------------------------------------------

std::vector<uint8_t> ToF16(const std::vector<uint8_t>& rgba8) {
  std::vector<uint8_t> out(rgba8.size() * 2);
  for (size_t i = 0; i < rgba8.size(); ++i) {
    const uint16_t h = FloatToHalf(rgba8[i] / 255.0f);
    out[2 * i] = static_cast<uint8_t>(h & 0xff);
    out[2 * i + 1] = static_cast<uint8_t>(h >> 8);
  }
  return out;
}

uint8_t F16ToUnorm8(const std::vector<uint8_t>& f16, size_t channel_index) {
  const uint16_t h = static_cast<uint16_t>(f16[2 * channel_index] |
                                           (f16[2 * channel_index + 1] << 8));
  return static_cast<uint8_t>(HalfToFloat(h) * 255.0f + 0.5f);
}

TEST(MoltFarblingTest, F16AgreesWithRGBA8AndIsIdempotent) {
  const Farbler f(KeyFromByte(13));
  TestRng rng(3);
  std::vector<uint8_t> rgba8(4 * 3000);
  for (uint8_t& b : rgba8) {
    b = rng.Byte();
  }
  std::vector<uint8_t> f16 = ToF16(rgba8);
  f.FarbleRGBA8(rgba8, ChannelOrder::kRGBA);
  f.FarbleRGBAF16(f16);
  for (size_t i = 0; i < rgba8.size(); ++i) {
    EXPECT_EQ(F16ToUnorm8(f16, i), rgba8[i]) << "channel byte " << i;
  }
  const std::vector<uint8_t> once = f16;
  f.FarbleRGBAF16(f16);
  EXPECT_EQ(f16, once);
}

TEST(MoltFarblingTest, F32AgreesWithRGBA8AndIsIdempotent) {
  const Farbler f(KeyFromByte(17));
  TestRng rng(4);
  std::vector<uint8_t> rgba8(4 * 3000);
  for (uint8_t& b : rgba8) {
    b = rng.Byte();
  }
  std::vector<uint8_t> f32(rgba8.size() * 4);
  for (size_t i = 0; i < rgba8.size(); ++i) {
    const uint32_t bits = std::bit_cast<uint32_t>(rgba8[i] / 255.0f);
    for (size_t b = 0; b < 4; ++b) {
      f32[4 * i + b] = static_cast<uint8_t>((bits >> (8 * b)) & 0xff);
    }
  }
  f.FarbleRGBA8(rgba8, ChannelOrder::kRGBA);
  f.FarbleRGBAF32(f32);
  for (size_t i = 0; i < rgba8.size(); ++i) {
    uint32_t bits = 0;
    for (size_t b = 0; b < 4; ++b) {
      bits |= uint32_t{f32[4 * i + b]} << (8 * b);
    }
    EXPECT_EQ(static_cast<uint8_t>(std::bit_cast<float>(bits) * 255.0f + 0.5f),
              rgba8[i]);
  }
  const std::vector<uint8_t> once = f32;
  f.FarbleRGBAF32(f32);
  EXPECT_EQ(f32, once);
}

TEST(MoltFarblingTest, FloatPixelsOutsideUnitRangeAreNotMoved) {
  const Farbler f(KeyFromByte(19));
  // Every channel at 2.0 (HDR): decisions clamp, but nothing is written.
  std::vector<uint8_t> f16(8 * 64);
  for (size_t i = 0; i < f16.size(); i += 2) {
    const uint16_t two = FloatToHalf(2.0f);
    f16[i] = static_cast<uint8_t>(two & 0xff);
    f16[i + 1] = static_cast<uint8_t>(two >> 8);
  }
  const std::vector<uint8_t> original = f16;
  f.FarbleRGBAF16(f16);
  EXPECT_EQ(f16, original);
}

TEST(MoltFarblingTest, HalfFloatConversionRoundTripsAndRounds) {
  for (uint32_t h = 0; h <= 0xffff; ++h) {
    const uint16_t half = static_cast<uint16_t>(h);
    const bool is_nan = (half & 0x7c00) == 0x7c00 && (half & 0x3ff) != 0;
    if (is_nan) {
      EXPECT_TRUE(std::isnan(HalfToFloat(half)));
      continue;
    }
    EXPECT_EQ(FloatToHalf(HalfToFloat(half)), half) << h;
  }
  EXPECT_EQ(FloatToHalf(1.0f), 0x3c00);
  EXPECT_EQ(FloatToHalf(0.5f), 0x3800);
  EXPECT_EQ(FloatToHalf(-2.0f), 0xc000);
  EXPECT_EQ(FloatToHalf(65504.0f), 0x7bff);
  EXPECT_EQ(FloatToHalf(65536.0f), 0x7c00);               // Overflow.
  EXPECT_EQ(FloatToHalf(std::ldexp(1.0f, -24)), 0x0001);  // Min subnormal.
  EXPECT_EQ(FloatToHalf(std::ldexp(1.0f, -26)), 0x0000);  // Rounds to 0.
  // 1 + 2^-11 is exactly halfway between 1 and the next half: ties to even.
  EXPECT_EQ(FloatToHalf(1.0f + std::ldexp(1.0f, -11)), 0x3c00);
  EXPECT_EQ(FloatToHalf(1.0f + 3 * std::ldexp(1.0f, -11)), 0x3c02);
  EXPECT_EQ(HalfToFloat(0x3555), 0.333251953125f);
}

// --- Audio -------------------------------------------------------------------

TEST(MoltFarblingTest, AudioSamplesGetTinyIdempotentNoise) {
  const Farbler f(KeyFromByte(21));
  TestRng rng(11);
  int changed = 0;
  for (int i = 0; i < 100000; ++i) {
    const float s =
        (static_cast<float>(rng.Next() % 2000001) - 1000000.0f) / 1000000.0f;
    const float out = f.FarbleSample(s);
    if (s == 0.0f) {
      EXPECT_EQ(std::bit_cast<uint32_t>(out), std::bit_cast<uint32_t>(s));
      continue;
    }
    // Sign and exponent are kept; only the low mantissa bits change.
    EXPECT_EQ(std::bit_cast<uint32_t>(out) >> kAudioNoiseBits,
              std::bit_cast<uint32_t>(s) >> kAudioNoiseBits);
    EXPECT_LE(std::fabs(out - s), std::fabs(s) * std::ldexp(1.0f, -15));
    EXPECT_EQ(f.FarbleSample(out), out);
    changed += out != s;
  }
  EXPECT_GT(changed, 90000);
}

TEST(MoltFarblingTest, AudioLeavesSpecialValuesAlone) {
  const Farbler f(KeyFromByte(23));
  const float specials[] = {0.0f,
                            -0.0f,
                            std::numeric_limits<float>::denorm_min(),
                            -std::numeric_limits<float>::denorm_min(),
                            std::numeric_limits<float>::infinity(),
                            -std::numeric_limits<float>::infinity()};
  for (float s : specials) {
    EXPECT_EQ(std::bit_cast<uint32_t>(f.FarbleSample(s)),
              std::bit_cast<uint32_t>(s));
  }
  EXPECT_TRUE(
      std::isnan(f.FarbleSample(std::numeric_limits<float>::quiet_NaN())));
}

TEST(MoltFarblingTest, AudioBufferNoiseIsPerSiteAndRepeatable) {
  std::vector<float> signal(4096);
  for (size_t i = 0; i < signal.size(); ++i) {
    signal[i] = std::sin(static_cast<float>(i) * 0.05f) * 0.8f;
  }
  std::vector<float> a1 = signal;
  std::vector<float> a2 = signal;
  std::vector<float> b = signal;
  const Farbler site_a(KeyFromByte(30));
  site_a.FarbleAudio(a1);
  site_a.FarbleAudio(a2);
  Farbler(KeyFromByte(31)).FarbleAudio(b);
  EXPECT_EQ(a1, a2);
  EXPECT_NE(a1, b);
  // A typical audio fingerprint: the sum of |samples| over a window.
  double sum_a = 0, sum_b = 0, sum_real = 0;
  for (size_t i = 0; i < signal.size(); ++i) {
    sum_a += std::fabs(a1[i]);
    sum_b += std::fabs(b[i]);
    sum_real += std::fabs(signal[i]);
  }
  EXPECT_NE(sum_a, sum_real);
  EXPECT_NE(sum_a, sum_b);
  EXPECT_NEAR(sum_a, sum_real, sum_real * 1e-4);
}

TEST(MoltFarblingTest, AnalyserBytesKeepNeutralAndMoveByAtMostOne) {
  const Farbler f(KeyFromByte(25));
  for (uint8_t neutral : {uint8_t{0}, uint8_t{128}}) {
    std::vector<uint8_t> bytes(256);
    for (size_t i = 0; i < bytes.size(); ++i) {
      bytes[i] = static_cast<uint8_t>(i);
    }
    std::vector<uint8_t> out = bytes;
    f.FarbleAudioBytes(out, neutral);
    EXPECT_EQ(out[neutral], neutral);
    int changed = 0;
    for (size_t i = 0; i < out.size(); ++i) {
      EXPECT_LE(std::abs(int{out[i]} - int{bytes[i]}), 1);
      changed += out[i] != bytes[i];
    }
    EXPECT_LT(changed, 64);
  }
}

// --- hardwareConcurrency -----------------------------------------------------

TEST(MoltFarblingTest, HardwareConcurrencyStaysInRange) {
  for (uint8_t k = 0; k < 64; ++k) {
    const Farbler f(KeyFromByte(k));
    for (unsigned real = 0; real <= 128; ++real) {
      const unsigned value = f.HardwareConcurrency(real);
      if (real <= 2) {
        EXPECT_EQ(value, real);
      } else {
        EXPECT_GE(value, 2u);
        EXPECT_LE(value, real);
        EXPECT_EQ(value, f.HardwareConcurrency(real));
      }
    }
  }
}

TEST(MoltFarblingTest, HardwareConcurrencyVariesAcrossSites) {
  std::set<unsigned> values;
  for (uint8_t k = 0; k < 64; ++k) {
    values.insert(Farbler(KeyFromByte(k)).HardwareConcurrency(16));
  }
  EXPECT_GT(values.size(), 4u);
}

// --- Letterboxing ------------------------------------------------------------

TEST(MoltFarblingTest, LetterboxBucketRoundsDownAndCaps) {
  auto bucket = [](int w, int h) {
    const LetterboxSize s = LetterboxBucket(w, h);
    return std::make_pair(s.width, s.height);
  };
  EXPECT_EQ(bucket(1234, 777), std::make_pair(1200, 700));
  EXPECT_EQ(bucket(1400, 900), std::make_pair(1400, 900));
  EXPECT_EQ(bucket(1399, 899), std::make_pair(1200, 800));
  EXPECT_EQ(bucket(3840, 2160), std::make_pair(1400, 900));
  EXPECT_EQ(bucket(150, 50), std::make_pair(200, 100));
  EXPECT_EQ(bucket(0, 0), std::make_pair(200, 100));
  EXPECT_EQ(bucket(-20, -1), std::make_pair(200, 100));
  EXPECT_EQ(bucket(200, 100), std::make_pair(200, 100));
  EXPECT_EQ(bucket(601, 401), std::make_pair(600, 400));
}

}  // namespace
}  // namespace blink::molt_fingerprinting
