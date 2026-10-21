// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// DocumentParserClient: how the browser parses untrusted documents (audit S5,
// the "Rule of 2"): PDFs downloaded for /pdf, and the PDF / Office / e-book /
// CSV files attached in the AI chat, plus the pages of a scanned PDF rendered
// for OCR. Every parse runs in a utility process under the most restrictive
// sandbox (molt_ai.mojom.DocumentParser, sandbox.mojom.Sandbox.kService;
// //chrome/services/molt_ai/document_parser), so a parser bug or a Rust
// panic costs that process, never the browser.
//
//   - One process per call. Launching costs some latency per document, which
//     these user-initiated actions can afford, and it isolates documents
//     from each other: a document that compromises its parser never sees, or
//     changes the text of, a later one, and a crash or hang affects only its
//     own call. Reusing a process would save the launch but share all three.
//   - A timeout per call: a hostile document can make a parser loop forever.
//     Text calls get kTimeout; rendering pages gets kRenderTimeout, which is
//     longer because PDFium does far more per page than the text scraper
//     (decoding the scan's images, rasterizing up to 20 pages of up to 25 MP,
//     PNG-encoding each). A 20-page scan at 200 dpi takes a few seconds; the
//     limit only bounds a hung or hostile render, and a render that comes
//     close to it still leaves half of the OCR's overall deadline
//     (ocr_service.h) to tesseract. On timeout the call fails, the pipe is
//     closed and the process is terminated. Closing the pipe alone would not
//     do: a process stuck inside a parse never gets to notice it.
//   - A crashed or killed process fails its call (disconnect handler), so
//     every call is answered exactly once.
//   - Replies are untrusted: text or error strings that are not valid UTF-8
//     (base::IsStringUTF8) fail the call, and the text is cut to the call's
//     max_chars. Rendered pages must keep every limit of the mojom (page
//     count, pixel size, total bytes), come in page order, and be
//     well-formed PNGs of the size they claim (signature, chunk structure,
//     IHDR). Each PNG is copied out of the reply before it is checked: a
//     large BigBuffer is shared memory the service could still write to. The
//     browser never decodes them; tesseract does, in its own sandbox.
//
// Not here: OCR itself runs tesseract as a helper process under its own
// OS-level sandbox and deadline (molt_ai/ocr, molt_ai/common/
// helper_sandbox.h), which is looser than this one (and on Windows confines
// no files).
//
// The service keeps no profile state, so incognito and MoltNet profiles need
// nothing special. Use on the UI thread.

#ifndef CHROME_BROWSER_MOLT_AI_DOCUMENT_PARSER_DOCUMENT_PARSER_CLIENT_H_
#define CHROME_BROWSER_MOLT_AI_DOCUMENT_PARSER_DOCUMENT_PARSER_CLIENT_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/process/process.h"
#include "base/sequence_checker.h"
#include "base/time/time.h"
#include "chrome/services/molt_ai/document_parser/public/mojom/document_parser.mojom-forward.h"
#include "mojo/public/cpp/bindings/pending_remote.h"

namespace molt_ai {

struct DocumentParseResult {
  enum class Status {
    // `text` holds the document's text: valid UTF-8, not empty, at most the
    // call's max_chars bytes.
    kOk,
    // The parser ran and found nothing to extract, or refused the document
    // as malformed. `error` may say why; it can be empty.
    kNoText,
    // No usable answer: the parser process crashed, was killed or timed out,
    // its reply was malformed, or the document could not be sent. `error`
    // says which.
    kFailed,
  };

  DocumentParseResult();
  DocumentParseResult(Status status, std::string text, std::string error);
  DocumentParseResult(const DocumentParseResult&);
  DocumentParseResult(DocumentParseResult&&);
  DocumentParseResult& operator=(const DocumentParseResult&);
  DocumentParseResult& operator=(DocumentParseResult&&);
  ~DocumentParseResult();

  Status status = Status::kFailed;
  std::string text;
  std::string error;
};

// One page of a PDF rendered for OCR. `png` passed IsWellFormedPng() for
// `width` x `height`; it has not been decoded.
struct RenderedPdfPage {
  RenderedPdfPage();
  RenderedPdfPage(RenderedPdfPage&&);
  RenderedPdfPage& operator=(RenderedPdfPage&&);
  ~RenderedPdfPage();

  // 0-based index of the page in the document.
  uint32_t page_index = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> png;
};

struct PdfPagesResult {
  enum class Status {
    // `pages` holds at least one page, in page order.
    kOk,
    // The parser ran and could not render the document: not a PDF, damaged,
    // password-protected, or no page could be drawn. `error` may say why.
    kUnreadable,
    // No usable answer: the parser process crashed, was killed or timed out,
    // its reply was malformed, or the document could not be sent. `error`
    // says which.
    kFailed,
  };

  PdfPagesResult();
  PdfPagesResult(PdfPagesResult&&);
  PdfPagesResult& operator=(PdfPagesResult&&);
  ~PdfPagesResult();

