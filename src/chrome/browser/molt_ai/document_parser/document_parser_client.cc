// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/document_parser/document_parser_client.h"

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/numerics/byte_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"
#include "base/timer/timer.h"
#include "chrome/services/molt_ai/document_parser/public/mojom/document_parser.mojom.h"
#include "content/public/browser/service_process_host.h"
#include "content/public/common/result_codes.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "mojo/public/cpp/bindings/remote.h"

namespace molt_ai {

namespace document_parser_client_internal {

namespace {

bool ChunkTypeIs(base::span<const uint8_t> type, std::string_view name) {
  return std::ranges::equal(type, base::as_byte_span(name));
}

}  // namespace

bool IsWellFormedPng(base::span<const uint8_t> png,
                     uint32_t width,
                     uint32_t height) {
  static constexpr std::array<uint8_t, 8> kSignature = {
      0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  // A chunk is a 4-byte big-endian data length, a 4-byte type, the data and
  // a 4-byte CRC.
  constexpr size_t kChunkOverhead = 12;
  constexpr uint32_t kMaxChunkLength = 0x7FFFFFFF;  // The PNG spec's limit.
  if (png.size() < kSignature.size() ||
      !std::ranges::equal(png.first(kSignature.size()), kSignature)) {
    return false;
  }
  base::span<const uint8_t> rest = png.subspan(kSignature.size());
  bool first = true;
  bool saw_idat = false;
  while (!rest.empty()) {
    if (rest.size() < kChunkOverhead) {
      return false;
    }
    const uint32_t length = base::U32FromBigEndian(rest.first<4u>());
    if (length > kMaxChunkLength || length > rest.size() - kChunkOverhead) {
      return false;
    }
    const base::span<const uint8_t> type = rest.subspan(4u, 4u);
    if (!std::ranges::all_of(type, [](uint8_t c) {
          return base::IsAsciiAlpha(c);
        })) {
      return false;
    }
    const base::span<const uint8_t> data = rest.subspan(8u, length);
    if (first) {
      if (!ChunkTypeIs(type, "IHDR") || length != 13) {
        return false;
      }
      const uint8_t bit_depth = data[8];
      const uint8_t color_type = data[9];
      const uint8_t compression = data[10];
      const uint8_t filter = data[11];
      const uint8_t interlace = data[12];
      if (base::U32FromBigEndian(data.first<4u>()) != width ||
          base::U32FromBigEndian(data.subspan(4u).first<4u>()) != height ||
          bit_depth != 8 ||
          (color_type != 0 && color_type != 2 && color_type != 4 &&
           color_type != 6) ||
          compression != 0 || filter != 0 || interlace > 1) {
        return false;
      }
      first = false;
    } else if (ChunkTypeIs(type, "IHDR")) {
      return false;
    } else if (ChunkTypeIs(type, "IDAT")) {
      saw_idat = true;
    } else if (ChunkTypeIs(type, "IEND")) {
      return length == 0 && rest.size() == kChunkOverhead && saw_idat;
    }
    rest = rest.subspan(kChunkOverhead + length);
  }
  return false;  // No IEND.
}

}  // namespace document_parser_client_internal

namespace {

using Status = DocumentParseResult::Status;
using PagesStatus = PdfPagesResult::Status;

// Shown in the OS task manager for the parser processes.
constexpr char16_t kDisplayName[] = u"MoltBrowser Document Parser";

constexpr char kMalformedReply[] =
    "the document parser returned malformed output";

mojo::PendingRemote<mojom::DocumentParser> LaunchInUtilityProcess(
    DocumentParserClient::ProcessCallback on_launched) {
  mojo::PendingRemote<mojom::DocumentParser> remote;
  content::ServiceProcessHost::Launch<mojom::DocumentParser>(
      remote.InitWithNewPipeAndPassReceiver(),
      content::ServiceProcessHost::Options()
          .WithDisplayName(std::u16string(kDisplayName))
          .WithProcessCallback(std::move(on_launched))
          .Pass());
  return remote;
}

DocumentParseResult Failed(std::string error) {
  return DocumentParseResult(Status::kFailed, std::string(), std::move(error));
}

PdfPagesResult PagesFailed(std::string error) {
  PdfPagesResult result;
  result.status = PagesStatus::kFailed;
  result.error = std::move(error);
  return result;
}

uint32_t ClampMaxChars(size_t max_chars) {
  return static_cast<uint32_t>(
      std::min<size_t>(max_chars, mojom::kMaxOutputChars));
}

// The utility process may be compromised: check everything it sends.
DocumentParseResult ValidateReply(mojom::ParseResultPtr reply,
                                  size_t max_chars) {
  if (!reply || !base::IsStringUTF8(reply->text) ||
      !base::IsStringUTF8(reply->error)) {
    return Failed(kMalformedReply);
  }
  std::string error(
      base::TruncateUTF8ToByteSize(reply->error, mojom::kMaxErrorBytes));
  if (!reply->success) {
    return DocumentParseResult(Status::kNoText, std::string(),
                               std::move(error));
  }
  std::string text = std::move(reply->text);
  if (text.size() > max_chars) {
    text = std::string(base::TruncateUTF8ToByteSize(text, max_chars));
  }
  if (text.empty()) {
    return DocumentParseResult(Status::kNoText, std::string(),
                               std::move(error));
  }
  return DocumentParseResult(Status::kOk, std::move(text), std::string());
}

// Likewise for rendered pages: anything outside the mojom's limits, out of
// order, or not a well-formed PNG of its stated size fails the whole call.
// |max_pages| is the (clamped) number of pages the call asked for.
PdfPagesResult ValidatePages(mojom::PdfPageImagesPtr reply,
                             uint32_t max_pages) {
  if (!reply || !base::IsStringUTF8(reply->error)) {
    return PagesFailed(kMalformedReply);
  }
  std::string error(
      base::TruncateUTF8ToByteSize(reply->error, mojom::kMaxErrorBytes));
  if (!reply->success) {
    // Whatever pages came with a failure are ignored.
    PdfPagesResult result;
    result.status = PagesStatus::kUnreadable;
    result.error = std::move(error);
    return result;
  }
  if (reply->pages.empty() || reply->pages.size() > max_pages) {
    return PagesFailed(kMalformedReply);
  }
  PdfPagesResult result;
  result.status = PagesStatus::kOk;
  result.page_count = reply->page_count;
  uint64_t total_bytes = 0;
  std::optional<uint32_t> previous_index;
  for (const mojom::PdfPageImagePtr& page : reply->pages) {
    if (!page || page->page_index >= max_pages ||
        page->page_index >= reply->page_count ||
        (previous_index && page->page_index <= *previous_index)) {
      return PagesFailed(kMalformedReply);
    }
    previous_index = page->page_index;
    if (page->width == 0 || page->height == 0 ||
        page->width > mojom::kMaxPageImageSide ||
        page->height > mojom::kMaxPageImageSide ||
        uint64_t{page->width} * page->height > mojom::kMaxPageImagePixels) {
      return PagesFailed(kMalformedReply);
    }
    // An invalid BigBuffer (shared memory that could not be mapped) is empty.
    const base::span<const uint8_t> bytes(page->png);
    total_bytes += bytes.size();
    if (bytes.empty() || total_bytes > mojom::kMaxPageImageBytes) {
      return PagesFailed(kMalformedReply);
    }
    // Copy first, check the copy: a large BigBuffer is shared memory, which
    // a compromised service could still change after the check.
    RenderedPdfPage rendered;
    rendered.page_index = page->page_index;
    rendered.width = page->width;
    rendered.height = page->height;
    rendered.png.assign(bytes.begin(), bytes.end());
    if (!document_parser_client_internal::IsWellFormedPng(
            rendered.png, rendered.width, rendered.height)) {
      return PagesFailed(kMalformedReply);
    }
    result.pages.push_back(std::move(rendered));
  }
  return result;
}

}  // namespace

DocumentParseResult::DocumentParseResult() = default;
DocumentParseResult::DocumentParseResult(Status status,
                                         std::string text,
                                         std::string error)
    : status(status), text(std::move(text)), error(std::move(error)) {}
DocumentParseResult::DocumentParseResult(const DocumentParseResult&) = default;
DocumentParseResult::DocumentParseResult(DocumentParseResult&&) = default;
DocumentParseResult& DocumentParseResult::operator=(
    const DocumentParseResult&) = default;
DocumentParseResult& DocumentParseResult::operator=(DocumentParseResult&&) =
    default;
DocumentParseResult::~DocumentParseResult() = default;

RenderedPdfPage::RenderedPdfPage() = default;
RenderedPdfPage::RenderedPdfPage(RenderedPdfPage&&) = default;
RenderedPdfPage& RenderedPdfPage::operator=(RenderedPdfPage&&) = default;
RenderedPdfPage::~RenderedPdfPage() = default;

PdfPagesResult::PdfPagesResult() = default;
PdfPagesResult::PdfPagesResult(PdfPagesResult&&) = default;
PdfPagesResult& PdfPagesResult::operator=(PdfPagesResult&&) = default;
PdfPagesResult::~PdfPagesResult() = default;

// The process of one call, for terminating it. Shared with the launch
// callback, which can run after its Call is gone (the client was destroyed,
// or the call timed out, before the launch finished): a kill requested before
// the launch happens as soon as the process exists.
class DocumentParserClient::ProcessKiller
    : public base::RefCounted<ProcessKiller> {
 public:
  ProcessKiller() = default;
  ProcessKiller(const ProcessKiller&) = delete;
  ProcessKiller& operator=(const ProcessKiller&) = delete;

  void OnLaunched(const base::Process& process) {
    if (!process.IsValid()) {
      return;
    }
    if (kill_exit_code_) {
      process.Terminate(*kill_exit_code_, /*wait=*/false);
      return;
    }
    process_ = process.Duplicate();
  }

  // Terminates the process (only ever once). Call it only while the call is
  // pending: the process can't have exited and had its id reused then,
  // since its exit would have disconnected the call first.
  void Kill(int exit_code) {
    if (kill_exit_code_) {
      return;
    }
    kill_exit_code_ = exit_code;
    if (process_.IsValid()) {
      process_.Terminate(exit_code, /*wait=*/false);
      process_.Close();
    }
  }

 private:
  friend class base::RefCounted<ProcessKiller>;
  ~ProcessKiller() = default;

  base::Process process_;
  std::optional<int> kill_exit_code_;
};

// One call: its process, its pipe, its timeout and its caller's callback,
// which is a text callback or a pages callback (exactly one is set until the
// call is answered).
class DocumentParserClient::Call {
 public:
  Call(DocumentParserClient* owner, ResultCallback callback)
      : owner_(owner),
        text_callback_(std::move(callback)),
        process_(base::MakeRefCounted<ProcessKiller>()) {}
  Call(DocumentParserClient* owner, PdfPagesCallback callback)
      : owner_(owner),
        pages_callback_(std::move(callback)),
        process_(base::MakeRefCounted<ProcessKiller>()) {}
  Call(const Call&) = delete;
  Call& operator=(const Call&) = delete;

