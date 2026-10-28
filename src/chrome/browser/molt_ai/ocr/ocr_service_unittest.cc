// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/ocr/ocr_service.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/location.h"
#include "base/memory/raw_ptr.h"
#include "base/strings/string_util.h"
#include "base/strings/string_view_util.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/simple_test_tick_clock.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "base/time/time.h"
#include "chrome/browser/molt_ai/document_parser/document_parser_client.h"
#include "chrome/browser/molt_ai/ocr/ocr_image.h"
#include "chrome/services/molt_ai/document_parser/public/mojom/document_parser.mojom.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "services/data_decoder/public/cpp/decode_image.h"
#include "services/data_decoder/public/cpp/service_provider.h"
#include "services/data_decoder/public/mojom/data_decoder_service.mojom.h"
#include "services/data_decoder/public/mojom/image_decoder.mojom.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/skia/include/core/SkRect.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/geometry/size.h"

namespace molt_ai {
namespace ocr {
namespace {

using internal::ImageOcrFunction;
using internal::OcrRenderedPages;

// A rendered page whose "PNG" is just |marker| (the fake OCR reads it back).
RenderedPdfPage FakePage(uint32_t index, std::string_view marker) {
  RenderedPdfPage page;
  page.page_index = index;
  page.width = 10;
  page.height = 10;
  page.png.assign(marker.begin(), marker.end());
  return page;
}

std::vector<RenderedPdfPage> FakePages(
    const std::vector<std::pair<uint32_t, std::string>>& pages) {
  std::vector<RenderedPdfPage> out;
  for (const auto& [index, marker] : pages) {
    out.push_back(FakePage(index, marker));
  }
  return out;
}

OcrResult Text(std::string text) {
  OcrResult result;
  result.success = true;
  result.text = std::move(text);
  return result;
}

OcrResult Error(std::string error) {
  OcrResult result;
  result.error = std::move(error);
  return result;
}

// Stands in for decoding and tesseract: the page's text is its marker,
// except "fail", which fails. Records each call's timeout, advances |clock|
// by |seconds_per_page|, and answers at once or (|answer_later|) from a
// task of its own.
class FakeOcr {
 public:
  explicit FakeOcr(base::SimpleTestTickClock* clock = nullptr,
                   int seconds_per_page = 0,
                   bool answer_later = false)
      : clock_(clock),
        seconds_per_page_(seconds_per_page),
        answer_later_(answer_later) {}

  ImageOcrFunction Function() {
    return base::BindRepeating(&FakeOcr::Run, base::Unretained(this));
  }

  const std::vector<base::TimeDelta>& timeouts() const { return timeouts_; }

 private:
  void Run(base::span<const uint8_t> png,
           size_t max_chars,
           base::TimeDelta timeout,
           base::OnceCallback<void(OcrResult)> done) {
    timeouts_.push_back(timeout);
    if (clock_) {
      clock_->Advance(base::Seconds(seconds_per_page_));
    }
    const std::string marker(base::as_string_view(png));
    OcrResult result = marker == "fail" ? Error("tesseract exit=1: bad page")
                                        : Text(marker.substr(0, max_chars));
    if (answer_later_) {
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, base::BindOnce(std::move(done), std::move(result)));
      return;
    }
    std::move(done).Run(std::move(result));
  }

  raw_ptr<base::SimpleTestTickClock> clock_;
  const int seconds_per_page_;
  const bool answer_later_;
  std::vector<base::TimeDelta> timeouts_;
};

class OcrRenderedPagesTest : public testing::Test {
 protected:
  OcrResult Run(std::vector<RenderedPdfPage> pages,
                uint32_t page_count,
                size_t max_chars,
                FakeOcr& ocr) {
    return RunUntil(std::move(pages), page_count, max_chars,
                    clock_.NowTicks() + kPdfOcrTimeout, ocr);
  }

  OcrResult RunUntil(std::vector<RenderedPdfPage> pages,
                     uint32_t page_count,
                     size_t max_chars,
                     base::TimeTicks deadline,
                     FakeOcr& ocr) {
    base::test::TestFuture<OcrResult> future;
    OcrRenderedPages(std::move(pages), page_count, max_chars, deadline, &clock_,
                     ocr.Function(), future.GetCallback());
    // Never synchronously.
    EXPECT_FALSE(future.IsReady());
    return future.Take();
  }

