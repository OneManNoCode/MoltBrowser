// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/document_parser/document_parser_client.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
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

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  mojo::ReceiverSet<mojom::DocumentParser> receivers_;
  std::unique_ptr<DocumentParserClient> client_;

  Behavior behavior_ = Behavior::kReply;
  mojom::ParseResultPtr reply_ = Reply(true, "hello");
  std::vector<base::OnceCallback<void(mojom::ParseResultPtr)>> hung_;
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
