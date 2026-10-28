// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// OcrService — local OCR via bundled tesseract.
//
// Two inputs, one way in:
//   - Images (chat attachments, OcrImage): PNG, JPEG or WebP.
//   - PDFs with no text layer (scans), the fallback of /pdf and of PDF
//     attachments when the sandboxed parser finds no text (OcrPdf).
//     tesseract can't read PDFs: it decodes its input through leptonica,
//     which writes PDFs but doesn't read them ("Pdf reading is not
//     supported"). So the sandboxed document parser renders the first
//     kPdfOcrMaxPages pages at kPdfOcrDpi to PNG
//     (DocumentParserClient::RenderPdfPages: PDFium in a kService utility
//     process), the browser checks the PNGs without decoding them, and each
//     page is OCR'd like an image, one after another. The pages' text is
//     joined with "--- Page N ---" headers.
// tesseract never reads the image file itself. Chromium's data decoder (a
// sandboxed utility process, which shrinks an image past 128 MiB of
// pixels while decoding it) decodes it, and tesseract reads a binary PNM
// the browser writes from the pixels (ocr_image.h: the format leptonica
// reads with the least code). Its PNG, JPEG, TIFF, GIF and WebP readers
// are never reached. A TIFF, or anything but PNG, JPEG or WebP, is refused
// with a reason.
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
// deadline. An image, or one page of a PDF, has kImageOcrTimeout for
// decoding and tesseract together. A PDF has one overall deadline,
// kPdfOcrTimeout, for rendering and every page: rendering is bounded by
// DocumentParserClient::kRenderTimeout (60 s), and pages not reached in
// time are left out, which the text says.
//
// Threading: OcrImage and OcrPdf on the UI thread (they decode, and render
// through their DocumentParserClient, there); tesseract runs on MayBlock
// thread-pool tasks.

#ifndef CHROME_BROWSER_MOLT_AI_OCR_OCR_SERVICE_H_
#define CHROME_BROWSER_MOLT_AI_OCR_OCR_SERVICE_H_

#include <atomic>
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

class SkBitmap;

namespace base {
template <typename T>
class NoDestructor;
class TickClock;
}  // namespace base