  base::test::TaskEnvironment task_environment_;
  base::SimpleTestTickClock clock_;
};

TEST_F(OcrRenderedPagesTest, JoinsPagesWithHeaders) {
  for (bool answer_later : {false, true}) {
    FakeOcr ocr(nullptr, 0, answer_later);
    // tesseract ends a page with a form feed; page 2 has no text.
    OcrResult result =
        Run(FakePages({{0, "Hello\f"}, {1, " \n\f"}, {2, "World\n"}}), 3, 1000,
            ocr);
    ASSERT_TRUE(result.success) << result.error;
    EXPECT_EQ(result.text, "--- Page 1 ---\nHello\n\n--- Page 3 ---\nWorld");
    EXPECT_TRUE(result.error.empty());
  }
}

TEST_F(OcrRenderedPagesTest, SaysWhenPagesWereLeftOut) {
  FakeOcr ocr;
  // Pages 1 and 3 of a 57-page document (page 2 couldn't be rendered).
  OcrResult result = Run(FakePages({{0, "one"}, {2, "three"}}), 57, 1000, ocr);
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.text,
            "--- Page 1 ---\none\n\n--- Page 3 ---\nthree\n\n"
            "--- OCR read 2 of 57 pages ---");
}

TEST_F(OcrRenderedPagesTest, StopsAtMaxChars) {
  FakeOcr ocr;
  const std::string page(100, 'a');
  OcrResult result =
      Run(FakePages({{0, page}, {1, page}, {2, page}, {3, page}}), 4, 150, ocr);
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.text.size(), 150u);
  EXPECT_TRUE(base::StartsWith(result.text, "--- Page 1 ---\naaa"));
  // Pages after the budget was used up weren't OCR'd.
  EXPECT_EQ(ocr.timeouts().size(), 2u);
}

TEST_F(OcrRenderedPagesTest, TheCutKeepsUtf8Valid) {
  FakeOcr ocr;
  // "--- Page 1 ---\n" (15 bytes), 20 x's, then two 2-byte "é"s: a cut at 36
  // bytes falls inside the first "é", which must go entirely.
  OcrResult result = Run(
      FakePages({{0, std::string(20, 'x') + "\xC3\xA9\xC3\xA9"}}), 1, 36, ocr);
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.text, "--- Page 1 ---\n" + std::string(20, 'x'));
  EXPECT_TRUE(base::IsStringUTF8(result.text));
}

TEST_F(OcrRenderedPagesTest, FailedPagesAreSkipped) {
  FakeOcr ocr;
  OcrResult result =
      Run(FakePages({{0, "fail"}, {1, "two"}, {2, "fail"}}), 3, 1000, ocr);
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.text, "--- Page 2 ---\ntwo");

  // No page gave text: the last page's error.
  FakeOcr failing;
  result = Run(FakePages({{0, "fail"}, {1, "  "}}), 2, 1000, failing);
  EXPECT_FALSE(result.success);
  EXPECT_TRUE(result.text.empty());
  EXPECT_EQ(result.error, "tesseract exit=1: bad page");

  FakeOcr blank;
  result = Run(FakePages({{0, " "}}), 1, 1000, blank);
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error, "OCR returned no text");
}

TEST_F(OcrRenderedPagesTest, OneDeadlineForAllPages) {
  // Each page takes 70 s of the 180 s: the first two get the full
  // per-page limit, the third what is left, and the fourth isn't started.
  FakeOcr ocr(&clock_, /*seconds_per_page=*/70);
  OcrResult result =
      Run(FakePages({{0, "one"}, {1, "two"}, {2, "three"}, {3, "four"}}), 4,
          1000, ocr);
  ASSERT_TRUE(result.success);
  ASSERT_EQ(ocr.timeouts().size(), 3u);
  EXPECT_EQ(ocr.timeouts()[0], kImageOcrTimeout);
  EXPECT_EQ(ocr.timeouts()[1], kImageOcrTimeout);
  EXPECT_EQ(ocr.timeouts()[2], kPdfOcrTimeout - base::Seconds(140));
  EXPECT_TRUE(base::EndsWith(result.text, "--- OCR read 3 of 4 pages ---"));
  EXPECT_EQ(result.text.find("four"), std::string::npos);
}