  // A call destroyed before it was answered (its client is going away)
  // takes its process with it.
  ~Call() {
    if (pending()) {
      process_->Kill(content::RESULT_CODE_KILLED);
    }
  }

  // Launches the parser process and arms the timeout. False if the launcher
  // gave no pipe.
  bool Start(const LaunchCallback& launch, base::TimeDelta timeout) {
    remote_.Bind(
        launch.Run(base::BindOnce(&ProcessKiller::OnLaunched, process_)));
    if (!remote_.is_bound()) {
      return false;
    }
    // Unretained: |remote_| and |timer_| are owned by this call and drop
    // their callbacks when it is destroyed.
    remote_.set_disconnect_handler(
        base::BindOnce(&Call::OnDisconnected, base::Unretained(this)));
    timer_.Start(FROM_HERE, timeout,
                 base::BindOnce(&Call::OnTimeout, base::Unretained(this)));
    return true;
  }

  mojom::DocumentParser* parser() { return remote_.get(); }

  // The reply callbacks of the text methods and of RenderPdfPages. Unretained
  // as above: |remote_| drops a pending reply when this call is destroyed.
  base::OnceCallback<void(mojom::ParseResultPtr)> TextReplyCallback(
      size_t max_chars) {
    return base::BindOnce(&Call::OnTextReply, base::Unretained(this),
                          max_chars);
  }
  base::OnceCallback<void(mojom::PdfPageImagesPtr)> PagesReplyCallback(
      uint32_t max_pages) {
    return base::BindOnce(&Call::OnPagesReply, base::Unretained(this),
                          max_pages);
  }

