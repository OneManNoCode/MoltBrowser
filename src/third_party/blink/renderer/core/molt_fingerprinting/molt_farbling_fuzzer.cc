// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The fingerprinting protection's pure parts (farbling.h, fonts.h) on what a
// page controls: the pixels it reads back from a canvas or a WebGL
// framebuffer (8-bit, premultiplied, half and single float), the audio it
// renders, the font family names it asks for, and the site and partition
// its key is derived for.
//
// Checks: no memory or UB error; every buffer routine gives what its
// documented per-value rule gives value by value (so the run caches never
// change a result), and its documented promises hold: noise is idempotent,
// moves at most one colour channel by one level, never touches transparent
// pixels, silence, zero, infinities or NaN, keeps premultiplied data valid,
// and agrees between RGBA and BGRA, between 8-bit reads with 1-4 channels,
// and between a float read and an 8-bit read of the same data; half floats
// round-trip; font names compare the same in UTF-8, Latin-1 and UTF-16 and
// regardless of ASCII case and whitespace, the hashed index finds exactly
// what a linear scan finds, and the visibility rules follow from the lists
// and the site's sample.

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <fuzzer/FuzzedDataProvider.h>

#include "base/check.h"
#include "base/containers/span.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/farbling.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/font_allowlists.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/fonts.h"

namespace blink::molt_fingerprinting {
namespace {

using Pixel = std::array<uint8_t, 4>;

uint16_t Load16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
uint32_t Load32(const uint8_t* p) {
  return uint32_t{p[0]} | (uint32_t{p[1]} << 8) | (uint32_t{p[2]} << 16) |
         (uint32_t{p[3]} << 24);
}
void Store16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
void Store32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    p[i] = static_cast<uint8_t>(v >> (8 * i));
  }
}

int Diff(uint8_t a, uint8_t b) {
  return std::abs(int{a} - int{b});
}

// The documented pixel rule, checked on one canonical pixel.
Pixel CheckedPixel(const Farbler& f, const Pixel& in) {
  const Pixel out = f.FarblePixel(in);
  CHECK(out[3] == in[3]) << "alpha moved";
  if (in[3] == 0) {
    CHECK(out == in) << "a transparent pixel moved";
  }
  CHECK(Diff(out[0], in[0]) + Diff(out[1], in[1]) + Diff(out[2], in[2]) <= 1)
      << "more than one channel or more than one level moved";
  CHECK(f.FarblePixel(out) == out) << "pixel noise isn't idempotent";
  return out;
}

// --- 8-bit pixels ------------------------------------------------------------

void CheckRgba8(const Farbler& f, const std::vector<uint8_t>& in) {
  std::vector<uint8_t> want = in;
  std::vector<uint8_t> want_premul = in;
  for (size_t p = 0; p + 4 <= in.size(); p += 4) {
    const Pixel px = {in[p], in[p + 1], in[p + 2], in[p + 3]};
    const Pixel out = CheckedPixel(f, px);
    for (size_t c = 0; c < 4; ++c) {
      want[p + c] = out[c];
      // Premultiplied: the same decision, refused above alpha.
      want_premul[p + c] = out[c] > px[3] && c < 3 ? px[c] : out[c];
    }
  }

  std::vector<uint8_t> got = in;
  f.FarbleRGBA8(base::span(got), ChannelOrder::kRGBA);
  CHECK(got == want) << "FarbleRGBA8 differs from FarblePixel";
  f.FarbleRGBA8(base::span(got), ChannelOrder::kRGBA);
  CHECK(got == want) << "FarbleRGBA8 isn't idempotent";

  // A BGRA snapshot of the same canvas farbles identically.
  auto swap_rb = [](std::vector<uint8_t>& v) {
    for (size_t p = 0; p + 4 <= v.size(); p += 4) {
      std::swap(v[p], v[p + 2]);
    }
  };
  std::vector<uint8_t> bgra = in;
  swap_rb(bgra);
  f.FarbleRGBA8(base::span(bgra), ChannelOrder::kBGRA);
  swap_rb(bgra);
  CHECK(bgra == want) << "BGRA and RGBA disagree";

  std::vector<uint8_t> premul = in;
  f.FarbleRGBA8Premul(base::span(premul), ChannelOrder::kRGBA);
  CHECK(premul == want_premul) << "FarbleRGBA8Premul differs from its rule";
  for (size_t p = 0; p + 4 <= in.size(); p += 4) {
    for (size_t c = 0; c < 3; ++c) {
      if (in[p + c] <= in[p + 3]) {
        CHECK(premul[p + c] <= premul[p + 3])
            << "valid premultiplied data became invalid";
      }
    }
  }
  std::vector<uint8_t> premul2 = premul;
  f.FarbleRGBA8Premul(base::span(premul2), ChannelOrder::kRGBA);
  CHECK(premul2 == premul) << "FarbleRGBA8Premul isn't idempotent";
  std::vector<uint8_t> premul_bgra = in;
  swap_rb(premul_bgra);
  f.FarbleRGBA8Premul(base::span(premul_bgra), ChannelOrder::kBGRA);
  swap_rb(premul_bgra);
  CHECK(premul_bgra == want_premul);
}

