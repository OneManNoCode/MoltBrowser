// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/document_parser/document_parser_client.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/process/process.h"
#include "base/run_loop.h"
#include "base/strings/string_view_util.h"
#include "base/test/multiprocess_test.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "base/test/test_timeouts.h"
#include "base/threading/platform_thread.h"
#include "build/build_config.h"
#include "chrome/services/molt_ai/document_parser/public/mojom/document_parser.mojom.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/multiprocess_func_list.h"

namespace molt_ai {
namespace {

using document_parser_client_internal::IsWellFormedPng;
using PagesStatus = PdfPagesResult::Status;
using Status = DocumentParseResult::Status;

mojom::ParseResultPtr Reply(bool success,
                            std::string text,
                            std::string error = std::string()) {
  mojom::ParseResultPtr reply = mojom::ParseResult::New();
  reply->success = success;
  reply->text = std::move(text);
  reply->error = std::move(error);
  return reply;
}

void AppendBe32(std::string& out, uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(static_cast<char>((value >> shift) & 0xff));
  }
}

// A PNG chunk. The CRC is left zero: the browser doesn't check CRCs.
std::string Chunk(std::string_view type,
                  std::string_view data,
                  std::optional<uint32_t> length = std::nullopt) {
  std::string chunk;
  AppendBe32(chunk, length.value_or(static_cast<uint32_t>(data.size())));
  chunk += type;
  chunk += data;
  AppendBe32(chunk, 0);
  return chunk;
}

constexpr char kPngSignature[] = "\x89PNG\r\n\x1a\n";

std::string Ihdr(uint32_t width,
                 uint32_t height,
                 uint8_t bit_depth = 8,
                 uint8_t color_type = 2) {
  std::string data;
  AppendBe32(data, width);
  AppendBe32(data, height);
  data.push_back(static_cast<char>(bit_depth));
  data.push_back(static_cast<char>(color_type));
  data.append(3, '\0');  // Compression, filter, no interlace.
  return Chunk("IHDR", data);
}

// A structurally valid PNG of |width| x |height| (the browser never decodes
// it, so the IDAT needn't be real zlib data). |idat_size| pads the IDAT.
std::string MakePng(uint32_t width, uint32_t height, size_t idat_size = 16) {
  return std::string(kPngSignature, 8) + Ihdr(width, height) +
         Chunk("IDAT", std::string(idat_size, 'x')) + Chunk("IEND", "");
}

mojom::PdfPageImagePtr Page(uint32_t index,
                            uint32_t width,
                            uint32_t height,
                            const std::string& png) {
  mojom::PdfPageImagePtr page = mojom::PdfPageImage::New();
  page->page_index = index;
  page->width = width;
  page->height = height;
  page->png = mojo_base::BigBuffer(base::as_byte_span(png));
  return page;
}

// A successful reply with |count| pages of 100 x 50 from page 0, in a
// document of |page_count| pages.
mojom::PdfPageImagesPtr PagesReply(uint32_t count, uint32_t page_count) {
  mojom::PdfPageImagesPtr reply = mojom::PdfPageImages::New();
  reply->success = true;
  reply->page_count = page_count;
  for (uint32_t i = 0; i < count; ++i) {
    reply->pages.push_back(Page(i, 100, 50, MakePng(100, 50)));
  }
  return reply;
}

// The test fixture plays the utility process: it is the DocumentParser
// every launch connects to.
class DocumentParserClientTest : public testing::Test,
                                 public mojom::DocumentParser {
 protected:
  enum class Behavior {
    kReply,  // Answer with |reply_|.
    kHang,   // Never answer (keeps the reply callback).
    kCrash,  // Close every pipe without answering.
  };

  DocumentParserClientTest()
      : client_(std::make_unique<DocumentParserClient>(
            base::BindRepeating(&DocumentParserClientTest::Launch,
                                base::Unretained(this)),
            DocumentParserClient::kTimeout)) {}

  ~DocumentParserClientTest() override {
    client_.reset();
    receivers_.Clear();
    hung_.clear();
    hung_pages_.clear();
  }

  mojo::PendingRemote<mojom::DocumentParser> Launch(
      DocumentParserClient::ProcessCallback on_launched) {
    ++launches_;
    if (defer_process_callback_) {
      deferred_process_callback_ = std::move(on_launched);
    } else if (process_for_launch_) {
      std::move(on_launched).Run(*process_for_launch_);
    }
    mojo::PendingRemote<mojom::DocumentParser> remote;
    receivers_.Add(this, remote.InitWithNewPipeAndPassReceiver());
    return remote;
  }

  // mojom::DocumentParser:
  void ExtractPdfText(mojo_base::BigBuffer pdf,
                      uint32_t max_chars,
                      ExtractPdfTextCallback callback) override {
    Handle(std::move(pdf), std::string(), max_chars, std::move(callback));
  }
  void ConvertToMarkdown(mojo_base::BigBuffer document,
                         const std::string& extension,
                         uint32_t max_chars,
                         ConvertToMarkdownCallback callback) override {
    Handle(std::move(document), extension, max_chars, std::move(callback));
  }
  void ExtractDocxText(mojo_base::BigBuffer docx,
                       uint32_t max_chars,
                       ExtractDocxTextCallback callback) override {
    Handle(std::move(docx), std::string(), max_chars, std::move(callback));
  }
  void RenderPdfPages(mojo_base::BigBuffer pdf,
                      uint32_t max_pages,
                      uint32_t dpi,
                      RenderPdfPagesCallback callback) override {
    ++parses_;
    last_document_ =
        std::string(base::as_string_view(base::span<const uint8_t>(pdf)));
    last_max_pages_ = max_pages;
    last_dpi_ = dpi;
    switch (behavior_) {
      case Behavior::kReply:
        // Each test sets a fresh reply for each render.
        ASSERT_TRUE(pages_reply_);
        std::move(callback).Run(std::move(pages_reply_));
        return;
      case Behavior::kHang:
        hung_pages_.push_back(std::move(callback));
        return;
      case Behavior::kCrash:
        receivers_.Clear();
        return;
    }
  }

  void Handle(mojo_base::BigBuffer document,
              std::string extension,
              uint32_t max_chars,
              base::OnceCallback<void(mojom::ParseResultPtr)> callback) {
    ++parses_;
    last_document_ =
        std::string(base::as_string_view(base::span<const uint8_t>(document)));
    last_extension_ = std::move(extension);
    last_max_chars_ = max_chars;
    switch (behavior_) {
      case Behavior::kReply:
        std::move(callback).Run(reply_.Clone());
        return;
      case Behavior::kHang:
        hung_.push_back(std::move(callback));
        return;
      case Behavior::kCrash:
        // Close the pipes first: a reply callback may only be dropped
        // unrun once its pipe is gone.
        receivers_.Clear();
        return;
    }
  }

  // Runs |client_->ExtractPdfText()| on |document| to completion.
  DocumentParseResult ParsePdf(const std::string& document,
                               size_t max_chars = 1000) {
    base::test::TestFuture<DocumentParseResult> future;
    client_->ExtractPdfText(base::as_byte_span(document), max_chars,
                            future.GetCallback());
    return future.Take();
  }

  // Runs |client_->RenderPdfPages()| to completion, with the service
  // answering |reply|.
  PdfPagesResult Render(mojom::PdfPageImagesPtr reply,
                        uint32_t max_pages = 20,
                        uint32_t dpi = 200,
                        const std::string& document = "%PDF-") {
    pages_reply_ = std::move(reply);
    base::test::TestFuture<PdfPagesResult> future;
    client_->RenderPdfPages(base::as_byte_span(document), max_pages, dpi,
                            future.GetCallback());
    return future.Take();
  }

  // Expects |reply| to fail the render as malformed.
  void ExpectMalformed(mojom::PdfPageImagesPtr reply,
                       uint32_t max_pages = 20) {
    PdfPagesResult result = Render(std::move(reply), max_pages);
    EXPECT_EQ(result.status, PagesStatus::kFailed);
    EXPECT_TRUE(result.pages.empty());
    EXPECT_NE(result.error.find("malformed"), std::string::npos);
  }

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  mojo::ReceiverSet<mojom::DocumentParser> receivers_;
  std::unique_ptr<DocumentParserClient> client_;

  Behavior behavior_ = Behavior::kReply;
  mojom::ParseResultPtr reply_ = Reply(true, "hello");
  mojom::PdfPageImagesPtr pages_reply_;
  std::vector<base::OnceCallback<void(mojom::ParseResultPtr)>> hung_;
  std::vector<RenderPdfPagesCallback> hung_pages_;
  // The process each launch reports, and whether it reports it only later
  // (when the test runs |deferred_process_callback_|).
  std::optional<base::Process> process_for_launch_;
  bool defer_process_callback_ = false;
  DocumentParserClient::ProcessCallback deferred_process_callback_;
  int launches_ = 0;
  int parses_ = 0;
  std::string last_document_;
  std::string last_extension_;
  uint32_t last_max_chars_ = 0;
  uint32_t last_max_pages_ = 0;
  uint32_t last_dpi_ = 0;
};

TEST_F(DocumentParserClientTest, PassesTheDocumentAndReturnsText) {
  base::test::TestFuture<DocumentParseResult> future;
  client_->ConvertToMarkdown(base::as_byte_span(std::string("PK\x03\x04 doc")),
                             "docx", 500, future.GetCallback());
  DocumentParseResult result = future.Take();
  EXPECT_EQ(result.status, Status::kOk);
  EXPECT_EQ(result.text, "hello");
  EXPECT_TRUE(result.error.empty());
  EXPECT_EQ(last_document_, "PK\x03\x04 doc");
  EXPECT_EQ(last_extension_, "docx");
  EXPECT_EQ(last_max_chars_, 500u);
}

TEST_F(DocumentParserClientTest, NeverAnswersSynchronously) {
  bool answered = false;
  client_->ExtractDocxText(
      base::as_byte_span(std::string("doc")), 100,
      base::BindOnce(
          [](bool* answered, DocumentParseResult) { *answered = true; },
          &answered));
  EXPECT_FALSE(answered);
  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(answered);
}

TEST_F(DocumentParserClientTest, OneProcessPerCallClosedAfterwards) {
  EXPECT_EQ(ParsePdf("%PDF-1").status, Status::kOk);
  EXPECT_EQ(ParsePdf("%PDF-2").status, Status::kOk);
  EXPECT_EQ(launches_, 2);
  base::RunLoop().RunUntilIdle();
  // Every pipe is closed once its call is answered.
  EXPECT_TRUE(receivers_.empty());
  EXPECT_EQ(client_->pending_calls_for_testing(), 0u);
}

TEST_F(DocumentParserClientTest, MaxCharsIsClampedToTheServiceLimit) {
  ParsePdf("%PDF-", size_t{mojom::kMaxOutputChars} * 4);
  EXPECT_EQ(last_max_chars_, mojom::kMaxOutputChars);
}

TEST_F(DocumentParserClientTest, CutsTextToMaxChars) {
  reply_ = Reply(true, std::string(100, 'a'));
  DocumentParseResult result = ParsePdf("%PDF-", 10);
  EXPECT_EQ(result.status, Status::kOk);
  EXPECT_EQ(result.text, std::string(10, 'a'));

  // Never inside a character: five 2-byte 'é's cut at 5 bytes are two.
  reply_ = Reply(true, "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9");
  result = ParsePdf("%PDF-", 5);
  EXPECT_EQ(result.status, Status::kOk);
  EXPECT_EQ(result.text, "\xC3\xA9\xC3\xA9");
}

TEST_F(DocumentParserClientTest, RejectsMalformedText) {
  for (const char* bad :
       {"\xff", "ab\xC3", "\xED\xA0\x80", "non-character \xEF\xBF\xBE"}) {
    reply_ = Reply(true, bad);
    DocumentParseResult result = ParsePdf("%PDF-");
    EXPECT_EQ(result.status, Status::kFailed) << bad;
    EXPECT_TRUE(result.text.empty());
    EXPECT_NE(result.error.find("malformed"), std::string::npos);
  }
  reply_ = Reply(false, std::string(), "bad \xff error");
  EXPECT_EQ(ParsePdf("%PDF-").status, Status::kFailed);
}

TEST_F(DocumentParserClientTest, NoTextCarriesTheError) {
  reply_ = Reply(false, std::string(), "the PDF has no readable text layer");
  DocumentParseResult result = ParsePdf("%PDF-");
  EXPECT_EQ(result.status, Status::kNoText);
  EXPECT_TRUE(result.text.empty());
  EXPECT_EQ(result.error, "the PDF has no readable text layer");

  // Text that comes with a failure is ignored.
  reply_ = Reply(false, "ignored", "nope");
  result = ParsePdf("%PDF-");
  EXPECT_EQ(result.status, Status::kNoText);
  EXPECT_TRUE(result.text.empty());

  // "Success" with no text is no text.
  reply_ = Reply(true, std::string());
  EXPECT_EQ(ParsePdf("%PDF-").status, Status::kNoText);
}

TEST_F(DocumentParserClientTest, LongErrorsAreCut) {
  reply_ = Reply(false, std::string(), std::string(5000, 'e'));
  DocumentParseResult result = ParsePdf("%PDF-");
  EXPECT_EQ(result.status, Status::kNoText);
  EXPECT_EQ(result.error.size(), size_t{mojom::kMaxErrorBytes});
}

TEST_F(DocumentParserClientTest, CrashFailsTheCall) {
  behavior_ = Behavior::kCrash;
  DocumentParseResult result = ParsePdf("%PDF-");
  EXPECT_EQ(result.status, Status::kFailed);
  EXPECT_NE(result.error.find("stopped unexpectedly"), std::string::npos);
  EXPECT_EQ(client_->pending_calls_for_testing(), 0u);
}

TEST_F(DocumentParserClientTest, TimeoutFailsTheCallAndClosesThePipe) {
  behavior_ = Behavior::kHang;
  base::test::TestFuture<DocumentParseResult> future;
  client_->ExtractPdfText(base::as_byte_span(std::string("%PDF-")), 100,
                          future.GetCallback());
  task_environment_.FastForwardBy(DocumentParserClient::kTimeout -
                                  base::Seconds(1));
  EXPECT_FALSE(future.IsReady());
  EXPECT_EQ(parses_, 1);

  task_environment_.FastForwardBy(base::Seconds(2));
  ASSERT_TRUE(future.IsReady());
  DocumentParseResult result = future.Take();
  EXPECT_EQ(result.status, Status::kFailed);
  EXPECT_NE(result.error.find("timed out"), std::string::npos);
  EXPECT_EQ(client_->pending_calls_for_testing(), 0u);
  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(receivers_.empty());
}

TEST_F(DocumentParserClientTest, DestroyingTheClientDropsPendingCalls) {
  behavior_ = Behavior::kHang;
  bool answered = false;
  client_->ExtractPdfText(
      base::as_byte_span(std::string("%PDF-")), 100,
      base::BindOnce(
          [](bool* answered, DocumentParseResult) { *answered = true; },
          &answered));
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(parses_, 1);
  client_.reset();
  task_environment_.FastForwardBy(DocumentParserClient::kTimeout * 2);
  EXPECT_FALSE(answered);
  EXPECT_TRUE(receivers_.empty());
}

TEST_F(DocumentParserClientTest, RefusesEmptyAndOversizeDocuments) {
  EXPECT_EQ(ParsePdf(std::string()).status, Status::kNoText);

  const std::string huge(size_t{mojom::kMaxDocumentBytes} + 1, 'x');
  DocumentParseResult result = ParsePdf(huge);
  EXPECT_EQ(result.status, Status::kFailed);
  EXPECT_NE(result.error.find("too large"), std::string::npos);
  EXPECT_EQ(launches_, 0);
}

TEST_F(DocumentParserClientTest, LimitsConcurrentCalls) {
  behavior_ = Behavior::kHang;
  for (size_t i = 0; i < DocumentParserClient::kMaxConcurrentCalls; ++i) {
    client_->ExtractPdfText(base::as_byte_span(std::string("%PDF-")), 100,
                            base::DoNothing());
  }
  DocumentParseResult result = ParsePdf("%PDF-");
  EXPECT_EQ(result.status, Status::kFailed);
  EXPECT_NE(result.error.find("too many"), std::string::npos);
  EXPECT_EQ(launches_,
            static_cast<int>(DocumentParserClient::kMaxConcurrentCalls));
}

TEST_F(DocumentParserClientTest, CallbackMayStartAnotherCall) {
  // The .docx fallback: a failed conversion starts the flat-text call from
  // inside its callback.
  reply_ = Reply(false, std::string(), "anydoc can't read it");
  base::test::TestFuture<DocumentParseResult> fallback;
  client_->ConvertToMarkdown(
      base::as_byte_span(std::string("PK\x03\x04")), "docx", 100,
      base::BindOnce(
          [](DocumentParserClient* client,
             base::OnceCallback<void(DocumentParseResult)> done,
             DocumentParseResult first) {
            EXPECT_EQ(first.status, Status::kNoText);
            client->ExtractDocxText(base::as_byte_span(std::string("PK")), 100,
                                    std::move(done));
          },
          client_.get(), fallback.GetCallback()));
  EXPECT_EQ(fallback.Take().status, Status::kNoText);
  EXPECT_EQ(launches_, 2);
}

TEST_F(DocumentParserClientTest, RenderReturnsTheCheckedPages) {
  mojom::PdfPageImagesPtr reply = mojom::PdfPageImages::New();
  reply->success = true;
  reply->page_count = 7;
  const std::string first = MakePng(100, 50);
  const std::string second = MakePng(30, 40);
  reply->pages.push_back(Page(0, 100, 50, first));
  // Pages may be left out (one PDFium couldn't draw).
  reply->pages.push_back(Page(2, 30, 40, second));
  PdfPagesResult result = Render(std::move(reply), /*max_pages=*/5,
                                 /*dpi=*/150, "%PDF-1.7 scan");
  ASSERT_EQ(result.status, PagesStatus::kOk) << result.error;
  EXPECT_TRUE(result.error.empty());
  EXPECT_EQ(result.page_count, 7u);
  ASSERT_EQ(result.pages.size(), 2u);
  EXPECT_EQ(result.pages[0].page_index, 0u);
  EXPECT_EQ(result.pages[0].width, 100u);
  EXPECT_EQ(result.pages[0].height, 50u);
  EXPECT_EQ(std::string(result.pages[0].png.begin(), result.pages[0].png.end()),
            first);
  EXPECT_EQ(result.pages[1].page_index, 2u);
  EXPECT_EQ(std::string(result.pages[1].png.begin(), result.pages[1].png.end()),
            second);
  EXPECT_EQ(last_document_, "%PDF-1.7 scan");
  EXPECT_EQ(last_max_pages_, 5u);
  EXPECT_EQ(last_dpi_, 150u);
}

TEST_F(DocumentParserClientTest, RenderClampsPagesAndDpi) {
  Render(PagesReply(1, 1), /*max_pages=*/1000, /*dpi=*/5000);
  EXPECT_EQ(last_max_pages_, mojom::kMaxRenderPages);
  EXPECT_EQ(last_dpi_, mojom::kMaxRenderDpi);

  Render(PagesReply(1, 1), /*max_pages=*/0, /*dpi=*/0);
  EXPECT_EQ(last_max_pages_, 1u);
  EXPECT_EQ(last_dpi_, mojom::kMinRenderDpi);
}

TEST_F(DocumentParserClientTest, RenderUnreadableCarriesTheError) {
  mojom::PdfPageImagesPtr reply = mojom::PdfPageImages::New();
  reply->success = false;
  reply->error = "the PDF is password-protected";
  // Pages that come with a failure are ignored.
  reply->pages.push_back(Page(0, 100, 50, MakePng(100, 50)));
  PdfPagesResult result = Render(std::move(reply));
  EXPECT_EQ(result.status, PagesStatus::kUnreadable);
  EXPECT_TRUE(result.pages.empty());
  EXPECT_EQ(result.error, "the PDF is password-protected");

  reply = mojom::PdfPageImages::New();
  reply->success = false;
  reply->error = std::string(5000, 'e');
  result = Render(std::move(reply));
  EXPECT_EQ(result.status, PagesStatus::kUnreadable);
  EXPECT_EQ(result.error.size(), size_t{mojom::kMaxErrorBytes});

  reply = mojom::PdfPageImages::New();
  reply->success = false;
  reply->error = "bad \xff error";
  EXPECT_EQ(Render(std::move(reply)).status, PagesStatus::kFailed);
}

TEST_F(DocumentParserClientTest, RenderRejectsTooManyPages) {
  // More pages than the mojom allows ...
  ExpectMalformed(PagesReply(mojom::kMaxRenderPages + 1, 100));
  // ... or than the call asked for.
  ExpectMalformed(PagesReply(4, 100), /*max_pages=*/3);
  // "Success" without pages.
  ExpectMalformed(PagesReply(0, 100));
  // More pages than the document has.
  ExpectMalformed(PagesReply(3, 2));
}

TEST_F(DocumentParserClientTest, RenderRejectsBadPageIndices) {
  const std::string png = MakePng(100, 50);
  // Beyond what the call asked for.
  mojom::PdfPageImagesPtr reply = PagesReply(0, 100);
  reply->pages.push_back(Page(3, 100, 50, png));
  ExpectMalformed(std::move(reply), /*max_pages=*/3);
  // Beyond the document.
  reply = PagesReply(0, 2);
  reply->pages.push_back(Page(2, 100, 50, png));
  ExpectMalformed(std::move(reply));
  // Out of order, and repeated.
  reply = PagesReply(0, 100);
  reply->pages.push_back(Page(1, 100, 50, png));
  reply->pages.push_back(Page(0, 100, 50, png));
  ExpectMalformed(std::move(reply));
  reply = PagesReply(0, 100);
  reply->pages.push_back(Page(1, 100, 50, png));
  reply->pages.push_back(Page(1, 100, 50, png));
  ExpectMalformed(std::move(reply));
}

TEST_F(DocumentParserClientTest, RenderRejectsOversizePages) {
  const auto one_page = [](uint32_t width, uint32_t height) {
    mojom::PdfPageImagesPtr reply = PagesReply(0, 1);
    reply->pages.push_back(Page(0, width, height, MakePng(width, height)));
    return reply;
  };
  ExpectMalformed(one_page(0, 50));
  ExpectMalformed(one_page(100, 0));
  ExpectMalformed(one_page(mojom::kMaxPageImageSide + 1, 10));
  ExpectMalformed(one_page(10, mojom::kMaxPageImageSide + 1));
  // Each side within the limit, the area not.
  ExpectMalformed(one_page(mojom::kMaxPageImageSide, 2600));
  // At the limits is fine.
  EXPECT_EQ(Render(one_page(mojom::kMaxPageImageSide, 2500)).status,
            PagesStatus::kOk);
}

TEST_F(DocumentParserClientTest, RenderRejectsMoreBytesThanTheLimit) {
  // One page whose PNG alone is over the total.
  mojom::PdfPageImagesPtr reply = PagesReply(0, 1);
  reply->pages.push_back(
      Page(0, 100, 50, MakePng(100, 50, size_t{mojom::kMaxPageImageBytes})));
  ExpectMalformed(std::move(reply));
}

TEST_F(DocumentParserClientTest, RenderRejectsWhatIsNotAPng) {
  const auto with_png = [](const std::string& png) {
    mojom::PdfPageImagesPtr reply = PagesReply(0, 1);
    reply->pages.push_back(Page(0, 100, 50, png));
    return reply;
  };
  ExpectMalformed(with_png(std::string()));
  ExpectMalformed(with_png("GIF89a not a png at all"));
  // A PNG of another size than the reply claims.
  ExpectMalformed(with_png(MakePng(50, 100)));
  // Cut short.
  const std::string png = MakePng(100, 50);
  ExpectMalformed(with_png(png.substr(0, png.size() - 1)));
}

TEST_F(DocumentParserClientTest, RenderTimeoutIsLongerThanTheTextTimeout) {
  static_assert(DocumentParserClient::kRenderTimeout >
                DocumentParserClient::kTimeout);
  behavior_ = Behavior::kHang;
  base::test::TestFuture<PdfPagesResult> future;
  client_->RenderPdfPages(base::as_byte_span(std::string("%PDF-")), 20, 200,
                          future.GetCallback());
  task_environment_.FastForwardBy(DocumentParserClient::kTimeout +
                                  base::Seconds(1));
  EXPECT_FALSE(future.IsReady());
  task_environment_.FastForwardBy(DocumentParserClient::kRenderTimeout -
                                  DocumentParserClient::kTimeout);
  ASSERT_TRUE(future.IsReady());
  PdfPagesResult result = future.Take();
  EXPECT_EQ(result.status, PagesStatus::kFailed);
  EXPECT_NE(result.error.find("timed out"), std::string::npos);
  EXPECT_EQ(client_->pending_calls_for_testing(), 0u);
}

TEST_F(DocumentParserClientTest, RenderCrashFailsTheCall) {
  behavior_ = Behavior::kCrash;
  PdfPagesResult result = Render(PagesReply(1, 1));
  EXPECT_EQ(result.status, PagesStatus::kFailed);
  EXPECT_NE(result.error.find("stopped unexpectedly"), std::string::npos);
}

TEST_F(DocumentParserClientTest, RenderRefusesEmptyAndOversizeDocuments) {
  PdfPagesResult result = Render(nullptr, 20, 200, std::string());
  EXPECT_EQ(result.status, PagesStatus::kUnreadable);
  EXPECT_NE(result.error.find("empty"), std::string::npos);

  const std::string huge(size_t{mojom::kMaxDocumentBytes} + 1, 'x');
  result = Render(nullptr, 20, 200, huge);
  EXPECT_EQ(result.status, PagesStatus::kFailed);
  EXPECT_NE(result.error.find("too large"), std::string::npos);
  EXPECT_EQ(launches_, 0);
}

TEST_F(DocumentParserClientTest, RenderSharesTheConcurrencyLimit) {
  behavior_ = Behavior::kHang;
  for (size_t i = 0; i < DocumentParserClient::kMaxConcurrentCalls; ++i) {
    client_->ExtractPdfText(base::as_byte_span(std::string("%PDF-")), 100,
                            base::DoNothing());
  }
  PdfPagesResult result = Render(nullptr);
  EXPECT_EQ(result.status, PagesStatus::kFailed);
  EXPECT_NE(result.error.find("too many"), std::string::npos);
}

TEST(DocumentParserPngCheckTest, AcceptsWellFormedPngs) {
  EXPECT_TRUE(IsWellFormedPng(base::as_byte_span(MakePng(100, 50)), 100, 50));
  // Ancillary chunks and several IDATs are fine; so are RGBA and gray.
  const std::string sig(kPngSignature, 8);
  EXPECT_TRUE(IsWellFormedPng(
      base::as_byte_span(sig + Ihdr(3, 4) + Chunk("tEXt", "a\0b") +
                         Chunk("IDAT", "xx") + Chunk("IDAT", "yy") +
                         Chunk("IEND", "")),
      3, 4));
  for (const uint8_t color_type : {uint8_t{0}, uint8_t{4}, uint8_t{6}}) {
    EXPECT_TRUE(IsWellFormedPng(
        base::as_byte_span(sig + Ihdr(3, 4, 8, color_type) +
                           Chunk("IDAT", "x") + Chunk("IEND", "")),
        3, 4))
        << int{color_type};
  }
}

TEST(DocumentParserPngCheckTest, RejectsMalformedPngs) {
  const std::string sig(kPngSignature, 8);
  const std::string idat = Chunk("IDAT", "xx");
  const std::string iend = Chunk("IEND", "");
  const auto check = [](const std::string& png) {
    return IsWellFormedPng(base::as_byte_span(png), 3, 4);
  };
  EXPECT_FALSE(check(std::string()));
  EXPECT_FALSE(check(sig));
  // Wrong signature.
  EXPECT_FALSE(check("\x89PNG\r\n\x1a\r" + Ihdr(3, 4) + idat + iend));
  // Wrong size.
  EXPECT_FALSE(check(sig + Ihdr(4, 3) + idat + iend));
  // IHDR not first, twice, or the wrong length.
  EXPECT_FALSE(check(sig + idat + Ihdr(3, 4) + iend));
  EXPECT_FALSE(check(sig + Ihdr(3, 4) + Ihdr(3, 4) + idat + iend));
  EXPECT_FALSE(check(sig + Chunk("IHDR", std::string(12, '\0')) + idat + iend));
  // Formats the parser never writes: 16-bit, palette.
  EXPECT_FALSE(check(sig + Ihdr(3, 4, 16, 2) + idat + iend));
  EXPECT_FALSE(check(sig + Ihdr(3, 4, 8, 3) + idat + iend));
  // No IDAT; no IEND; IEND with data; something after IEND.
  EXPECT_FALSE(check(sig + Ihdr(3, 4) + iend));
  EXPECT_FALSE(check(sig + Ihdr(3, 4) + idat));
  EXPECT_FALSE(check(sig + Ihdr(3, 4) + idat + Chunk("IEND", "x")));
  EXPECT_FALSE(check(sig + Ihdr(3, 4) + idat + iend + "x"));
  // A chunk longer than what is left, or than PNG allows.
  EXPECT_FALSE(check(sig + Ihdr(3, 4) + Chunk("IDAT", "xx", 1000) + iend));
  EXPECT_FALSE(
      check(sig + Ihdr(3, 4) + Chunk("IDAT", "xx", 0xFFFFFFFF) + iend));
  // A chunk type that isn't four letters.
  EXPECT_FALSE(check(sig + Ihdr(3, 4) + Chunk("ID4T", "xx") + idat + iend));
}

#if !BUILDFLAG(IS_ANDROID) && !BUILDFLAG(IS_IOS)
// Stands in for a parser stuck in a hostile document: sleeps for minutes,
// then exits with 0.
MULTIPROCESS_TEST_MAIN(DocumentParserHungChild) {
  base::PlatformThread::Sleep(TestTimeouts::action_max_timeout());
  return 0;
}

base::Process SpawnHungChild() {
  return base::SpawnMultiProcessTestChild(
      "DocumentParserHungChild",
      base::GetMultiProcessTestChildBaseCommandLine(), base::LaunchOptions());
}

// True if |child| was terminated (rather than finishing its sleep).
bool WasTerminated(const base::Process& child) {
  int exit_code = 0;
  return base::WaitForMultiprocessTestChildExit(
             child, TestTimeouts::action_timeout(), &exit_code) &&
         exit_code != 0;
}

TEST_F(DocumentParserClientTest, TimeoutTerminatesTheProcess) {
  base::Process child = SpawnHungChild();
  ASSERT_TRUE(child.IsValid());
  process_for_launch_ = child.Duplicate();
  behavior_ = Behavior::kHang;

  DocumentParseResult result = ParsePdf("%PDF-");
  EXPECT_EQ(result.status, Status::kFailed);
  // Closing the pipe can't stop a process stuck in a parse; the client
  // terminated it.
  EXPECT_TRUE(WasTerminated(child));
}

TEST_F(DocumentParserClientTest, ProcessLaunchedAfterTheTimeoutIsTerminated) {
  base::Process child = SpawnHungChild();
  ASSERT_TRUE(child.IsValid());
  process_for_launch_ = child.Duplicate();
  defer_process_callback_ = true;
  behavior_ = Behavior::kHang;

  EXPECT_EQ(ParsePdf("%PDF-").status, Status::kFailed);
  // The launch completes only now, after the call is gone.
  ASSERT_TRUE(deferred_process_callback_);
  std::move(deferred_process_callback_).Run(*process_for_launch_);
  EXPECT_TRUE(WasTerminated(child));
}

TEST_F(DocumentParserClientTest, DestroyingTheClientTerminatesItsProcesses) {
  base::Process child = SpawnHungChild();
  ASSERT_TRUE(child.IsValid());
  process_for_launch_ = child.Duplicate();
  behavior_ = Behavior::kHang;

  client_->ExtractPdfText(base::as_byte_span(std::string("%PDF-")), 100,
                          base::DoNothing());
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(parses_, 1);
  client_.reset();
  EXPECT_TRUE(WasTerminated(child));
}
#endif  // !BUILDFLAG(IS_ANDROID) && !BUILDFLAG(IS_IOS)

}  // namespace
}  // namespace molt_ai