  // The caller's callback bound to a kFailed answer with |error|.
  base::OnceClosure TakeFailure(std::string error) {
    DCHECK(pending());
    if (text_callback_) {
      return base::BindOnce(std::move(text_callback_),
                            Failed(std::move(error)));
    }
    return base::BindOnce(std::move(pages_callback_),
                          PagesFailed(std::move(error)));
  }

  // The caller's callback bound to "the parser can't read this" (kNoText,
  // kUnreadable) with |error|.
  base::OnceClosure TakeRefusal(std::string error) {
    DCHECK(pending());
    if (text_callback_) {
      return base::BindOnce(std::move(text_callback_),
                            DocumentParseResult(Status::kNoText, std::string(),
                                                std::move(error)));
    }
    PdfPagesResult result;
    result.status = PagesStatus::kUnreadable;
    result.error = std::move(error);
    return base::BindOnce(std::move(pages_callback_), std::move(result));
  }

 private:
  bool pending() const { return text_callback_ || pages_callback_; }

  // Each of these ends the call: |this| is destroyed inside OnCallDone().
  void OnTextReply(size_t max_chars, mojom::ParseResultPtr reply) {
    owner_->OnCallDone(this,
                       base::BindOnce(std::move(text_callback_),
                                      ValidateReply(std::move(reply),
                                                    max_chars)));
  }