// WebGL readPixels with RED / RG / RGB / RGBA: the channels returned are
// exactly those an RGBA read of the same data (G = B = 0, A = 255) returns.
void CheckUnorm8(const Farbler& f, const std::vector<uint8_t>& in) {
  for (size_t channels = 0; channels <= 5; ++channels) {
    std::vector<uint8_t> got = in;
    f.FarbleUnorm8(base::span(got), channels);
    if (channels < 1 || channels > 4) {
      CHECK(got == in) << "an unsupported channel count changed data";
      continue;
    }
    std::vector<uint8_t> want = in;
    for (size_t p = 0; p + channels <= in.size(); p += channels) {
      Pixel px = {0, 0, 0, 255};
      for (size_t c = 0; c < channels; ++c) {
        px[c] = in[p + c];
      }
      const Pixel out = f.FarblePixel(px);
      for (size_t c = 0; c < channels; ++c) {
        want[p + c] = out[c];
      }
    }
    CHECK(got == want) << "a " << channels << "-channel read disagrees";
  }
}

// --- Float pixels ------------------------------------------------------------

// The 8-bit level a float channel is decided on (documented in farbling.h:
// clamped to [0, 1], NaN as 0).
uint8_t ToLevel(float v) {
  if (!(v >= 0.0f)) {
    v = 0.0f;
  } else if (v > 1.0f) {
    v = 1.0f;
  }
  return static_cast<uint8_t>(v * 255.0f + 0.5f);
}

template <size_t kComponentSize>
float LoadComponent(const uint8_t* p) {
  if constexpr (kComponentSize == 2) {
    return HalfToFloat(Load16(p));
  } else {
    return std::bit_cast<float>(Load32(p));
  }
}

template <size_t kComponentSize>
void CheckFloatPixels(const Farbler& f, const std::vector<uint8_t>& in) {
  constexpr size_t kPixelSize = 4 * kComponentSize;
  std::vector<uint8_t> want = in;
  for (size_t p = 0; p + kPixelSize <= in.size(); p += kPixelSize) {
    Pixel level;
    std::array<float, 4> value;
    for (size_t c = 0; c < 4; ++c) {
      value[c] = LoadComponent<kComponentSize>(&in[p + c * kComponentSize]);
      level[c] = ToLevel(value[c]);
    }
    const Pixel out = f.FarblePixel(level);
    for (size_t c = 0; c < 3; ++c) {
      if (out[c] == level[c] || !(value[c] >= 0.0f && value[c] <= 1.0f)) {
        continue;  // Out-of-range (HDR) values are never moved.
      }
      uint8_t* at = &want[p + c * kComponentSize];
      if constexpr (kComponentSize == 2) {
        Store16(at, FloatToHalf(out[c] / 255.0f));
      } else {
        Store32(at, std::bit_cast<uint32_t>(out[c] / 255.0f));
      }
      // An 8-bit read of the farbled data sees exactly the farbled level.
      CHECK(ToLevel(LoadComponent<kComponentSize>(at)) == out[c]);
    }
  }
  std::vector<uint8_t> got = in;
  if constexpr (kComponentSize == 2) {
    f.FarbleRGBAF16(base::span(got));
  } else {
    f.FarbleRGBAF32(base::span(got));
  }
  CHECK(got == want) << "float pixels differ from the 8-bit decision";
  if constexpr (kComponentSize == 2) {
    f.FarbleRGBAF16(base::span(got));
  } else {
    f.FarbleRGBAF32(base::span(got));
  }
  CHECK(got == want) << "float pixel noise isn't idempotent";
}