TEST_F(OcrRenderedPagesTest, NoTimeLeft) {
  FakeOcr ocr;
  OcrResult result =
      RunUntil(FakePages({{0, "one"}}), 1, 1000,
               clock_.NowTicks() + kMinPageOcrTime - base::Seconds(1), ocr);
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error, "OCR ran out of time");
  EXPECT_TRUE(ocr.timeouts().empty());
}

// The whole of OcrPdf but the singleton, the decoder and tesseract: a fake
// document parser renders, a fake OCR reads the "PNGs".
class RenderAndOcrPdfTest : public testing::Test,
                            public mojom::DocumentParser {
 protected:
  RenderAndOcrPdfTest()
      : parser_(base::BindRepeating(&RenderAndOcrPdfTest::Launch,
                                    base::Unretained(this)),
                DocumentParserClient::kTimeout) {}

  ~RenderAndOcrPdfTest() override { receivers_.Clear(); }

  mojo::PendingRemote<mojom::DocumentParser> Launch(
      DocumentParserClient::ProcessCallback) {
    mojo::PendingRemote<mojom::DocumentParser> remote;
    receivers_.Add(this, remote.InitWithNewPipeAndPassReceiver());
    return remote;
  }

  // mojom::DocumentParser: only RenderPdfPages is used.
  void ExtractPdfText(mojo_base::BigBuffer,
                      uint32_t,
                      ExtractPdfTextCallback callback) override {
    std::move(callback).Run(mojom::ParseResult::New());
  }
  void ConvertToMarkdown(mojo_base::BigBuffer,
                         const std::string&,
                         uint32_t,
                         ConvertToMarkdownCallback callback) override {
    std::move(callback).Run(mojom::ParseResult::New());
  }
  void ExtractDocxText(mojo_base::BigBuffer,
                       uint32_t,
                       ExtractDocxTextCallback callback) override {
    std::move(callback).Run(mojom::ParseResult::New());
  }
  void RenderPdfPages(mojo_base::BigBuffer pdf,
                      uint32_t max_pages,
                      uint32_t dpi,
                      RenderPdfPagesCallback callback) override {
    last_document_ =
        std::string(base::as_string_view(base::span<const uint8_t>(pdf)));
    last_max_pages_ = max_pages;
    last_dpi_ = dpi;
    if (crash_) {
      receivers_.Clear();
      return;
    }
    std::move(callback).Run(std::move(reply_));
  }

  // A page whose PNG's IDAT holds |marker|, so the fake OCR can tell pages
  // apart. Well formed: the client checks it.
  static std::string Png(std::string_view marker) {
    const auto be32 = [](std::string& out, uint32_t value) {
      for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xff));
      }
    };
    const auto chunk = [&be32](std::string_view type, std::string_view data) {
      std::string out;
      be32(out, static_cast<uint32_t>(data.size()));
      out += type;
      out += data;
      be32(out, 0);  // CRC: not checked by the browser.
      return out;
    };
    std::string ihdr;
    be32(ihdr, 10);
    be32(ihdr, 10);
    ihdr += std::string("\x08\x02\x00\x00\x00", 5);
    return std::string("\x89PNG\r\n\x1a\n", 8) + chunk("IHDR", ihdr) +
           chunk("IDAT", marker) + chunk("IEND", "");
  }

  // The fake OCR: the IDAT data of the PNG (see Png()), or a failure.
  static void ReadMarker(base::span<const uint8_t> png,
                         size_t max_chars,
                         base::TimeDelta timeout,
                         base::OnceCallback<void(OcrResult)> done) {
    const std::string bytes(base::as_string_view(png));
    // Signature (8) + IHDR chunk (25) + IDAT length and type (8).
    const size_t start = 8 + 25 + 8;
    const size_t end = bytes.size() - 12 - 4;  // IEND, IDAT's CRC.
    if (bytes.size() < start + 16 || end < start) {
      std::move(done).Run(Error("not one of ours"));
      return;
    }
    std::move(done).Run(Text(bytes.substr(start, end - start)));
  }

  void SetPages(std::vector<std::string> markers, uint32_t page_count) {
    reply_ = mojom::PdfPageImages::New();
    reply_->success = true;
    reply_->page_count = page_count;
    for (size_t i = 0; i < markers.size(); ++i) {
      mojom::PdfPageImagePtr page = mojom::PdfPageImage::New();
      page->page_index = static_cast<uint32_t>(i);
      page->width = 10;
      page->height = 10;
      const std::string png = Png(markers[i]);
      page->png = mojo_base::BigBuffer(base::as_byte_span(png));
      reply_->pages.push_back(std::move(page));
    }
  }

  OcrResult OcrPdf(const std::string& pdf) {
    base::test::TestFuture<OcrResult> future;
    internal::RenderAndOcrPdf(parser_, base::as_byte_span(pdf), 1000,
                              base::TimeTicks::Now() + kPdfOcrTimeout,
                              base::BindRepeating(&ReadMarker),
                              future.GetCallback());
    return future.Take();
  }

  base::test::TaskEnvironment task_environment_;
  mojo::ReceiverSet<mojom::DocumentParser> receivers_;
  DocumentParserClient parser_;
  mojom::PdfPageImagesPtr reply_;
  bool crash_ = false;
  std::string last_document_;
  uint32_t last_max_pages_ = 0;
  uint32_t last_dpi_ = 0;
};

