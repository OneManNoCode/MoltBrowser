// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/molt_fingerprinting.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "base/containers/span.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/shields_decision.h"
#include "third_party/blink/renderer/core/testing/dummy_page_holder.h"
#include "third_party/blink/renderer/platform/graphics/test/fake_gles2_interface.h"
#include "third_party/blink/renderer/platform/graphics/unaccelerated_static_bitmap_image.h"
#include "third_party/blink/renderer/platform/storage/blink_storage_key.h"
#include "third_party/blink/renderer/platform/testing/task_environment.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "ui/gfx/skia_span_util.h"

namespace blink::molt_fingerprinting {
namespace {

// These tests never configure kUniform: that pins the process-wide time zone
// and locale, which would leak into other tests in blink_unittests.
class MoltFingerprintingTest : public testing::Test {
 protected:
  void SetUp() override { ResetForTesting(); }
  void TearDown() override { ResetForTesting(); }

  // A window whose top-level site is `url`'s site.
  LocalDOMWindow& Window(const char* url) {
    holders_.push_back(DummyPageHolder::CreateAndCommitNavigation(KURL(url)));
    LocalDOMWindow& window = *holders_.back()->GetFrame().DomWindow();
    window.SetStorageKey(
        BlinkStorageKey::CreateFirstParty(SecurityOrigin::Create(KURL(url))));
    return window;
  }