  Status status = Status::kFailed;
  // How many pages the document has; can be more than `pages` holds.
  uint32_t page_count = 0;
  std::vector<RenderedPdfPage> pages;
  std::string error;
};

namespace document_parser_client_internal {

// True if |png| is structurally a PNG of |width| x |height| as the
// document parser writes them: the signature, then well-formed chunks (each
// length within the buffer, ASCII-letter types), IHDR first and only once
// (13 bytes: this size, 8 bits per channel, gray / RGB / gray+alpha / RGBA,
// the standard compression and filter), at least one IDAT, and IEND last.
// Doesn't decompress anything or check CRCs (tesseract's libpng does).
// Linear in the size. Exposed for tests.
bool IsWellFormedPng(base::span<const uint8_t> png,
                     uint32_t width,
                     uint32_t height);

}  // namespace document_parser_client_internal

class DocumentParserClient {
 public:
  using ResultCallback = base::OnceCallback<void(DocumentParseResult)>;
  using PdfPagesCallback = base::OnceCallback<void(PdfPagesResult)>;
  // Receives the parser's process once it runs, so that a hung parse can be
  // terminated.
  using ProcessCallback = base::OnceCallback<void(const base::Process&)>;
  // Starts one parser process and returns its remote. The default launches a
  // sandboxed utility process; tests pass a fake.
  using LaunchCallback =
      base::RepeatingCallback<mojo::PendingRemote<mojom::DocumentParser>(
          ProcessCallback)>;

  // The timeout of the text calls (ExtractPdfText, ConvertToMarkdown,
  // ExtractDocxText) and of RenderPdfPages; see the file comment.
  static constexpr base::TimeDelta kTimeout = base::Seconds(30);
  static constexpr base::TimeDelta kRenderTimeout = base::Seconds(60);
  // Each call is a process; calls beyond this many at once fail right away.
  static constexpr size_t kMaxConcurrentCalls = 4;

  // Parses in sandboxed utility processes.
  DocumentParserClient();
  // For tests: |launch| instead of utility processes, and custom timeouts.
  DocumentParserClient(LaunchCallback launch,
                       base::TimeDelta timeout,
                       base::TimeDelta render_timeout = kRenderTimeout);
  DocumentParserClient(const DocumentParserClient&) = delete;
  DocumentParserClient& operator=(const DocumentParserClient&) = delete;
  // Pending calls end without running their callbacks, and their processes
  // are terminated.
  ~DocumentParserClient();

  // Each call copies the document before returning and later runs
  // |callback| exactly once (never synchronously), unless this client is
  // destroyed first. At most |max_chars| bytes of text come back (and never
  // more than mojom::kMaxOutputChars).

  // The text layer of a PDF. kNoText: no readable text layer (e.g. a scan).
  void ExtractPdfText(base::span<const uint8_t> pdf,
                      size_t max_chars,
                      ResultCallback callback);

  // Office / e-book / spreadsheet / CSV to Markdown (the anydoc crate).
  // |extension| is the file name's, lowercase and without the dot ("docx");
  // the format is sniffed from the bytes first. kNoText carries anydoc's
  // diagnostic, or an empty error when the document converted to nothing.
  void ConvertToMarkdown(base::span<const uint8_t> document,
                         std::string_view extension,
                         size_t max_chars,
                         ResultCallback callback);

  // The flat text of a .docx (word/document.xml).
  void ExtractDocxText(base::span<const uint8_t> docx,
                       size_t max_chars,
                       ResultCallback callback);

  // The first |max_pages| pages of a PDF (clamped to [1,
  // mojom::kMaxRenderPages]) rendered at |dpi| (clamped to
  // [mojom::kMinRenderDpi, mojom::kMaxRenderDpi]) as PNGs, for OCR. Same
  // rules as the calls above, with kRenderTimeout.
  void RenderPdfPages(base::span<const uint8_t> pdf,
                      uint32_t max_pages,
                      uint32_t dpi,
                      PdfPagesCallback callback);

  size_t pending_calls_for_testing() const { return calls_.size(); }

 private:
  class Call;
  class ProcessKiller;

  // Starts |call| (its process launching) with |timeout| and returns it, or
  // returns null after answering its callback with a refusal.
  Call* StartCall(std::unique_ptr<Call> call,
                  size_t document_size,
                  base::TimeDelta timeout);
  // Runs |answer| (a caller's callback bound to its result) from a fresh
  // task, unless this client is destroyed first.
  void Refuse(base::OnceClosure answer);
  void RunRefusal(base::OnceClosure answer);
  // Ends |call| (closing its pipe) and then runs |answer|.
  void OnCallDone(Call* call, base::OnceClosure answer);

  const LaunchCallback launch_;
  const base::TimeDelta timeout_;
  const base::TimeDelta render_timeout_;
  std::vector<std::unique_ptr<Call>> calls_;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<DocumentParserClient> weak_ptr_factory_{this};
};

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_DOCUMENT_PARSER_DOCUMENT_PARSER_CLIENT_H_