TEST_F(RenderAndOcrPdfTest, RendersThenOcrsEachPage) {
  SetPages({"first page", "second page"}, 2);
  OcrResult result = OcrPdf("%PDF-1.7 scan");
  ASSERT_TRUE(result.success) << result.error;
  EXPECT_EQ(result.text,
            "--- Page 1 ---\nfirst page\n\n--- Page 2 ---\nsecond page");
  EXPECT_EQ(last_document_, "%PDF-1.7 scan");
  EXPECT_EQ(last_max_pages_, kPdfOcrMaxPages);
  EXPECT_EQ(last_dpi_, kPdfOcrDpi);
}

TEST_F(RenderAndOcrPdfTest, AnUnreadablePdfFailsWithTheParsersReason) {
  reply_ = mojom::PdfPageImages::New();
  reply_->success = false;
  reply_->error = "the PDF is password-protected";
  OcrResult result = OcrPdf("%PDF-1.7 locked");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error, "the PDF is password-protected");
}

TEST_F(RenderAndOcrPdfTest, ARenderCrashFails) {
  crash_ = true;
  OcrResult result = OcrPdf("%PDF-1.7 hostile");
  EXPECT_FALSE(result.success);
  EXPECT_TRUE(base::StartsWith(result.error, "rendering its pages failed"))
      << result.error;
}

TEST_F(RenderAndOcrPdfTest, AHostileReplyIsNeverOcrd) {
  // A "page" that isn't a PNG: the client rejects the whole reply, so
  // nothing reaches the decoder or tesseract.
  reply_ = mojom::PdfPageImages::New();
  reply_->success = true;
  reply_->page_count = 1;
  mojom::PdfPageImagePtr page = mojom::PdfPageImage::New();
  page->width = 10;
  page->height = 10;
  page->png = mojo_base::BigBuffer(base::as_byte_span(std::string("GIF89a")));
  reply_->pages.push_back(std::move(page));
  bool ocr_ran = false;
  base::test::TestFuture<OcrResult> future;
  internal::RenderAndOcrPdf(
      parser_, base::as_byte_span(std::string("%PDF-")), 1000,
      base::TimeTicks::Now() + kPdfOcrTimeout,
      base::BindRepeating(
          [](bool* ran, base::span<const uint8_t>, size_t, base::TimeDelta,
             base::OnceCallback<void(OcrResult)> done) {
            *ran = true;
            std::move(done).Run(Text("never"));
          },
          &ocr_ran),
      future.GetCallback());
  OcrResult result = future.Take();
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("malformed"), std::string::npos);
  EXPECT_FALSE(ocr_ran);
}

SkBitmap Solid(int width, int height, SkColor color) {
  SkBitmap bitmap;
  bitmap.allocN32Pixels(width, height);
  bitmap.eraseColor(color);
  return bitmap;
}

std::vector<uint8_t> PngOf(const SkBitmap& bitmap) {
  std::optional<std::vector<uint8_t>> png =
      gfx::PNGCodec::EncodeBGRASkBitmap(bitmap, /*discard_transparency=*/false);
  EXPECT_TRUE(png);
  return png.value_or(std::vector<uint8_t>());
}