  test::TaskEnvironment task_environment_;
  std::vector<std::unique_ptr<DummyPageHolder>> holders_;
};

std::array<uint8_t, kSessionSeedSize> TestSeed() {
  std::array<uint8_t, kSessionSeedSize> seed;
  for (size_t i = 0; i < seed.size(); ++i) {
    seed[i] = static_cast<uint8_t>(5 + 11 * i);
  }
  return seed;
}

SiteKey TestKey() {
  SiteKey key{};
  for (size_t i = 0; i < key.size(); ++i) {
    key[i] = static_cast<uint8_t>(3 + 17 * i);
  }
  return key;
}

// Two farblers are the same iff their PRF agrees on a few inputs.
bool Same(const Farbler& a, const Farbler& b) {
  for (uint64_t i = 0; i < 4; ++i) {
    if (a.Prf(i, i * 77) != b.Prf(i, i * 77)) {
      return false;
    }
  }
  return true;
}

SkBitmap BusyBitmap(SkColorType type, SkAlphaType alpha, size_t row_bytes) {
  SkBitmap bitmap;
  const SkImageInfo info = SkImageInfo::Make(48, 16, type, alpha);
  EXPECT_TRUE(bitmap.tryAllocPixels(info, row_bytes));
  base::span<uint8_t> bytes = gfx::SkPixmapToWritableSpan(bitmap.pixmap());
  uint32_t state = 12345;
  for (uint8_t& b : bytes) {
    state = state * 1664525u + 1013904223u;
    b = static_cast<uint8_t>(state >> 24);
  }
  return bitmap;
}

std::vector<uint8_t> Bytes(const SkBitmap& bitmap) {
  const base::span<const uint8_t> span = gfx::SkPixmapToSpan(bitmap.pixmap());
  return std::vector<uint8_t>(span.begin(), span.end());
}

TEST_F(MoltFingerprintingTest, OffUntilConfigured) {
  EXPECT_EQ(GetMode(), Mode::kOff);
  EXPECT_FALSE(IsUniform());
  EXPECT_FALSE(FarblerFor(nullptr).has_value());
  EXPECT_EQ(HardwareConcurrency(nullptr, 8), 8u);
  EXPECT_FALSE(UniformScreenRect(nullptr).has_value());
  EXPECT_FALSE(UniformScreenPoint(nullptr, gfx::PointF(10, 20)).has_value());
  // A seed alone turns nothing on.
  SetSessionSeed(TestSeed());
  EXPECT_FALSE(IsFarbled(&Window("https://a.example/")));
}

TEST_F(MoltFingerprintingTest, OnlyTheFirstConfigureCounts) {
  Configure(Mode::kFarble);
  EXPECT_EQ(GetMode(), Mode::kFarble);
  Configure(Mode::kOff);
  EXPECT_EQ(GetMode(), Mode::kFarble);
  // Without an execution context there is no site, so nothing is farbled.
  EXPECT_FALSE(FarblerFor(nullptr).has_value());
  EXPECT_EQ(HardwareConcurrency(nullptr, 8), 8u);
  EXPECT_FALSE(UniformScreenRect(nullptr).has_value());
}

TEST_F(MoltFingerprintingTest, WindowsUseTheirTopLevelSitesKey) {
  Configure(Mode::kFarble);
  SetSessionSeed(TestSeed());
  LocalDOMWindow& a = Window("https://www.a.example/page");
  const std::optional<Farbler> farbler = FarblerFor(&a);
  ASSERT_TRUE(farbler.has_value());
  EXPECT_TRUE(Same(
      *farbler, Farbler(DeriveSiteKey(TestSeed(), "https://a.example", ""))));
  EXPECT_FALSE(Same(*farbler, *FarblerFor(&Window("https://b.example/"))));
}

TEST_F(MoltFingerprintingTest, ShieldsOffDocumentsAreNotFarbled) {
  Configure(Mode::kFarble);
  SetSessionSeed(TestSeed());
  LocalDOMWindow& a = Window("https://a.example/");
  LocalDOMWindow& b = Window("https://b.example/");
  // No decision: farbled.
  EXPECT_TRUE(IsFarbled(&a));
  // The browser's decision for a.example's document (shields_decision.h).
  SetWindowShieldsDecision(a, /*shields_down=*/true, "https://a.example");
  EXPECT_FALSE(IsFarbled(&a));
  EXPECT_FALSE(FarblerFor(&a).has_value());
  EXPECT_EQ(HardwareConcurrency(&a, 12), 12u);
  // Other documents are unaffected.
  EXPECT_TRUE(IsFarbled(&b));
  // A later decision for the same document replaces it.
  SetWindowShieldsDecision(a, /*shields_down=*/false, "https://a.example");
  EXPECT_TRUE(IsFarbled(&a));
}

TEST_F(MoltFingerprintingTest, KeysNeededBeforeTheSeedStayRandomForLife) {
  Configure(Mode::kFarble);
  LocalDOMWindow& early = Window("https://a.example/");
  const std::optional<Farbler> first = FarblerFor(&early);
  ASSERT_TRUE(first.has_value());  // Fail closed.
  const Farbler derived(DeriveSiteKey(TestSeed(), "https://a.example", ""));
  EXPECT_FALSE(Same(*first, derived));
  SetSessionSeed(TestSeed());
  EXPECT_TRUE(Same(*FarblerFor(&early), *first));
  EXPECT_TRUE(Same(*FarblerFor(&Window("https://a.example/")), derived));
}

TEST_F(MoltFingerprintingTest, MalformedSeedsAreIgnored) {
  Configure(Mode::kFarble);
  const std::array<uint8_t, 16> short_seed = {};
  SetSessionSeed(short_seed);
  const std::optional<Farbler> farbler =
      FarblerFor(&Window("https://a.example/"));
  ASSERT_TRUE(farbler.has_value());
  EXPECT_FALSE(Same(
      *farbler, Farbler(DeriveSiteKey(TestSeed(), "https://a.example", ""))));
}

TEST_F(MoltFingerprintingTest, PixmapFarblingMatchesFarblerInBothOrders) {
  const Farbler farbler(TestKey());
  for (SkColorType type : {kRGBA_8888_SkColorType, kBGRA_8888_SkColorType}) {
    SkBitmap bitmap = BusyBitmap(type, kUnpremul_SkAlphaType, 0);
    std::vector<uint8_t> expected = Bytes(bitmap);
    farbler.FarbleRGBA8(expected, type == kRGBA_8888_SkColorType
                                      ? ChannelOrder::kRGBA
                                      : ChannelOrder::kBGRA);
    FarblePixmapWith(farbler, bitmap.pixmap());
    EXPECT_EQ(Bytes(bitmap), expected);
  }
}

TEST_F(MoltFingerprintingTest, PixmapRowPaddingIsNeverTouched) {
  const Farbler farbler(TestKey());
  constexpr size_t kRowBytes = 48 * 4 + 12;
  SkBitmap bitmap =
      BusyBitmap(kRGBA_8888_SkColorType, kUnpremul_SkAlphaType, kRowBytes);
  const std::vector<uint8_t> before = Bytes(bitmap);
  FarblePixmapWith(farbler, bitmap.pixmap());
  const std::vector<uint8_t> after = Bytes(bitmap);
  ASSERT_EQ(before.size(), after.size());
  for (size_t i = 0; i < before.size(); ++i) {
    if (i % kRowBytes >= 48 * 4) {
      EXPECT_EQ(before[i], after[i]) << "padding byte " << i;
    }
  }
  EXPECT_NE(before, after);
}

TEST_F(MoltFingerprintingTest, PremultipliedPixmapsAreLeftAlone) {
  const Farbler farbler(TestKey());
  SkBitmap bitmap = BusyBitmap(kRGBA_8888_SkColorType, kPremul_SkAlphaType, 0);
  const std::vector<uint8_t> before = Bytes(bitmap);
  FarblePixmapWith(farbler, bitmap.pixmap());
  EXPECT_EQ(Bytes(bitmap), before);
}

TEST_F(MoltFingerprintingTest, UnsupportedFormatsAreLeftAlone) {
  const Farbler farbler(TestKey());
  SkBitmap bitmap = BusyBitmap(kRGB_565_SkColorType, kOpaque_SkAlphaType, 0);
  const std::vector<uint8_t> before = Bytes(bitmap);
  FarblePixmapWith(farbler, bitmap.pixmap());
  EXPECT_EQ(Bytes(bitmap), before);
}

TEST_F(MoltFingerprintingTest, ExportCopiesStayPremultiplied) {
  const Farbler farbler(TestKey());
  // A valid premultiplied image: every colour channel <= alpha.
  SkBitmap bitmap;
  ASSERT_TRUE(bitmap.tryAllocPixels(
      SkImageInfo::Make(64, 16, kN32_SkColorType, kPremul_SkAlphaType)));
  base::span<uint8_t> bytes = gfx::SkPixmapToWritableSpan(bitmap.pixmap());
  uint32_t state = 777;
  for (size_t p = 0; p < bytes.size(); p += 4) {
    state = state * 1664525u + 1013904223u;
    const uint8_t alpha =
        (p % 32 == 0) ? 255 : static_cast<uint8_t>(state >> 24);
    bytes[p + 3] = alpha;
    for (size_t c = 0; c < 3; ++c) {
      state = state * 1664525u + 1013904223u;
      bytes[p + c] = static_cast<uint8_t>((state >> 16) % (alpha + 1u));
    }
  }
  const std::vector<uint8_t> original = Bytes(bitmap);
  bitmap.setImmutable();
  scoped_refptr<StaticBitmapImage> image =
      UnacceleratedStaticBitmapImage::Create(bitmap.asImage());
  scoped_refptr<StaticBitmapImage> out = FarbleForExportWith(farbler, *image);
  ASSERT_TRUE(out);
  const PaintImage paint_image = out->PaintImageForCurrentFrame();
  const SkImageInfo info = paint_image.GetSkImageInfo();
  EXPECT_EQ(info.alphaType(), kPremul_SkAlphaType);
  ASSERT_EQ(info.colorType(), kN32_SkColorType);
  std::vector<uint8_t> farbled(original.size());
  ASSERT_TRUE(
      paint_image.readPixels(info, farbled.data(), info.minRowBytes(), 0, 0));
  std::vector<uint8_t> expected = original;
  farbler.FarbleRGBA8Premul(expected,
                            info.colorType() == kRGBA_8888_SkColorType
                                ? ChannelOrder::kRGBA
                                : ChannelOrder::kBGRA);
  EXPECT_EQ(farbled, expected);
  EXPECT_NE(farbled, original);
  for (size_t p = 0; p < farbled.size(); p += 4) {
    for (size_t c = 0; c < 3; ++c) {
      EXPECT_LE(farbled[p + c], farbled[p + 3]);
    }
  }
}

TEST_F(MoltFingerprintingTest, ReadPixelsFarblesNarrowAndFloatFormats) {
  const Farbler farbler(TestKey());
  WebGLImageConversion::PixelStoreParams pack;
  pack.alignment = 8;
  pack.row_length = 21;
  pack.skip_pixels = 3;
  pack.skip_rows = 2;
  constexpr int kWidth = 17;
  constexpr int kHeight = 9;
  struct Case {
    GLenum format;
    GLenum type;
    size_t channels;
    size_t component_size;
  } cases[] = {
      {GL_RED, GL_UNSIGNED_BYTE, 1, 1}, {GL_RG, GL_UNSIGNED_BYTE, 2, 1},
      {GL_RGB, GL_UNSIGNED_BYTE, 3, 1}, {GL_RGBA, GL_UNSIGNED_BYTE, 4, 1},
      {GL_RGBA, GL_FLOAT, 4, 4},        {GL_RG, GL_FLOAT, 2, 4},
      {GL_RGBA, GL_HALF_FLOAT, 4, 2},   {GL_RGBA, GL_HALF_FLOAT_OES, 4, 2},
  };
  for (const Case& c : cases) {
    const size_t bpp = c.channels * c.component_size;
    const size_t stride = (21 * bpp + 7) / 8 * 8;
    const size_t first = 2 * stride + 3 * bpp;
    std::vector<uint8_t> buffer(first + stride * kHeight);
    for (size_t i = 0; i < buffer.size(); ++i) {
      buffer[i] = static_cast<uint8_t>((i * 97) ^ (i >> 5));
    }
    std::vector<uint8_t> expected = buffer;
    for (int y = 0; y < kHeight; ++y) {
      base::span<uint8_t> row =
          base::span(expected).subspan(first + y * stride, kWidth * bpp);
      if (c.component_size == 1) {
        farbler.FarbleUnorm8(row, c.channels);
      } else if (c.component_size == 2) {
        farbler.FarbleFloat16(row);
      } else {
        farbler.FarbleFloat32(row);
      }
    }
    FarbleReadPixelsWith(farbler, c.format, c.type, kWidth, kHeight, pack,
                         buffer);
    EXPECT_EQ(buffer, expected) << c.format << " / " << c.type;
  }
  // Integer and packed reads are left alone.
  std::vector<uint8_t> integer(4096, 77);
  FarbleReadPixelsWith(farbler, GL_RGBA_INTEGER, GL_UNSIGNED_INT, 8, 8, pack,
                       integer);
  FarbleReadPixelsWith(farbler, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 8, 8, pack,
                       integer);
  EXPECT_EQ(integer, std::vector<uint8_t>(4096, 77));
}

// A PIXEL_PACK buffer read back and written the way the command-buffer
// client does it. set_fail_reads(): reads as the client does when it gets no
// shared memory, leaving `data` untouched.
class PackBufferGLES2 : public FakeGLES2Interface {
 public:
  explicit PackBufferGLES2(std::vector<uint8_t> contents)
      : buffer_(std::move(contents)) {}