  void OnPagesReply(uint32_t max_pages, mojom::PdfPageImagesPtr reply) {
    owner_->OnCallDone(this,
                       base::BindOnce(std::move(pages_callback_),
                                      ValidatePages(std::move(reply),
                                                    max_pages)));
  }

  void OnDisconnected() {
    owner_->OnCallDone(this,
                       TakeFailure("the document parser stopped unexpectedly"));
  }

  void OnTimeout() {
    process_->Kill(content::RESULT_CODE_HUNG);
    owner_->OnCallDone(this, TakeFailure("the document parser timed out"));
  }

  const raw_ptr<DocumentParserClient> owner_;
  ResultCallback text_callback_;
  PdfPagesCallback pages_callback_;
  const scoped_refptr<ProcessKiller> process_;
  mojo::Remote<mojom::DocumentParser> remote_;
  base::OneShotTimer timer_;
};

DocumentParserClient::DocumentParserClient()
    : DocumentParserClient(base::BindRepeating(&LaunchInUtilityProcess),
                           kTimeout,
                           kRenderTimeout) {}

DocumentParserClient::DocumentParserClient(LaunchCallback launch,
                                           base::TimeDelta timeout,
                                           base::TimeDelta render_timeout)
    : launch_(std::move(launch)),
      timeout_(timeout),
      render_timeout_(render_timeout) {}

DocumentParserClient::~DocumentParserClient() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

void DocumentParserClient::ExtractPdfText(base::span<const uint8_t> pdf,
                                          size_t max_chars,
                                          ResultCallback callback) {
  const uint32_t chars = ClampMaxChars(max_chars);
  Call* call = StartCall(std::make_unique<Call>(this, std::move(callback)),
                         pdf.size(), timeout_);
  if (call) {
    call->parser()->ExtractPdfText(mojo_base::BigBuffer(pdf), chars,
                                   call->TextReplyCallback(chars));
  }
}

void DocumentParserClient::ConvertToMarkdown(base::span<const uint8_t> document,
                                             std::string_view extension,
                                             size_t max_chars,
                                             ResultCallback callback) {
  const uint32_t chars = ClampMaxChars(max_chars);
  Call* call = StartCall(std::make_unique<Call>(this, std::move(callback)),
                         document.size(), timeout_);
  if (call) {
    call->parser()->ConvertToMarkdown(mojo_base::BigBuffer(document),
                                      std::string(extension), chars,
                                      call->TextReplyCallback(chars));
  }
}

void DocumentParserClient::ExtractDocxText(base::span<const uint8_t> docx,
                                           size_t max_chars,
                                           ResultCallback callback) {
  const uint32_t chars = ClampMaxChars(max_chars);
  Call* call = StartCall(std::make_unique<Call>(this, std::move(callback)),
                         docx.size(), timeout_);
  if (call) {
    call->parser()->ExtractDocxText(mojo_base::BigBuffer(docx), chars,
                                    call->TextReplyCallback(chars));
  }
}

void DocumentParserClient::RenderPdfPages(base::span<const uint8_t> pdf,
                                          uint32_t max_pages,
                                          uint32_t dpi,
                                          PdfPagesCallback callback) {
  // The service clamps the same way; the reply is checked against these.
  max_pages = std::clamp(max_pages, uint32_t{1}, mojom::kMaxRenderPages);
  dpi = std::clamp(dpi, mojom::kMinRenderDpi, mojom::kMaxRenderDpi);
  Call* call = StartCall(std::make_unique<Call>(this, std::move(callback)),
                         pdf.size(), render_timeout_);
  if (call) {
    call->parser()->RenderPdfPages(mojo_base::BigBuffer(pdf), max_pages, dpi,
                                   call->PagesReplyCallback(max_pages));
  }
}

DocumentParserClient::Call* DocumentParserClient::StartCall(
    std::unique_ptr<Call> call,
    size_t document_size,
    base::TimeDelta timeout) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (document_size == 0) {
    Refuse(call->TakeRefusal("the document is empty"));
    return nullptr;
  }
  if (document_size > mojom::kMaxDocumentBytes) {
    Refuse(call->TakeFailure("the document is too large to read (over 32 MiB)"));
    return nullptr;
  }
  if (calls_.size() >= kMaxConcurrentCalls) {
    Refuse(call->TakeFailure(
        "too many documents are being read at once; try again"));
    return nullptr;
  }
  if (!call->Start(launch_, timeout)) {
    Refuse(call->TakeFailure("the document parser could not be started"));
    return nullptr;
  }
  calls_.push_back(std::move(call));
  return calls_.back().get();
}

void DocumentParserClient::Refuse(base::OnceClosure answer) {
  // Never answer synchronously; and like every other answer, a refusal is
  // dropped if this client goes away first.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&DocumentParserClient::RunRefusal,
                                weak_ptr_factory_.GetWeakPtr(),
                                std::move(answer)));
}

void DocumentParserClient::RunRefusal(base::OnceClosure answer) {
  std::move(answer).Run();
}

void DocumentParserClient::OnCallDone(Call* call, base::OnceClosure answer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = std::find_if(
      calls_.begin(), calls_.end(),
      [call](const std::unique_ptr<Call>& c) { return c.get() == call; });
  CHECK(it != calls_.end());
  std::unique_ptr<Call> done = std::move(*it);
  calls_.erase(it);
  // Closes the pipe: a parser that answered exits on its own (a hung one was
  // terminated by OnTimeout()).
  done.reset();
  // Last: the answer may start another call, or destroy this client.
  std::move(answer).Run();
}

}  // namespace molt_ai