// Stands in for the data decoder process. The browser side is real
// (DecodeImageForOcr and the data_decoder::DataDecoder it owns, over Mojo);
// this end decodes PNGs with gfx::PNGCodec, since the real service decodes
// with Blink, which this test binary doesn't start. It records what it was
// asked, and can hold its answers back or drop the connection unanswered,
// as a hung or crashed decoder process would.
class FakeDataDecoder : public data_decoder::ServiceProvider,
                        public data_decoder::mojom::DataDecoderService,
                        public data_decoder::mojom::ImageDecoder {
 public:
  FakeDataDecoder() { data_decoder::ServiceProvider::Set(this); }
  FakeDataDecoder(const FakeDataDecoder&) = delete;
  FakeDataDecoder& operator=(const FakeDataDecoder&) = delete;
  ~FakeDataDecoder() override {
    data_decoder::ServiceProvider::Set(nullptr);
    // The pipes close before any answer held back goes, as Mojo wants.
    decoders_.Clear();
    services_.Clear();
  }

  void set_hold_answers(bool hold) { hold_answers_ = hold; }
  void set_crash(bool crash) { crash_ = crash; }
  // Sends the answers held back.
  void AnswerHeld() {
    for (auto& [bitmap, callback] : held_) {
      std::move(callback).Run(bitmap);
    }
    held_.clear();
  }

  int decodes() const { return decodes_; }
  data_decoder::mojom::ImageCodec last_codec() const { return last_codec_; }
  bool last_shrink_to_fit() const { return last_shrink_to_fit_; }
  int64_t last_max_size() const { return last_max_size_; }
  const gfx::Size& last_frame_size() const { return last_frame_size_; }

 private:
  // data_decoder::ServiceProvider:
  void BindDataDecoderService(
      mojo::PendingReceiver<data_decoder::mojom::DataDecoderService> receiver)
      override {
    services_.Add(this, std::move(receiver));
  }

  // data_decoder::mojom::DataDecoderService: images only.
  void BindImageDecoder(mojo::PendingReceiver<data_decoder::mojom::ImageDecoder>
                            receiver) override {
    if (crash_) {
      return;  // Dropped: the client sees the pipe close.
    }
    decoders_.Add(this, std::move(receiver));
  }
  void BindXmlParser(
      mojo::PendingReceiver<data_decoder::mojom::XmlParser>) override {}
  void BindWebBundleParserFactory(
      mojo::PendingReceiver<web_package::mojom::WebBundleParserFactory>)
      override {}
  void BindGzipper(
      mojo::PendingReceiver<data_decoder::mojom::Gzipper>) override {}
  void BindStructuredHeadersParser(
      mojo::PendingReceiver<data_decoder::mojom::StructuredHeadersParser>)
      override {}

  // data_decoder::mojom::ImageDecoder:
  void DecodeImage(mojo_base::BigBuffer encoded_data,
                   data_decoder::mojom::ImageCodec codec,
                   bool shrink_to_fit,
                   int64_t max_size_in_bytes,
                   const gfx::Size& desired_image_frame_size,
                   DecodeImageCallback callback) override {
    ++decodes_;
    last_codec_ = codec;
    last_shrink_to_fit_ = shrink_to_fit;
    last_max_size_ = max_size_in_bytes;
    last_frame_size_ = desired_image_frame_size;
    SkBitmap bitmap =
        gfx::PNGCodec::Decode(base::span<const uint8_t>(encoded_data));
    if (hold_answers_) {
      held_.emplace_back(std::move(bitmap), std::move(callback));
      return;
    }
    std::move(callback).Run(bitmap);
  }
  void DecodeAnimation(mojo_base::BigBuffer,
                       bool,
                       int64_t,
                       DecodeAnimationCallback callback) override {
    std::move(callback).Run({});
  }

  mojo::ReceiverSet<data_decoder::mojom::DataDecoderService> services_;
  mojo::ReceiverSet<data_decoder::mojom::ImageDecoder> decoders_;
  bool hold_answers_ = false;
  bool crash_ = false;
  std::vector<std::pair<SkBitmap, DecodeImageCallback>> held_;
  int decodes_ = 0;
  data_decoder::mojom::ImageCodec last_codec_ =
      data_decoder::mojom::ImageCodec::kPng;
  bool last_shrink_to_fit_ = false;
  int64_t last_max_size_ = 0;
  gfx::Size last_frame_size_{-1, -1};
};