  void GetBufferSubDataCHROMIUM(GLenum target,
                                GLintptr offset,
                                GLsizeiptr size,
                                void* data) override {
    EXPECT_EQ(target, static_cast<GLenum>(GL_PIXEL_PACK_BUFFER));
    ++reads_;
    if (fail_reads_ || offset < 0 || size <= 0 ||
        static_cast<size_t>(offset + size) > buffer_.size()) {
      return;
    }
    std::copy_n(buffer_.begin() + offset, size, static_cast<uint8_t*>(data));
  }

  void BufferSubData(GLenum target,
                     GLintptr offset,
                     GLsizeiptr size,
                     const void* data) override {
    EXPECT_EQ(target, static_cast<GLenum>(GL_PIXEL_PACK_BUFFER));
    ++writes_;
    ASSERT_GE(offset, 0);
    ASSERT_LE(static_cast<size_t>(offset + size), buffer_.size());
    std::copy_n(static_cast<const uint8_t*>(data), size,
                buffer_.begin() + offset);
  }

  void set_fail_reads() { fail_reads_ = true; }
  const std::vector<uint8_t>& buffer() const { return buffer_; }
  int reads() const { return reads_; }
  int writes() const { return writes_; }

 private:
  std::vector<uint8_t> buffer_;
  bool fail_reads_ = false;
  int reads_ = 0;
  int writes_ = 0;
};

TEST_F(MoltFingerprintingTest, PixelPackBufferMatchesClientMemoryReads) {
  const Farbler farbler(TestKey());
  WebGLImageConversion::PixelStoreParams pack;
  pack.alignment = 4;
  pack.row_length = 40;
  pack.skip_rows = 1;
  constexpr int kWidth = 33;
  constexpr int kHeight = 12;
  constexpr int64_t kOffset = 64;
  std::vector<uint8_t> contents(kOffset + 40 * 4 * (kHeight + 1) + 16);
  for (size_t i = 0; i < contents.size(); ++i) {
    contents[i] = static_cast<uint8_t>((i * 131) ^ (i >> 3));
  }
  std::vector<uint8_t> expected = contents;
  FarbleReadPixelsWith(
      farbler, GL_RGBA, GL_UNSIGNED_BYTE, kWidth, kHeight, pack,
      base::span(expected).subspan(static_cast<size_t>(kOffset)));
  ASSERT_NE(expected, contents);
  PackBufferGLES2 gl(contents);
  FarblePixelPackBufferWith(farbler, &gl, GL_RGBA, GL_UNSIGNED_BYTE, kWidth,
                            kHeight, pack, kOffset);
  EXPECT_EQ(gl.buffer(), expected);
  EXPECT_EQ(gl.reads(), 1);
  EXPECT_EQ(gl.writes(), 1);
  // Formats that aren't farbled are never read back.
  PackBufferGLES2 untouched(contents);
  FarblePixelPackBufferWith(farbler, &untouched, GL_RGBA_INTEGER,
                            GL_UNSIGNED_INT, kWidth, kHeight, pack, kOffset);
  EXPECT_EQ(untouched.reads(), 0);
  EXPECT_EQ(untouched.buffer(), contents);
  // A read back that didn't happen writes nothing.
  PackBufferGLES2 failing(contents);
  failing.set_fail_reads();
  FarblePixelPackBufferWith(farbler, &failing, GL_RGBA, GL_UNSIGNED_BYTE,
                            kWidth, kHeight, pack, kOffset);
  EXPECT_EQ(failing.reads(), 2);
  EXPECT_EQ(failing.writes(), 0);
  EXPECT_EQ(failing.buffer(), contents);
  // Rows that really are all zeros are farbled like any others.
  const std::vector<uint8_t> zeros(contents.size(), 0);
  std::vector<uint8_t> expected_zeros = zeros;
  FarbleReadPixelsWith(
      farbler, GL_RGBA, GL_UNSIGNED_BYTE, kWidth, kHeight, pack,
      base::span(expected_zeros).subspan(static_cast<size_t>(kOffset)));
  PackBufferGLES2 black(zeros);
  FarblePixelPackBufferWith(farbler, &black, GL_RGBA, GL_UNSIGNED_BYTE, kWidth,
                            kHeight, pack, kOffset);
  EXPECT_EQ(black.reads(), 2);
  EXPECT_EQ(black.buffer(), expected_zeros);
}

}  // namespace
}  // namespace blink::molt_fingerprinting
