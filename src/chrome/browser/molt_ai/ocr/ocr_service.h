// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// OcrService — local OCR via bundled tesseract.
//
// Two inputs:
//   - Images (chat attachments): tesseract reads PNG, JPEG and WebP itself
//     (OcrImageWithTesseract).
//   - PDFs with no text layer (scans), the fallback of /pdf and of PDF
//     attachments when the sandboxed parser finds no text (OcrPdf).
//     tesseract can't read PDFs: it decodes its input through leptonica,
//     which writes PDFs but doesn't read them ("Pdf reading is not
//     supported"). So the sandboxed document parser renders the first
//     kPdfOcrMaxPages pages at kPdfOcrDpi to PNG
//     (DocumentParserClient::RenderPdfPages: PDFium in a kService utility
//     process), the browser checks the PNGs without decoding them, and
//     tesseract OCRs each page like an attached image. The pages' text is
//     joined with "--- Page N ---" headers.
//
// Resolution (FindHelperBinary, helper_binary.h — same as VoiceService and
// TorManager):
//   1. the bundled copy: MoltBrowser.app/Contents/Resources/ocr/tesseract on
//      macOS, <install dir>/ocr/tesseract[.exe] on Linux and Windows;
//   2. Linux only: /usr/bin/tesseract, if root owns it and every directory
//      above it.
// No Homebrew, /usr/local, /opt/local or PATH fallback (audit S18).
//
// tesseract reads untrusted images (attachments, pages of any website's
// PDF), so it runs under the helper sandbox (helper_sandbox.h): it may read
// its own directory, its tessdata and the input, write only a private
// per-run scratch directory, never use the network, and is killed at a
// deadline (kImageOcrTimeout per image or page). A PDF has one overall
// deadline, kPdfOcrTimeout, for rendering and every page: rendering is
// bounded by DocumentParserClient::kRenderTimeout (60 s), and pages not
// reached in time are left out, which the text says.
//
// Threading: OcrPdf on the UI thread (it renders through its
// DocumentParserClient there); tesseract runs on MayBlock thread-pool tasks.

#ifndef CHROME_BROWSER_MOLT_AI_OCR_OCR_SERVICE_H_
#define CHROME_BROWSER_MOLT_AI_OCR_OCR_SERVICE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/time/time.h"
#include "chrome/browser/molt_ai/common/helper_binary.h"
#include "chrome/browser/molt_ai/common/helper_sandbox.h"
#include "chrome/browser/molt_ai/document_parser/document_parser_client.h"

namespace base {
template <typename T>
class NoDestructor;
class TickClock;
}  // namespace base

namespace molt_ai {
namespace ocr {

// Deadlines. One image, or one page of a PDF: tesseract takes a few
// seconds per page. A whole PDF (rendering, then up to kPdfOcrMaxPages
// pages one after another): rendering takes at most
// DocumentParserClient::kRenderTimeout, which leaves two minutes for
// tesseract.
inline constexpr base::TimeDelta kImageOcrTimeout = base::Seconds(60);
inline constexpr base::TimeDelta kPdfOcrTimeout = base::Seconds(180);
// No page is started with less time than this left before the deadline.
inline constexpr base::TimeDelta kMinPageOcrTime = base::Seconds(5);

// The pages of a PDF that are OCR'd (from the first), and their resolution
// (tesseract does well from about 200 dpi). The document parser's
// mojom::kMaxRenderPages and mojom::kMaxRenderDpi.
inline constexpr uint32_t kPdfOcrMaxPages = 20;
inline constexpr uint32_t kPdfOcrDpi = 200;

// tesseract's stdout and stderr kept per run (only error messages use it).
inline constexpr size_t kMaxTesseractOutputBytes = 256 * 1024;
// The most of tesseract's .txt output read back (callers cut it further).
inline constexpr size_t kMaxTesseractTextFileBytes = 4 * 1024 * 1024;

// Runs |bin| (tesseract) on |input| under the helper sandbox, writing
// <|out_stem|>.txt. |input| and |out_stem| are in |scratch_dir|, the only
// directory it may write. It may read its own directory and, when
// |tessdata_parent| is set (bundled builds), |tessdata_parent|/tessdata,
// which it is also pointed at. A system tesseract finds its data under
// /usr. Blocking; MayBlock thread-pool tasks only.
molt_ai::HelperRunResult RunSandboxedTesseract(
    const base::FilePath& bin,
    const base::FilePath& tessdata_parent,
    const base::FilePath& scratch_dir,
    const base::FilePath& input,
    const base::FilePath& out_stem,
    base::TimeDelta timeout);

struct OcrResult {
  OcrResult();
  OcrResult(const OcrResult&);
  OcrResult(OcrResult&&);
  OcrResult& operator=(const OcrResult&);
  OcrResult& operator=(OcrResult&&);
  ~OcrResult();

