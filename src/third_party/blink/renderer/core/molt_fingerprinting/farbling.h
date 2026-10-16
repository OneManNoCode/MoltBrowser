// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: the pure math.
//
// Everything here is deterministic integer/float arithmetic over plain
// buffers. It depends only on //base (span) and //crypto (HMAC-SHA256), never
// on the rest of Blink, so it is unit-tested standalone (farbling_unittest.cc,
// also linked into molt_ai_unittests). The Blink glue that decides *when* to
// farble and finds the key for an execution context is molt_fingerprinting.h.
//
// Design (docs/FINGERPRINTING.md has the threat model and the known gaps):
//
//  * A per-profile session seed (32 random bytes, generated in the browser,
//    never persisted) reaches the renderer once at process start.
//  * Each execution context farbles with a per-site key:
//        key = HMAC-SHA256(seed, "molt-fp-site-v1" 0x00 site partition)
//    (site and partition each length-prefixed, see DeriveSiteKey()),
//    where `site` is the schemeful registrable domain (eTLD+1) of the
//    context's top-level site, e.g. "https://example.com". Same site + same
//    session => same key (pages stay consistent); different site or new
//    session => unrelated key (fingerprints don't link).
//  * All noise is a pure function of (key, value). It does not depend on the
//    position of a pixel/sample, the size of the read, or the rest of the
//    buffer, so re-reading, reading a sub-rectangle or drawing the same thing
//    twice always gives the same answer. Canvas and audio noise are also
//    idempotent (farbling already-farbled data changes nothing), so
//    read-modify-write loops (getImageData -> putImageData -> getImageData)
//    never drift.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_FARBLING_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_FARBLING_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "base/containers/span.h"

namespace blink::molt_fingerprinting {

inline constexpr size_t kSessionSeedSize = 32;
inline constexpr size_t kSiteKeySize = 32;
using SiteKey = std::array<uint8_t, kSiteKeySize>;

// HMAC-SHA256(session_seed,
//             "molt-fp-site-v1" 0x00 be32(|site|) site be32(|partition|)
//             partition).
// `partition` is empty for ordinary contexts; it carries e.g. the storage-key
// nonce of a fenced frame or credentialless iframe so such a frame gets a key
// of its own.
SiteKey DeriveSiteKey(base::span<const uint8_t> session_seed,
                      std::string_view site,
                      std::string_view partition);

// Byte order of 8-bit RGBA pixels in a buffer. Decisions are always made on
// the canonical (R, G, B, A) tuple so a BGRA snapshot and an RGBA ImageData of
// the same canvas farble identically.
enum class ChannelOrder { kRGBA, kBGRA };

// Canvas / audio noise for one site key. Cheap to copy.
class Farbler {
 public:
  explicit Farbler(const SiteKey& key);

  // Keyed 64-bit PRF (two splitmix64-finalizer rounds over the key words).
  // Not a cryptographic MAC, and it doesn't need to be: the per-site key is
  // already an HMAC output, so learning it says nothing about other sites'
  // keys, and a page can learn its own site's noise by probing anyway (see
  // docs/FINGERPRINTING.md). It is cheap because it runs per distinct pixel.
  uint64_t Prf(uint64_t domain, uint64_t input) const;

  // --- Canvas / WebGL readback --------------------------------------------
  //
  // Sparse, idempotent, value-keyed noise on unpremultiplied pixels:
  //  * fully transparent pixels are never touched (blank-canvas checks, e.g.
  //    signature pads, keep working);
  //  * for each pixel one colour channel is picked from the alpha value; its
  //    256 values are split into pairs whose alignment ({2k,2k+1} or
  //    {2k+1,2k+2}) depends on the key and the other channels; one pair in
  //    eight is collapsed onto one of its two values. So about 1 colour in 16
  //    moves by exactly 1, the rest are untouched, and a value that was moved
  //    is never moved again.

  // Farbles one canonical RGBA pixel (exposed for tests).
  std::array<uint8_t, 4> FarblePixel(const std::array<uint8_t, 4>& rgba) const;
  // 4 bytes per pixel, 8 bits per channel. size() must be a multiple of 4
  // (a trailing partial pixel is ignored).
  void FarbleRGBA8(base::span<uint8_t> pixels, ChannelOrder order) const;
  // Same decisions on *premultiplied* RGBA8 (the tuple as stored), except
  // that a channel is never moved above its alpha, so the result is still
  // valid premultiplied data. Idempotent. Used where canvas pixels leave as
  // premultiplied images (VideoFrame, captureStream, WebGPU uploads).
  void FarbleRGBA8Premul(base::span<uint8_t> pixels, ChannelOrder order) const;
  // 8-bit normalized pixels with `channels` (1-4) components each, as WebGL
  // readPixels packs RED, RG, RGB and RGBA data. Missing components take
  // their GL defaults for the decision (G = B = 0, A = 255), so a RED read
  // returns exactly the R channel an RGBA read of the same data returns.
  // A trailing partial pixel is ignored; other channel counts do nothing.
  void FarbleUnorm8(base::span<uint8_t> pixels, size_t channels) const;
  // 8 bytes per pixel: R, G, B, A as little-endian IEEE half floats.
  void FarbleRGBAF16(base::span<uint8_t> pixels) const;
  // 16 bytes per pixel: R, G, B, A as little-endian IEEE single floats.
  void FarbleRGBAF32(base::span<uint8_t> pixels) const;