// Images are decoded in the data decoder (FakeDataDecoder here) and written
// as the PNM tesseract reads.
class OcrImageFileTest : public testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(dir_.CreateUniqueTempDir()); }

  std::string ReadBack(const base::FilePath& path) {
    std::string contents;
    EXPECT_TRUE(base::ReadFileToString(path, &contents));
    return contents;
  }

  OcrResult OcrFile(base::span<const uint8_t> image) {
    base::test::TestFuture<OcrResult> future;
    // No tesseract here: a file that gets that far fails at the run.
    internal::OcrImageFile(base::FilePath(), base::FilePath(), image, 1000,
                           kImageOcrTimeout, future.GetCallback());
    EXPECT_FALSE(future.IsReady());
    return future.Take();
  }

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  FakeDataDecoder decoder_;
  base::ScopedTempDir dir_;
};

TEST_F(OcrImageFileTest, DecodesAPngFile) {
  SkBitmap bitmap = Solid(40, 20, SK_ColorWHITE);
  bitmap.erase(SK_ColorBLACK, SkIRect::MakeXYWH(5, 5, 10, 3));
  base::test::TestFuture<SkBitmap, bool> future;
  internal::DecodeImageForOcr(PngOf(bitmap), kImageOcrTimeout,
                              future.GetCallback<const SkBitmap&, bool>());
  const SkBitmap& decoded = future.Get<0>();
  EXPECT_FALSE(future.Get<1>());
  ASSERT_FALSE(decoded.drawsNothing());
  EXPECT_EQ(decoded.width(), 40);
  EXPECT_EQ(decoded.height(), 20);
  EXPECT_EQ(decoded.getColor(6, 6), SK_ColorBLACK);
  EXPECT_EQ(decoded.getColor(30, 15), SK_ColorWHITE);
  // Chromium's own decoders and limits: a huge image is shrunk while it is
  // decoded, never handed over at full size.
  EXPECT_EQ(decoder_.decodes(), 1);
  EXPECT_EQ(decoder_.last_codec(), data_decoder::mojom::ImageCodec::kDefault);
  EXPECT_TRUE(decoder_.last_shrink_to_fit());
  EXPECT_EQ(decoder_.last_max_size(),
            static_cast<int64_t>(data_decoder::kDefaultMaxSizeInBytes));
  EXPECT_EQ(decoder_.last_frame_size(), gfx::Size());
}

TEST_F(OcrImageFileTest, NotAnImageDecodesToNothing) {
  const std::vector<uint8_t> junk = {0x89, 'P',  'N', 'G', '\r', '\n',
                                     0x1a, '\n', 1,   2,   3,    4};
  base::test::TestFuture<SkBitmap, bool> future;
  internal::DecodeImageForOcr(junk, kImageOcrTimeout,
                              future.GetCallback<const SkBitmap&, bool>());
  EXPECT_TRUE(future.Get<0>().drawsNothing());
  EXPECT_FALSE(future.Get<1>());
}

TEST_F(OcrImageFileTest, AHungDecoderTimesOut) {
  decoder_.set_hold_answers(true);
  base::test::TestFuture<SkBitmap, bool> future;
  internal::DecodeImageForOcr(PngOf(Solid(8, 8, SK_ColorWHITE)),
                              base::Seconds(10),
                              future.GetCallback<const SkBitmap&, bool>());
  task_environment_.FastForwardBy(base::Seconds(9));
  EXPECT_FALSE(future.IsReady());
  task_environment_.FastForwardBy(base::Seconds(1));
  ASSERT_TRUE(future.IsReady());
  EXPECT_TRUE(future.Get<0>().drawsNothing());
  EXPECT_TRUE(future.Get<1>());
  // An answer after the deadline goes nowhere.
  decoder_.AnswerHeld();
  task_environment_.RunUntilIdle();

  // Through OcrImageFile, the chat sees why.
  base::test::TestFuture<OcrResult> result;
  internal::OcrImageFile(base::FilePath(), base::FilePath(),
                         PngOf(Solid(8, 8, SK_ColorWHITE)), 1000,
                         base::Seconds(10), result.GetCallback());
  task_environment_.FastForwardBy(base::Seconds(10));
  ASSERT_TRUE(result.IsReady());
  EXPECT_EQ(result.Get().error, "decoding the image timed out");
  decoder_.AnswerHeld();
  task_environment_.RunUntilIdle();
}