namespace molt_ai {
namespace ocr {

// Deadlines. One image, or one page of a PDF (decoding it and tesseract):
// tesseract takes a few seconds per page. A whole PDF (rendering, then up
// to kPdfOcrMaxPages pages one after another): rendering takes at most
// DocumentParserClient::kRenderTimeout, which leaves two minutes for the
// pages.
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
// <|out_stem|>.txt. |input| and |out_stem| are in |scratch|, the only
// directory it may write. It may read its own directory and, when
// |tessdata_parent| is set (bundled builds), |tessdata_parent|/tessdata,
// which it is also pointed at. A system tesseract finds its data under
// /usr. Blocking; MayBlock thread-pool tasks only.
molt_ai::HelperRunResult RunSandboxedTesseract(
    const base::FilePath& bin,
    const base::FilePath& tessdata_parent,
    const molt_ai::HelperScratchDir& scratch,
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

// OCRs one decoded image with tesseract |bin| (|tessdata_parent| as for
// RunSandboxedTesseract): writes it to a fresh private scratch directory
// (deleted afterwards) as the PNM ocr_image.h describes, and runs
// tesseract on that, sandboxed and killed after |timeout|. Its text is made
// valid UTF-8 and cut to |max_chars| (SanitizeHelperText); output that is
// only whitespace is a failure ("OCR returned no text"), and so is an
// image more than kMaxOcrImageSide pixels on a side. Sets text, error,
// binary_path and duration_ms. Blocking; MayBlock thread-pool tasks only.
OcrResult OcrBitmapWithTesseract(const base::FilePath& bin,
                                 const base::FilePath& tessdata_parent,
                                 const SkBitmap& bitmap,
                                 size_t max_chars,
                                 base::TimeDelta timeout);

namespace internal {

// How many OCR runs (OcrImage, OcrPdf) may be under way at once. Each holds
// a decoder or document parser process, and up to 128 MiB of decoded image
// in the browser, so attachments sent faster than they are read (or by a
// compromised chat page) can't make it hold an unbounded number.
inline constexpr size_t kMaxOcrRunsInFlight = 2;
inline constexpr std::string_view kOcrBusyError =
    "OCR is already reading two other files: try again once they finish";

// The runs under way, at most kMaxOcrRunsInFlight. Thread-safe.
class OcrRunSlots {
 public:
  OcrRunSlots();
  OcrRunSlots(const OcrRunSlots&) = delete;
  OcrRunSlots& operator=(const OcrRunSlots&) = delete;
  ~OcrRunSlots();

  // Takes a slot for one more run; false if every slot is taken.
  bool TryTake();
  // |done| wrapped to give a taken slot back once it has run, or when it is
  // destroyed unrun, so a run that never answers can't keep its slot. This
  // must outlive the wrapper.
  base::OnceCallback<void(OcrResult)> ReleaseAfter(
      base::OnceCallback<void(OcrResult)> done);
  size_t in_flight() const { return in_flight_.load(); }

 private:
  void Release();

  std::atomic<size_t> in_flight_{0};
};

}  // namespace internal

class OcrService {
 public:
  static OcrService* Get();

  OcrService(const OcrService&) = delete;
  OcrService& operator=(const OcrService&) = delete;

  base::FilePath ResolveTesseractBinary() const;
  bool IsUsingBundledTesseract() const;
  base::FilePath GetBundledOcrDir() const;

  // OCRs an image file, given its bytes: a PNG, JPEG or WebP (anything
  // else fails with ocr_image.h's reason, e.g. a TIFF's). It is decoded in
  // the data decoder, then OCR'd as OcrBitmapWithTesseract says, within
  // kImageOcrTimeout. The text is capped at |max_chars|. Without tesseract,
  // binary_source is "none". |on_done| runs on the UI thread, never
  // synchronously. UI thread only.
  void OcrImage(std::string image,
                size_t max_chars,
                base::OnceCallback<void(OcrResult)> on_done);

  // OCRs a PDF that has no text layer, given its raw bytes (see the file
  // comment): its first kPdfOcrMaxPages pages are rendered in the sandboxed
  // document parser and each is OCR'd like an image, all within
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

  // The runs OcrImage and OcrPdf have under way.
  internal::OcrRunSlots run_slots_;
};

// The steps of OcrImage and OcrPdf, exposed for tests.
namespace internal {

// Decodes the image file |image| in Chromium's data decoder. |callback|
// gets the bitmap; an empty one if the bytes aren't an image the decoder
// takes, the decoder failed, or |timeout| passed (|timed_out|; the decoder
// process is then let go). It runs on the calling sequence, never
// synchronously. |image| is copied before this returns.
using DecodedImageCallback =
    base::OnceCallback<void(const SkBitmap& bitmap, bool timed_out)>;
void DecodeImageForOcr(base::span<const uint8_t> image,
                       base::TimeDelta timeout,
                       DecodedImageCallback callback);

// Writes |bitmap| (any colour and alpha type) to the new file |path| as the
// PNM ocr_image.h describes, row by row. Empty on success, else why not:
// kTooLargeForOcr, or that it couldn't be read or written. Blocking.
std::string WriteOcrPnm(const SkBitmap& bitmap, const base::FilePath& path);

// OCR of one image file: |image|, its text cut to |max_chars|, decoding and
// tesseract within |timeout|. |done| runs exactly once, on the calling
// sequence, perhaps before this returns.
using ImageOcrFunction =
    base::RepeatingCallback<void(base::span<const uint8_t> image,
                                 size_t max_chars,
                                 base::TimeDelta timeout,
                                 base::OnceCallback<void(OcrResult)> done)>;

// The ImageOcrFunction OcrImage and OcrPdf use: |image| must be a PNG,
// JPEG or WebP (else |done| gets ocr_image.h's reason); it is decoded with
// DecodeImageForOcr, then OcrBitmapWithTesseract runs on a MayBlock task
// with what is left of |timeout|. UI thread.
void OcrImageFile(const base::FilePath& bin,
                  const base::FilePath& tessdata_parent,
                  base::span<const uint8_t> image,
                  size_t max_chars,
                  base::TimeDelta timeout,
                  base::OnceCallback<void(OcrResult)> done);

// OCRs |pages| in order with |ocr_page|, one at a time, and joins the text
// of those that have any as "--- Page N ---\n<text>" (N counts from 1),
// separated by a blank line. A page starts only while at least
// kMinPageOcrTime is left before |deadline| (by |clock|, which must outlive
// this) and gets at most kImageOcrTimeout; a page that fails is skipped.
// When the document has more pages (|page_count|) than were OCR'd, a last
// line says how many were, if it fits. The text is valid UTF-8 of at most
// |max_chars| bytes. Fails, with the last page's error, when no page gave
// text. |on_done| runs on the calling sequence, never synchronously.
void OcrRenderedPages(std::vector<RenderedPdfPage> pages,
                      uint32_t page_count,
                      size_t max_chars,
                      base::TimeTicks deadline,
                      const base::TickClock* clock,
                      ImageOcrFunction ocr_page,
                      base::OnceCallback<void(OcrResult)> on_done);

// OcrPdf without the singleton and with a given page OCR: renders |pdf|
// through |parser| (which copies it before returning), then runs
// OcrRenderedPages. |on_done| runs on the calling sequence, never
// synchronously, unless |parser| is destroyed while rendering. Sets
// success, text and error only.
void RenderAndOcrPdf(DocumentParserClient& parser,
                     base::span<const uint8_t> pdf,
                     size_t max_chars,
                     base::TimeTicks deadline,
                     ImageOcrFunction ocr_page,
                     base::OnceCallback<void(OcrResult)> on_done);

}  // namespace internal

}  // namespace ocr
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_OCR_OCR_SERVICE_H_