  // --- Float readback (WebGL FLOAT / HALF_FLOAT readPixels) ----------------
  //
  // Values that came out of float framebuffers are not colours on an 8-bit
  // grid (GPGPU results, HDR), so the snapping above would be far too
  // coarse. Instead each value gets noise at its own precision:
  //  * float32: FarbleSample() (the audio noise: low kAudioNoiseBits of the
  //    mantissa, relative change < 2^-15);
  //  * float16: FarbleHalf(), the pair collapse on the 15-bit magnitude
  //    (about 1 value in 16 moves by exactly one ulp; zero, infinities and
  //    NaN never move, nothing moves to zero or infinity).
  // Both are idempotent and depend only on (key, value).
  // Little-endian float32 values; a trailing partial value is ignored.
  void FarbleFloat32(base::span<uint8_t> bytes) const;
  // Little-endian float16 values; a trailing partial value is ignored.
  void FarbleFloat16(base::span<uint8_t> bytes) const;
  uint16_t FarbleHalf(uint16_t half) const;

  // --- Web Audio -----------------------------------------------------------
  //
  // Replaces the low kAudioNoiseBits of a sample's 23-bit mantissa with keyed
  // bits derived from the rest of the sample: relative noise below 2^-15
  // (about -90 dB, inaudible), much larger than the 1-2 ulp differences
  // between machines that audio fingerprints rely on. Zero, subnormal,
  // infinite and NaN samples are left alone (silence stays silent). The
  // mapping is idempotent.
  float FarbleSample(float sample) const;
  void FarbleAudio(base::span<float> samples) const;
  // AnalyserNode byte data: the pair-collapse above on byte values, never
  // touching `neutral` (0 for frequency data, 128 = silence for time-domain
  // data).
  void FarbleAudioBytes(base::span<uint8_t> bytes, uint8_t neutral) const;

  // --- navigator.hardwareConcurrency --------------------------------------
  // A per-site value in [2, real] (real when real <= 2).
  unsigned HardwareConcurrency(unsigned real) const;

 private:
  // The pair collapse on values in [0, max]: pairs {2k + a, 2k + 1 + a}, the
  // alignment `a` from the key and `context`, one pair in kCollapseOneIn
  // collapsed onto one of its two values.
  uint32_t FarbleValue(uint32_t value, uint32_t max, uint64_t context) const;
  uint8_t FarbleByte(uint8_t value, uint64_t context) const;
  size_t ChannelFor(uint8_t alpha) const;
  std::array<uint8_t, 4> FarblePixelInChannel(
      const std::array<uint8_t, 4>& rgba,
      size_t channel) const;
  void FarbleRGBA8Impl(base::span<uint8_t> pixels,
                       ChannelOrder order,
                       bool premultiplied) const;

  std::array<uint64_t, 4> k_;
};

inline constexpr int kAudioNoiseBits = 8;

// --- MoltNet uniformity (no key) ---------------------------------------------

// Tor-Browser-style letterbox steps for the reported screen size.
inline constexpr int kLetterboxStepWidth = 200;
inline constexpr int kLetterboxStepHeight = 100;
inline constexpr int kLetterboxMaxWidth = 1400;
inline constexpr int kLetterboxMaxHeight = 900;

struct LetterboxSize {
  int width;
  int height;
};

// Viewport size (DIPs) rounded down to the letterbox steps, capped at
// 1400x900 and never below one step.
LetterboxSize LetterboxBucket(int viewport_width, int viewport_height);

// Every MoltNet window reports these.
inline constexpr unsigned kUniformHardwareConcurrency = 4;
inline constexpr unsigned kUniformColorDepth = 24;
inline constexpr char kUniformTimeZone[] = "Etc/UTC";
inline constexpr char kUniformLocale[] = "en-US";

// IEEE 754 binary16 <-> binary32 (round to nearest even). Exposed for tests.
float HalfToFloat(uint16_t half);
uint16_t FloatToHalf(float value);

}  // namespace blink::molt_fingerprinting

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_FARBLING_H_
