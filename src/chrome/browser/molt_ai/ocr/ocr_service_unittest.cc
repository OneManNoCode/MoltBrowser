// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/ocr/ocr_service.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/raw_ptr.h"
#include "base/strings/string_util.h"
#include "base/strings/string_view_util.h"
#include "base/test/simple_test_tick_clock.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "base/time/time.h"
#include "chrome/browser/molt_ai/document_parser/document_parser_client.h"
#include "chrome/services/molt_ai/document_parser/public/mojom/document_parser.mojom.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace ocr {
namespace {

using internal::OcrRenderedPages;
using internal::PageOcrFunction;

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

// Stands in for tesseract: the page's text is its marker, except "fail",
// which fails. Records each call's timeout, and advances |clock| by
// |seconds_per_page|.
class FakeOcr {
 public:
  explicit FakeOcr(base::SimpleTestTickClock* clock = nullptr,
                   int seconds_per_page = 0)
      : clock_(clock), seconds_per_page_(seconds_per_page) {}

  PageOcrFunction Function() {
    return base::BindRepeating(&FakeOcr::Run, base::Unretained(this));
  }

  const std::vector<base::TimeDelta>& timeouts() const { return timeouts_; }

 private:
  OcrResult Run(base::span<const uint8_t> png,
                size_t max_chars,
                base::TimeDelta timeout) {
    timeouts_.push_back(timeout);
    if (clock_) {
      clock_->Advance(base::Seconds(seconds_per_page_));
    }
    const std::string marker(base::as_string_view(png));
    if (marker == "fail") {
      return Error("tesseract exit=1: bad page");
    }
    return Text(marker.substr(0, max_chars));
  }

  raw_ptr<base::SimpleTestTickClock> clock_;
  const int seconds_per_page_;
  std::vector<base::TimeDelta> timeouts_;
};

class OcrRenderedPagesTest : public testing::Test {
 protected:
  OcrResult Run(const std::vector<RenderedPdfPage>& pages,
                uint32_t page_count,
                size_t max_chars,
                FakeOcr& ocr) {
    return OcrRenderedPages(pages, page_count, max_chars,
                            clock_.NowTicks() + kPdfOcrTimeout, &clock_,
                            ocr.Function());
  }

  base::SimpleTestTickClock clock_;
};

TEST_F(OcrRenderedPagesTest, JoinsPagesWithHeaders) {
  FakeOcr ocr;
  // tesseract ends a page with a form feed; page 2 has no text.
  OcrResult result = Run(
      FakePages({{0, "Hello\f"}, {1, " \n\f"}, {2, "World\n"}}), 3, 1000, ocr);
  ASSERT_TRUE(result.success) << result.error;
  EXPECT_EQ(result.text, "--- Page 1 ---\nHello\n\n--- Page 3 ---\nWorld");
  EXPECT_TRUE(result.error.empty());
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
  // The two minutes a PDF's OCR has always had, rendering included.
  static_assert(kPdfOcrTimeout == base::Seconds(120));
  // Each page takes 50 s of the 120 s: the first two get the full
  // per-page limit, the third what is left, and the fourth isn't started.
  FakeOcr ocr(&clock_, /*seconds_per_page=*/50);
  OcrResult result =
      Run(FakePages({{0, "one"}, {1, "two"}, {2, "three"}, {3, "four"}}), 4,
          1000, ocr);
  ASSERT_TRUE(result.success);
  ASSERT_EQ(ocr.timeouts().size(), 3u);
  EXPECT_EQ(ocr.timeouts()[0], kImageOcrTimeout);
  EXPECT_EQ(ocr.timeouts()[1], kImageOcrTimeout);
  EXPECT_EQ(ocr.timeouts()[2], kPdfOcrTimeout - base::Seconds(100));
  EXPECT_TRUE(base::EndsWith(result.text, "--- OCR read 3 of 4 pages ---"));
  EXPECT_EQ(result.text.find("four"), std::string::npos);
}

TEST_F(OcrRenderedPagesTest, NoTimeLeft) {
  FakeOcr ocr;
  OcrResult result = OcrRenderedPages(
      FakePages({{0, "one"}}), 1, 1000,
      clock_.NowTicks() + kMinPageOcrTime - base::Seconds(1), &clock_,
      ocr.Function());
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error, "OCR ran out of time");
  EXPECT_TRUE(ocr.timeouts().empty());
}

// The whole of OcrPdf but the singleton and tesseract: a fake document
// parser renders, a fake OCR reads the "PNGs".
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
  static OcrResult ReadMarker(base::span<const uint8_t> png,
                              size_t max_chars,
                              base::TimeDelta timeout) {
    const std::string bytes(base::as_string_view(png));
    // Signature (8) + IHDR chunk (25) + IDAT length and type (8).
    const size_t start = 8 + 25 + 8;
    const size_t end = bytes.size() - 12 - 4;  // IEND, IDAT's CRC.
    if (bytes.size() < start + 16 || end < start) {
      return Error("not one of ours");
    }
    return Text(bytes.substr(start, end - start));
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

TEST_F(RenderAndOcrPdfTest, AnEmptyPdfFailsAsItAlwaysHas) {
  base::test::TestFuture<OcrResult> future;
  internal::RenderAndOcrPdf(
      parser_, base::span<const uint8_t>(), 1000,
      base::TimeTicks::Now() + kPdfOcrTimeout,
      base::BindRepeating([](base::span<const uint8_t>, size_t,
                             base::TimeDelta) { return Text("never"); }),
      future.GetCallback());
  EXPECT_FALSE(future.IsReady());  // Never synchronously.
  OcrResult result = future.Take();
  EXPECT_FALSE(result.success);
  // Not the parser's "the document is empty": it never got the call.
  EXPECT_EQ(result.error, "empty pdf");
  EXPECT_TRUE(receivers_.empty());
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
  // nothing reaches tesseract.
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
          [](bool* ran, base::span<const uint8_t>, size_t, base::TimeDelta) {
            *ran = true;
            return Text("never");
          },
          &ocr_ran),
      future.GetCallback());
  OcrResult result = future.Take();
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("malformed"), std::string::npos);
  EXPECT_FALSE(ocr_ran);
}

}  // namespace
}  // namespace ocr
}  // namespace molt_ai