// --- Float readback and audio -----------------------------------------------

uint32_t CheckedSample(const Farbler& f, uint32_t bits) {
  const float out = f.FarbleSample(std::bit_cast<float>(bits));
  const uint32_t out_bits = std::bit_cast<uint32_t>(out);
  const uint32_t exponent = (bits >> 23) & 0xff;
  if (exponent == 0 || exponent == 0xff) {
    CHECK(out_bits == bits) << "silence, a subnormal, inf or NaN moved";
  }
  CHECK(((out_bits ^ bits) >> kAudioNoiseBits) == 0)
      << "audio noise reached above the low mantissa bits";
  CHECK(std::bit_cast<uint32_t>(f.FarbleSample(out)) == out_bits)
      << "audio noise isn't idempotent";
  return out_bits;
}

uint16_t CheckedHalf(const Farbler& f, uint16_t half) {
  const uint16_t out = f.FarbleHalf(half);
  const uint32_t magnitude = half & 0x7fffu;
  const uint32_t out_magnitude = out & 0x7fffu;
  CHECK((out & 0x8000u) == (half & 0x8000u)) << "a half's sign moved";
  if (magnitude == 0 || magnitude >= 0x7c00u) {
    CHECK(out == half) << "a zero, infinite or NaN half moved";
  } else {
    CHECK(out_magnitude >= 1 && out_magnitude <= 0x7bffu)
        << "a half moved to zero or infinity";
    CHECK(std::abs(int(out_magnitude) - int(magnitude)) <= 1);
  }
  CHECK(f.FarbleHalf(out) == out) << "half noise isn't idempotent";
  return out;
}

void CheckFloatReadback(const Farbler& f, const std::vector<uint8_t>& in) {
  std::vector<uint8_t> want32 = in;
  std::vector<float> samples;
  std::vector<uint32_t> want_samples;
  for (size_t i = 0; i + 4 <= in.size(); i += 4) {
    const uint32_t bits = Load32(&in[i]);
    const uint32_t out = CheckedSample(f, bits);
    Store32(&want32[i], out);
    samples.push_back(std::bit_cast<float>(bits));
    want_samples.push_back(out);
  }
  std::vector<uint8_t> got32 = in;
  f.FarbleFloat32(base::span(got32));
  CHECK(got32 == want32) << "FarbleFloat32 differs from FarbleSample";
  f.FarbleAudio(base::span(samples));
  for (size_t i = 0; i < samples.size(); ++i) {
    CHECK(std::bit_cast<uint32_t>(samples[i]) == want_samples[i])
        << "FarbleAudio differs from FarbleSample";
  }

  std::vector<uint8_t> want16 = in;
  for (size_t i = 0; i + 2 <= in.size(); i += 2) {
    Store16(&want16[i], CheckedHalf(f, Load16(&in[i])));
  }
  std::vector<uint8_t> got16 = in;
  f.FarbleFloat16(base::span(got16));
  CHECK(got16 == want16) << "FarbleFloat16 differs from FarbleHalf";
}

void CheckAudioBytes(const Farbler& f,
                     const std::vector<uint8_t>& in,
                     uint8_t neutral) {
  std::vector<uint8_t> got = in;
  f.FarbleAudioBytes(base::span(got), neutral);
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == neutral) {
      CHECK(got[i] == neutral) << "analyser silence moved";
    }
    CHECK(Diff(got[i], in[i]) <= 1);
  }
  std::vector<uint8_t> again = got;
  f.FarbleAudioBytes(base::span(again), neutral);
  CHECK(again == got) << "analyser noise isn't idempotent";
}