  bool success = false;
  // Valid UTF-8, at most the call's max_chars bytes; not empty on success.
  std::string text;
  std::string error;
  std::string binary_source;  // "bundled" | "system" | "none"
  std::string binary_path;
  int duration_ms = 0;
};

// OCRs one image with tesseract |bin| (|tessdata_parent| as for
// RunSandboxedTesseract): |image| holds a PNG, JPEG or WebP (tesseract
// sniffs the format; |extension|, e.g. "png", only names the input file)
// and is written to a fresh private scratch directory, deleted afterwards.
// tesseract runs sandboxed and is killed after |timeout|. Its text is made
// valid UTF-8 and cut to |max_chars| (SanitizeHelperText); output that is
// only whitespace is a failure ("OCR returned no text"). Sets text, error,
// binary_path and duration_ms. Blocking; MayBlock thread-pool tasks only.
OcrResult OcrImageWithTesseract(const base::FilePath& bin,
                                const base::FilePath& tessdata_parent,
                                base::span<const uint8_t> image,
                                std::string_view extension,
                                size_t max_chars,
                                base::TimeDelta timeout);

class OcrService {
 public:
  static OcrService* Get();

  OcrService(const OcrService&) = delete;
  OcrService& operator=(const OcrService&) = delete;

  base::FilePath ResolveTesseractBinary() const;
  bool IsUsingBundledTesseract() const;
  base::FilePath GetBundledOcrDir() const;

  // OCRs a PDF that has no text layer, given its raw bytes (see the file
  // comment): its first kPdfOcrMaxPages pages are rendered in the sandboxed
  // document parser and each is OCR'd with tesseract, all within
  // kPdfOcrTimeout. The text is capped at |max_chars|, since OCR of long
  // PDFs would tank the chat context budget. |on_done| runs on the UI
  // thread, never synchronously. UI thread only.
  void OcrPdf(std::string pdf_bytes,
              size_t max_chars,
              base::OnceCallback<void(OcrResult)> on_done);

 private:
  friend class base::NoDestructor<OcrService>;
  OcrService();
  ~OcrService();

  // The tesseract to run (bundled, else on Linux a root-owned
  // /usr/bin/tesseract), or why there is none.
  molt_ai::HelperBinary FindTesseract() const;

  // Renders PDF pages for OcrPdf, one sandboxed process per PDF. Created on
  // first use, on the UI thread; this singleton is never destroyed.
  std::unique_ptr<DocumentParserClient> document_parser_;
};

// The steps of OcrPdf, exposed for tests.
namespace internal {

// OCR of one rendered page: |png|, its text cut to |max_chars|, tesseract
// killed after |timeout|. Blocking; runs on a MayBlock thread-pool task.
using PageOcrFunction =
    base::RepeatingCallback<OcrResult(base::span<const uint8_t> png,
                                      size_t max_chars,
                                      base::TimeDelta timeout)>;

// OCRs |pages| in order with |ocr_page| and joins the text of those that
// have any as "--- Page N ---\n<text>" (N counts from 1), separated by a
// blank line. A page starts only while at least kMinPageOcrTime is left
// before |deadline| (by |clock|) and gets at most kImageOcrTimeout; a page
// that fails is skipped. When the document has more pages (|page_count|)
// than were OCR'd, a last line says how many were, if it fits. The text is
// valid UTF-8 of at most |max_chars| bytes. Fails, with the last page's
// error, when no page gave text. Blocking.
OcrResult OcrRenderedPages(const std::vector<RenderedPdfPage>& pages,
                           uint32_t page_count,
                           size_t max_chars,
                           base::TimeTicks deadline,
                           const base::TickClock* clock,
                           const PageOcrFunction& ocr_page);

// OcrPdf without the singleton and with a given page OCR: renders |pdf|
// through |parser| (which copies it before returning), then runs
// OcrRenderedPages on a MayBlock thread-pool task. |on_done| runs on the
// calling sequence, never synchronously, unless |parser| is destroyed while
// rendering. Sets success, text and error only.
void RenderAndOcrPdf(DocumentParserClient& parser,
                     base::span<const uint8_t> pdf,
                     size_t max_chars,
                     base::TimeTicks deadline,
                     PageOcrFunction ocr_page,
                     base::OnceCallback<void(OcrResult)> on_done);

}  // namespace internal

}  // namespace ocr
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_OCR_OCR_SERVICE_H_