TEST_F(OcrImageFileTest, ACrashedDecoderDecodesNothing) {
  decoder_.set_crash(true);
  base::test::TestFuture<SkBitmap, bool> future;
  internal::DecodeImageForOcr(PngOf(Solid(8, 8, SK_ColorWHITE)),
                              kImageOcrTimeout,
                              future.GetCallback<const SkBitmap&, bool>());
  EXPECT_TRUE(future.Get<0>().drawsNothing());
  EXPECT_FALSE(future.Get<1>());
  EXPECT_EQ(decoder_.decodes(), 0);
}

TEST_F(OcrImageFileTest, WritesThePnmTesseractReads) {
  // Gray: P5. Black in the top-left corner, white elsewhere.
  SkBitmap gray = Solid(3, 2, SK_ColorWHITE);
  gray.erase(SK_ColorBLACK, SkIRect::MakeXYWH(0, 0, 1, 1));
  const base::FilePath gray_path = dir_.GetPath().AppendASCII("gray.pnm");
  EXPECT_EQ(internal::WriteOcrPnm(gray, gray_path), "");
  EXPECT_EQ(ReadBack(gray_path),
            std::string("P5\n3 2\n255\n\x00\xff\xff\xff\xff\xff", 17));
  // Colour: P6.
  const base::FilePath red_path = dir_.GetPath().AppendASCII("red.pnm");
  EXPECT_EQ(internal::WriteOcrPnm(Solid(2, 1, SK_ColorRED), red_path), "");
  EXPECT_EQ(ReadBack(red_path),
            std::string("P6\n2 1\n255\n\xff\x00\x00\xff\x00\x00", 17));
  // Transparent: white, as tesseract composites a PNG's alpha.
  const base::FilePath clear_path = dir_.GetPath().AppendASCII("clear.pnm");
  EXPECT_EQ(internal::WriteOcrPnm(Solid(1, 1, SK_ColorTRANSPARENT), clear_path),
            "");
  EXPECT_EQ(ReadBack(clear_path), std::string("P5\n1 1\n255\n\xff", 12));
  // The same bytes as ocr_image.h's writer gives for the same pixels.
  EXPECT_EQ(ReadBack(red_path),
            OcrPnmFromRgba(2, 1, std::string("\xff\0\0\xff\xff\0\0\xff", 8)));

  // Too wide for tesseract; an empty bitmap; never over an existing file.
  SkBitmap wide;
  wide.allocN32Pixels(32768, 1);
  const base::FilePath wide_path = dir_.GetPath().AppendASCII("wide.pnm");
  EXPECT_EQ(internal::WriteOcrPnm(wide, wide_path), kTooLargeForOcr);
  EXPECT_FALSE(base::PathExists(wide_path));
  EXPECT_EQ(internal::WriteOcrPnm(SkBitmap(), wide_path),
            "couldn't decode this image");
  EXPECT_EQ(internal::WriteOcrPnm(gray, gray_path),
            "could not write temp image");
}

TEST_F(OcrImageFileTest, OnlyPngJpegAndWebpAreDecoded) {
  const std::string tiff("II*\0\x08\0\0\0", 8);
  OcrResult result = OcrFile(base::as_byte_span(tiff));
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("TIFF"), std::string::npos) << result.error;
  result = OcrFile(base::as_byte_span(std::string("GIF89a\1\0\1\0")));
  EXPECT_EQ(result.error, "this file isn't a PNG, JPEG or WebP image");
}

TEST_F(OcrImageFileTest, DecodesThenRunsTesseract) {
  // A PNG that decodes reaches the tesseract run (here: no tesseract).
  OcrResult result = OcrFile(PngOf(Solid(16, 16, SK_ColorWHITE)));
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error, "no tesseract binary available");
  // One that doesn't decode never does.
  const std::vector<uint8_t> broken = {0x89, 'P',  'N', 'G', '\r', '\n',
                                       0x1a, '\n', 0,   0,   0,    0};
  result = OcrFile(broken);
  EXPECT_EQ(result.error, "couldn't decode this image");
}

}  // namespace
}  // namespace ocr
}  // namespace molt_ai