void CheckHalfConversions(const std::vector<uint8_t>& in) {
  for (size_t i = 0; i + 2 <= in.size(); i += 2) {
    const uint16_t half = Load16(&in[i]);
    const float value = HalfToFloat(half);
    const bool nan = (half & 0x7c00u) == 0x7c00u && (half & 0x3ffu) != 0;
    CHECK(std::isnan(value) == nan);
    if (!nan) {
      CHECK(FloatToHalf(value) == half) << "a half doesn't round-trip";
    }
#if defined(__FLT16_MANT_DIG__)
    // The compiler's own conversions (IEEE, round to nearest even).
    if (!nan) {
      CHECK(std::bit_cast<uint32_t>(value) ==
            std::bit_cast<uint32_t>(
                static_cast<float>(std::bit_cast<_Float16>(half))));
    }
#endif
  }
  for (size_t i = 0; i + 4 <= in.size(); i += 4) {
    const float value = std::bit_cast<float>(Load32(&in[i]));
    const uint16_t half = FloatToHalf(value);
    if (std::isnan(value)) {
      CHECK(std::isnan(HalfToFloat(half))) << "NaN became a number";
      continue;
    }
#if defined(__FLT16_MANT_DIG__)
    CHECK(half == std::bit_cast<uint16_t>(static_cast<_Float16>(value)))
        << "FloatToHalf doesn't round to nearest even";
#endif
  }
}

// --- No-key uniformity --------------------------------------------------------

void CheckUniformity(const Farbler& f, FuzzedDataProvider& p) {
  const unsigned real = p.ConsumeIntegral<unsigned>();
  const unsigned cores = f.HardwareConcurrency(real);
  if (real <= 2) {
    CHECK(cores == real);
  } else {
    CHECK(cores >= 2 && cores <= real);
  }

  const int w = p.ConsumeIntegral<int>();
  const int h = p.ConsumeIntegral<int>();
  const LetterboxSize box = LetterboxBucket(w, h);
  CHECK(box.width >= kLetterboxStepWidth && box.width <= kLetterboxMaxWidth &&
        box.width % kLetterboxStepWidth == 0);
  CHECK(box.height >= kLetterboxStepHeight &&
        box.height <= kLetterboxMaxHeight &&
        box.height % kLetterboxStepHeight == 0);
  if (w >= kLetterboxStepWidth) {
    CHECK(box.width <= w);
  }
  if (h >= kLetterboxStepHeight) {
    CHECK(box.height <= h);
  }

  const double length = std::bit_cast<double>(p.ConsumeIntegral<uint64_t>());
  const double snapped = SnapViewportLength(length);
  if (std::isnan(length)) {
    CHECK(std::isnan(snapped));
  } else if (snapped != length) {
    CHECK(snapped == std::floor(snapped) &&
          std::fabs(snapped - length) <= kViewportSnapTolerance)
        << "a viewport length moved further than the tolerance";
  }

  const int thickness = p.ConsumeIntegral<int>();
  const float zoom = std::bit_cast<float>(p.ConsumeIntegral<uint32_t>());
  const int device = UniformScrollbarDevicePixels(thickness, zoom);
  if (thickness <= 0) {
    CHECK(device == 0);
  } else {
    CHECK(device >= 1);
    if (zoom > 0.0f && zoom < 1000.0f && thickness < (1 << 20)) {
      CHECK(device == 1 ||
            std::fabs(device - static_cast<double>(thickness) * zoom) <=
                0.5 + 1e-6);
    }
  }
}

// --- Font names ---------------------------------------------------------------

// Strict UTF-8 to code points; false on anything malformed.
bool DecodeUtf8(std::string_view s, std::vector<char32_t>* out) {
  for (size_t i = 0; i < s.size();) {
    const uint8_t b = static_cast<uint8_t>(s[i]);
    size_t len;
    char32_t c;
    char32_t min;
    if (b < 0x80) {
      len = 1, c = b, min = 0;
    } else if ((b & 0xe0) == 0xc0) {
      len = 2, c = b & 0x1f, min = 0x80;
    } else if ((b & 0xf0) == 0xe0) {
      len = 3, c = b & 0x0f, min = 0x800;
    } else if ((b & 0xf8) == 0xf0) {
      len = 4, c = b & 0x07, min = 0x10000;
    } else {
      return false;
    }
    if (i + len > s.size()) {
      return false;
    }
    for (size_t k = 1; k < len; ++k) {
      const uint8_t cont = static_cast<uint8_t>(s[i + k]);
      if ((cont & 0xc0) != 0x80) {
        return false;
      }
      c = (c << 6) | (cont & 0x3f);
    }
    if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) {
      return false;
    }
    out->push_back(c);
    i += len;
  }
  return true;
}

// |name| with ASCII letters' case flipped and ASCII whitespace inserted
// before ASCII bytes, as |p| decides: the same name in normalized form.
std::string Respell(std::string_view name, FuzzedDataProvider& p) {
  static constexpr char kWhitespace[] = {' ', '\t', '\n', '\f', '\r'};
  std::string out;
  for (char c : name) {
    if (static_cast<uint8_t>(c) < 0x80 && p.ConsumeBool()) {
      out.push_back(kWhitespace[p.ConsumeIntegralInRange<size_t>(0, 4)]);
    }
    if (((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) &&
        p.ConsumeBool()) {
      c = static_cast<char>(c ^ 0x20);
    }
    out.push_back(c);
  }
  if (p.ConsumeBool()) {
    out.push_back(' ');
  }
  return out;
}

const FontListEntry* LinearFind(FontPlatform platform, const FontName& name) {
  for (const FontListEntry& entry : FontListFor(platform)) {
    if (name.Equals(FontName::Utf8(entry.name))) {
      return &entry;
    }
  }
  return nullptr;
}

void CheckFontName(const Farbler& farbler,
                   FontPlatform platform,
                   std::string_view raw,
                   FuzzedDataProvider& p) {
  const FontName name = FontName::Utf8(raw);
  const uint64_t hash = name.Hash();
  CHECK(name.Equals(name));

  // ASCII case and whitespace don't matter.
  const std::string respelled = Respell(raw, p);
  const FontName other = FontName::Utf8(respelled);
  CHECK(other.Equals(name) && name.Equals(other))
      << "respelling a name changed it";
  CHECK(other.Hash() == hash);
  CHECK(other.IsEmpty() == name.IsEmpty());

  // The same name in UTF-16 and, where it fits, Latin-1.
  std::vector<char32_t> code_points;
  if (DecodeUtf8(raw, &code_points)) {
    std::u16string utf16;
    std::vector<uint8_t> latin1;
    bool fits_latin1 = true;
    for (char32_t c : code_points) {
      if (c >= 0x10000) {
        utf16.push_back(static_cast<char16_t>(0xd800 + ((c - 0x10000) >> 10)));
        utf16.push_back(static_cast<char16_t>(0xdc00 + ((c - 0x10000) & 0x3ff)));
      } else {
        utf16.push_back(static_cast<char16_t>(c));
      }
      fits_latin1 &= c <= 0xff;
      latin1.push_back(static_cast<uint8_t>(c));
    }
    const FontName as16 = FontName::Utf16(base::span(utf16));
    CHECK(as16.Equals(name) && name.Equals(as16)) << "UTF-16 disagrees";
    CHECK(as16.Hash() == hash);
    if (fits_latin1) {
      const FontName as_latin1 = FontName::Latin1(base::span(latin1));
      CHECK(as_latin1.Equals(name) && as_latin1.Hash() == hash)
          << "Latin-1 disagrees";
    }
  }
  // Raw bytes as UTF-16 (lone surrogates too) and Latin-1: no errors.
  std::u16string units(raw.size() / 2, u'\0');
  std::memcpy(units.data(), raw.data(), units.size() * 2);
  const FontName odd16 = FontName::Utf16(base::span(units));
  CHECK(odd16.Equals(odd16) == true);
  (void)odd16.Hash();
  (void)FontName::Latin1(base::as_byte_span(raw)).Hash();

  // The hashed index finds exactly what a linear scan finds.
  const FontListEntry* found = FindInFontList(platform, name, hash);
  const FontListEntry* linear = LinearFind(platform, name);
  CHECK((found != nullptr) == (linear != nullptr))
      << "the hashed index and a linear scan disagree";
  if (found) {
    CHECK(name.Equals(FontName::Utf8(found->name)));
  }
  CHECK(IsAllowListedFamily(platform, name, hash) ==
        (found && found->alias_of.empty()));

  // What a protected context may see.
  const FontVisibility all = FontVisibility::All();
  CHECK(all.ForFamilyRequest(name).kind ==
        FontVisibility::Request::Kind::kUnrestricted);
  CHECK(all.IsFamilyVisible(name));
  for (const FontVisibility& vis :
       {FontVisibility::AllowList(platform),
        FontVisibility::AllowListAndSample(platform, farbler)}) {
    const bool sampled = vis.IsSampled(hash);
    if (vis.access() == FontVisibility::Access::kAllowList) {
      CHECK(!sampled);
    }
    const bool listed = found && found->alias_of.empty();
    CHECK(vis.IsFamilyVisible(name) == (!name.IsEmpty() && (listed || sampled)));
    const FontVisibility::Request request = vis.ForFamilyRequest(name);
    using Kind = FontVisibility::Request::Kind;
    if (name.Equals(FontName::Utf8("system-ui"))) {
      CHECK(request.kind == Kind::kUnrestricted);
      continue;
    }
    switch (request.kind) {
      case Kind::kUnrestricted:
        CHECK(false) << "a protected context gave unrestricted access";
        break;
      case Kind::kBlocked:
        CHECK(!found && !sampled);
        break;
      case Kind::kLookUp:
        CHECK(listed || sampled);
        break;
      case Kind::kLookUpInstead: {
        CHECK(found && !found->alias_of.empty() && !sampled);
        CHECK(request.instead == found->alias_of);
        const FontName instead = FontName::Utf8(request.instead);
        CHECK(IsAllowListedFamily(platform, instead, instead.Hash()))
            << "an alias of a family that isn't listed";
        break;
      }
    }
  }
}

}  // namespace
}  // namespace blink::molt_fingerprinting

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace blink::molt_fingerprinting;
  FuzzedDataProvider p(data, size);

  // The page's buffer, its site and partition, a font name it asks for.
  const std::string buffer_bytes = p.ConsumeRandomLengthString(4096);
  const std::vector<uint8_t> buffer(buffer_bytes.begin(), buffer_bytes.end());
  const std::string site = p.ConsumeRandomLengthString(64);
  const std::string partition = p.ConsumeRandomLengthString(32);
  std::string font = p.ConsumeRandomLengthString(128);

  std::array<uint8_t, kSessionSeedSize> seed{};
  for (uint8_t& b : seed) {
    b = p.ConsumeIntegral<uint8_t>();
  }
  const SiteKey key = DeriveSiteKey(seed, site, partition);
  CHECK(key == DeriveSiteKey(seed, site, partition));
  if (!partition.empty()) {
    // Length-prefixed: moving a byte between site and partition is
    // another key.
    CHECK(key != DeriveSiteKey(seed, site + partition[0], partition.substr(1)))
        << "two (site, partition) pairs share a key";
  }
  const Farbler farbler(key);

  CheckRgba8(farbler, buffer);
  CheckUnorm8(farbler, buffer);
  CheckFloatPixels<2>(farbler, buffer);
  CheckFloatPixels<4>(farbler, buffer);
  CheckFloatReadback(farbler, buffer);
  CheckAudioBytes(farbler, buffer, p.ConsumeIntegral<uint8_t>());
  CheckHalfConversions(buffer);
  CheckUniformity(farbler, p);

  // A font name the page made up, or a listed one respelled.
  const FontPlatform platform =
      static_cast<FontPlatform>(p.ConsumeIntegralInRange<int>(0, 3));
  const base::span<const FontListEntry> list = FontListFor(platform);
  if (p.ConsumeBool() && !list.empty()) {
    const FontListEntry& entry =
        list[p.ConsumeIntegralInRange<size_t>(0, list.size() - 1)];
    font = Respell(entry.name, p);
    CHECK(FindInFontList(platform, FontName::Utf8(font),
                         FontName::Utf8(font).Hash()) != nullptr)
        << "a listed name, respelled, isn't found";
  }
  CheckFontName(farbler, platform, font, p);
  return 0;
}
