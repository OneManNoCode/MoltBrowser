// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/ocr/ocr_service.h"

#include <stdint.h>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/string_view_util.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/time/default_tick_clock.h"
#include "base/time/tick_clock.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "build/build_config.h"
#include "chrome/browser/molt_ai/common/helper_binary.h"
#include "chrome/browser/molt_ai/common/helper_output.h"
#include "chrome/browser/molt_ai/common/helper_sandbox.h"
#include "chrome/browser/molt_ai/ocr/ocr_image.h"
#include "chrome/services/molt_ai/document_parser/public/mojom/document_parser.mojom.h"
#include "services/data_decoder/public/cpp/data_decoder.h"
#include "services/data_decoder/public/cpp/decode_image.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "ui/gfx/geometry/size.h"

namespace molt_ai {
namespace ocr {

static_assert(kPdfOcrMaxPages == mojom::kMaxRenderPages);
static_assert(kPdfOcrDpi == mojom::kMaxRenderDpi);
static_assert(DocumentParserClient::kRenderTimeout + kMinPageOcrTime <
              kPdfOcrTimeout);

OcrResult::OcrResult() = default;
OcrResult::OcrResult(const OcrResult&) = default;
OcrResult::OcrResult(OcrResult&&) = default;
OcrResult& OcrResult::operator=(const OcrResult&) = default;
OcrResult& OcrResult::operator=(OcrResult&&) = default;
OcrResult::~OcrResult() = default;

namespace {

// Linux only: the distribution's tesseract-ocr package, accepted only if root
// owns it and every directory above it (helper_binary.h). There is no
// Homebrew / /usr/local / /opt/local / PATH fallback any more (audit S18):
// those are writable without root, and tesseract is fed the user's files.
std::vector<std::string> SystemTesseractPaths() {
  return {"/usr/bin/tesseract"};
}

OcrResult Failure(std::string error) {
  OcrResult result;
  result.error = std::move(error);
  return result;
}

// |result| to |done|, from a task of its own (never synchronously).
void ReplyLater(base::OnceCallback<void(OcrResult)> done, OcrResult result) {
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(std::move(done), std::move(result)));
}

// Adds which tesseract ran, and how long the whole call took.
void AddBinaryDetails(std::string source,
                      std::string path,
                      base::TimeTicks t0,
                      base::OnceCallback<void(OcrResult)> on_done,
                      OcrResult result) {
  result.binary_source = std::move(source);
  result.binary_path = std::move(path);
  result.duration_ms =
      static_cast<int>((base::TimeTicks::Now() - t0).InMilliseconds());
  std::move(on_done).Run(std::move(result));
}

// One image file in the data decoder, with a deadline. It owns itself from
// Start() until it answers, through its decoder's reply or its timer,
// whichever comes first; then it goes, and with it its decoder (which ends
// the decoder process and drops a decode still running) and its timer.
class OcrImageDecode {
 public:
  OcrImageDecode(const OcrImageDecode&) = delete;
  OcrImageDecode& operator=(const OcrImageDecode&) = delete;
  ~OcrImageDecode() = default;

  static void Start(base::span<const uint8_t> image,
                    base::TimeDelta timeout,
                    internal::DecodedImageCallback callback) {
    auto* decode = new OcrImageDecode(std::move(callback));
    decode->timer_.Start(FROM_HERE, timeout,
                         base::BindOnce(&OcrImageDecode::Finish,
                                        base::Unretained(decode), SkBitmap(),
                                        /*timed_out=*/true));
    // shrink_to_fit: an image past the size limit is scaled down while it
    // is decoded, so a huge one never reaches the browser at full size.
    data_decoder::DecodeImage(
        &decode->decoder_, image, data_decoder::mojom::ImageCodec::kDefault,
        /*shrink_to_fit=*/true, data_decoder::kDefaultMaxSizeInBytes,
        gfx::Size(),
        base::BindOnce(&OcrImageDecode::OnDecoded,
                       decode->weak_factory_.GetWeakPtr()));
  }

 private:
  explicit OcrImageDecode(internal::DecodedImageCallback callback)
      : callback_(std::move(callback)) {}

  void OnDecoded(const SkBitmap& bitmap) {
    Finish(bitmap, /*timed_out=*/false);
  }

  void Finish(SkBitmap bitmap, bool timed_out) {
    std::unique_ptr<OcrImageDecode> self(this);
    std::move(callback_).Run(bitmap, timed_out);
  }

  data_decoder::DataDecoder decoder_;
  base::OneShotTimer timer_;
  internal::DecodedImageCallback callback_;
  base::WeakPtrFactory<OcrImageDecode> weak_factory_{this};
};

void OnImageDecoded(const base::FilePath& bin,
                    const base::FilePath& tessdata_parent,
                    size_t max_chars,
                    base::TimeTicks deadline,
                    base::OnceCallback<void(OcrResult)> done,
                    const SkBitmap& bitmap,
                    bool timed_out) {
  if (timed_out) {
    std::move(done).Run(Failure("decoding the image timed out"));
    return;
  }
  if (bitmap.drawsNothing()) {
    std::move(done).Run(Failure("couldn't decode this image"));
    return;
  }
  const base::TimeDelta left = deadline - base::TimeTicks::Now();
  if (left < base::Seconds(1)) {
    std::move(done).Run(Failure("OCR ran out of time"));
    return;
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&OcrBitmapWithTesseract, bin, tessdata_parent, bitmap,
                     max_chars, left),
      std::move(done));
}

// One PDF's page OCR, passed from step to step (OcrRenderedPages).
struct PagesOcr {
  std::vector<RenderedPdfPage> pages;
  uint32_t page_count = 0;
  size_t max_chars = 0;
  base::TimeTicks deadline;
  raw_ptr<const base::TickClock> clock = nullptr;
  internal::ImageOcrFunction ocr_page;
  base::OnceCallback<void(OcrResult)> on_done;

  // The next page to OCR, the pages tried, the text so far.
  size_t next = 0;
  size_t pages_read = 0;
  std::string text;
  std::string last_error;
  bool out_of_time = false;
};

void FinishPages(std::unique_ptr<PagesOcr> state) {
  OcrResult result;
  if (state->text.empty()) {
    result.error = !state->last_error.empty() ? state->last_error
                   : state->out_of_time       ? "OCR ran out of time"
                                              : "OCR returned no text";
  } else {
    std::string text = std::move(state->text);
    if (state->pages_read < state->page_count) {
      // Tell the reader (and the model) that the document goes on.
      const std::string note =
          "\n\n--- OCR read " + base::NumberToString(state->pages_read) +
          " of " + base::NumberToString(state->page_count) + " pages ---";
      if (text.size() + note.size() <= state->max_chars) {
        text += note;
      }
    }
    // Each page's text was sanitized already; this makes the cut safe.
    result.text = SanitizeHelperText(text, state->max_chars);
    result.success = !result.text.empty();
    if (!result.success) {
      result.error = "OCR returned no text";
    }
  }
  std::move(state->on_done).Run(std::move(result));
}

void OcrNextPage(std::unique_ptr<PagesOcr> state);

void OnPageOcred(std::unique_ptr<PagesOcr> state, OcrResult page_result) {
  const uint32_t page_index = state->pages[state->next].page_index;
  ++state->next;
  ++state->pages_read;
  if (!page_result.success) {
    state->last_error = page_result.error;
  } else {
    const std::string_view page_text =
        base::TrimWhitespaceASCII(page_result.text, base::TRIM_ALL);
    if (!page_text.empty()) {
      if (!state->text.empty()) {
        state->text += "\n\n";
      }
      state->text +=
          "--- Page " + base::NumberToString(page_index + 1u) + " ---\n";
      state->text += page_text;
    }
  }
  // Not from inside the page OCR, which may have answered synchronously.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&OcrNextPage, std::move(state)));
}

void OcrNextPage(std::unique_ptr<PagesOcr> state) {
  if (state->next >= state->pages.size() ||
      state->text.size() >= state->max_chars) {
    FinishPages(std::move(state));
    return;
  }
  const base::TimeDelta left = state->deadline - state->clock->NowTicks();
  if (left < kMinPageOcrTime) {
    state->out_of_time = true;
    FinishPages(std::move(state));
    return;
  }
  // The page's bytes stay where they are while |state| moves into the
  // callback; the decoder copies them before ocr_page returns.
  PagesOcr* raw = state.get();
  const base::span<const uint8_t> image = raw->pages[raw->next].png;
  raw->ocr_page.Run(image, raw->max_chars, std::min(kImageOcrTimeout, left),
                    base::BindOnce(&OnPageOcred, std::move(state)));
}

void OnPdfPagesRendered(size_t max_chars,
                        base::TimeTicks deadline,
                        internal::ImageOcrFunction ocr_page,
                        base::OnceCallback<void(OcrResult)> on_done,
                        PdfPagesResult rendered) {
  if (rendered.status != PdfPagesResult::Status::kOk) {
    OcrResult result;
    if (rendered.status == PdfPagesResult::Status::kUnreadable) {
      // The parser's own diagnostic, e.g. "the PDF is password-protected".
      result.error = rendered.error.empty() ? "its pages couldn't be rendered"
                                            : rendered.error;
    } else {
      result.error = "rendering its pages failed: " + rendered.error;
    }
    std::move(on_done).Run(std::move(result));
    return;
  }
  internal::OcrRenderedPages(std::move(rendered.pages), rendered.page_count,
                             max_chars, deadline,
                             base::DefaultTickClock::GetInstance(),
                             std::move(ocr_page), std::move(on_done));
}

}  // namespace

HelperRunResult RunSandboxedTesseract(const base::FilePath& bin,
                                      const base::FilePath& tessdata_parent,
                                      const HelperScratchDir& scratch,
                                      const base::FilePath& input,
                                      const base::FilePath& out_stem,
                                      base::TimeDelta timeout) {
  base::CommandLine cmd(bin);
  cmd.AppendArgPath(input);
  cmd.AppendArgPath(out_stem);
  cmd.AppendArg("-l");
  cmd.AppendArg("eng");
  HelperSandboxPolicy policy = scratch.MakePolicy();
  // Its own directory: bundled libraries (and data, for a bundled copy).
  policy.read_only_paths.push_back(bin.DirName());
  if (!tessdata_parent.empty()) {
    // Point tesseract at our bundled traineddata. |tessdata_parent| is the
    // bundle dir (the *parent* of tessdata/, matching the TESSDATA_PREFIX
    // convention), so the flag gets the tessdata/ subdirectory that holds
    // eng.traineddata. On Windows it resolves <DIR_EXE>\ocr\tessdata.
    const base::FilePath tessdata = tessdata_parent.AppendASCII("tessdata");
    cmd.AppendArg("--tessdata-dir");
    cmd.AppendArgPath(tessdata);
    policy.read_only_paths.push_back(tessdata);
  }
  policy.read_only_paths.push_back(input);
  return RunSandboxedHelper(cmd, policy, timeout, kMaxTesseractOutputBytes);
}

OcrResult OcrBitmapWithTesseract(const base::FilePath& bin,
                                 const base::FilePath& tessdata_parent,
                                 const SkBitmap& bitmap,
                                 size_t max_chars,
                                 base::TimeDelta timeout) {
  OcrResult r;
  r.binary_path = bin.AsUTF8Unsafe();
  const base::TimeTicks t0 = base::TimeTicks::Now();
  const auto finish = [&r, t0]() {
    r.duration_ms =
        static_cast<int>((base::TimeTicks::Now() - t0).InMilliseconds());
    return std::move(r);
  };

  if (bin.value().empty()) {
    r.error = "no tesseract binary available";
    return finish();
  }

  // A private scratch directory per run, so the input and tesseract's .txt
  // output can't be raced by a local-user symlink attack (code review
  // MEDIUM #5+#6), and one run can't touch another's files. Deleted when
  // this returns.
  HelperScratchDir scratch;
  if (!scratch.Create("molt_ocr")) {
    r.error = "could not create scratch directory";
    return finish();
  }
  // The only image file tesseract sees: the pixels, as the browser writes
  // them (ocr_image.h).
  const base::FilePath input = scratch.path().AppendASCII("input.pnm");
  const std::string not_written = internal::WriteOcrPnm(bitmap, input);
  if (!not_written.empty()) {
    r.error = not_written;
    return finish();
  }

  // tesseract <input> <output-stem> [args], sandboxed, with a deadline.
  const base::FilePath out_stem = input.RemoveExtension();
  const HelperRunResult run = RunSandboxedTesseract(
      bin, tessdata_parent, scratch, input, out_stem, timeout);
  const std::string failure =
      DescribeHelperRunFailure(run, "tesseract", "failed to launch tesseract");
  if (!failure.empty()) {
    r.error = failure;
    return finish();
  }
  // tesseract writes <stem>.txt (read only if it is a plain file it made).
  std::string text;
  ReadHelperOutputFile(out_stem.AddExtensionASCII(".txt"),
                       kMaxTesseractTextFileBytes, &text);

  // tesseract ends each page with a form feed; a page without text is just
  // that.
  if (text.find_first_not_of(" \t\r\n\f\v") == std::string::npos) {
    r.error = "OCR returned no text";
    return finish();
  }
  // tesseract read an untrusted image: its text need not be UTF-8, and a
  // byte cut could split a character.
  r.text = SanitizeHelperText(text, max_chars);
  r.success = !r.text.empty();
  if (!r.success) {
    r.error = "OCR returned no text";
  }
  return finish();
}

// static
OcrService* OcrService::Get() {
  static base::NoDestructor<OcrService> instance;
  return instance.get();
}

OcrService::OcrService() = default;
OcrService::~OcrService() = default;

base::FilePath OcrService::GetBundledOcrDir() const {
#if BUILDFLAG(IS_MAC)
  base::FilePath exe;
  if (!base::PathService::Get(base::FILE_EXE, &exe)) return base::FilePath();
  return exe.DirName().DirName().AppendASCII("Resources").AppendASCII("ocr");
#else
  // Windows and Linux ship a flat install dir, so the bundle lives next to
  // the executable: <DIR_EXE>/ocr/ (tesseract[.exe] + tessdata/). DIR_EXE is
  // the directory containing the running executable.
  base::FilePath exe_dir;
  if (!base::PathService::Get(base::DIR_EXE, &exe_dir)) return base::FilePath();
  return exe_dir.Append(FILE_PATH_LITERAL("ocr"));
#endif
}

molt_ai::HelperBinary OcrService::FindTesseract() const {
#if BUILDFLAG(IS_WIN)
  const base::FilePath name(FILE_PATH_LITERAL("tesseract.exe"));
#else
  const base::FilePath name(FILE_PATH_LITERAL("tesseract"));
#endif
  return molt_ai::FindHelperBinary(GetBundledOcrDir(), {name},
                                   SystemTesseractPaths());
}

base::FilePath OcrService::ResolveTesseractBinary() const {
  return FindTesseract().path;
}

bool OcrService::IsUsingBundledTesseract() const {
  return FindTesseract().bundled;
}

void OcrService::OcrImage(std::string image,
                          size_t max_chars,
                          base::OnceCallback<void(OcrResult)> on_done) {
  const base::TimeTicks t0 = base::TimeTicks::Now();
  // A few stat() calls; allowed on the UI thread (helper_binary.h).
  const molt_ai::HelperBinary found = FindTesseract();
  if (!found.found()) {
    LOG(WARNING) << "[MoltOCR] no usable tesseract: " << found.error;
    OcrResult result;
    result.binary_source = "none";
    result.error = "no tesseract binary available";
    ReplyLater(std::move(on_done), std::move(result));
    return;
  }
  if (!run_slots_.TryTake()) {
    OcrResult result;
    result.error = std::string(internal::kOcrBusyError);
    ReplyLater(std::move(on_done), std::move(result));
    return;
  }
  on_done = run_slots_.ReleaseAfter(std::move(on_done));
  // bundle-tesseract.sh drops eng.traineddata into Resources/ocr/tessdata/;
  // tesseract takes the *parent* of the tessdata dir (TESSDATA_PREFIX).
  const base::FilePath tessdata =
      found.bundled ? GetBundledOcrDir() : base::FilePath();
  // The decoder copies |image| before this returns.
  internal::OcrImageFile(
      found.path, tessdata, base::as_byte_span(image), max_chars,
      kImageOcrTimeout,
      base::BindOnce(&AddBinaryDetails,
                     std::string(found.bundled ? "bundled" : "system"),
                     found.path.AsUTF8Unsafe(), t0, std::move(on_done)));
}

void OcrService::OcrPdf(std::string pdf_bytes,
                        size_t max_chars,
                        base::OnceCallback<void(OcrResult)> on_done) {
  const base::TimeTicks t0 = base::TimeTicks::Now();
  const base::TimeTicks deadline = t0 + kPdfOcrTimeout;
  // A few stat() calls; allowed on the UI thread (helper_binary.h).
  const molt_ai::HelperBinary found = FindTesseract();
  if (!found.found()) {
    LOG(WARNING) << "[MoltOCR] no usable tesseract: " << found.error;
    // Without tesseract there is no point rendering the pages.
    OcrResult result;
    result.binary_source = "none";
    result.error = "no tesseract binary available";
    ReplyLater(std::move(on_done), std::move(result));
    return;
  }
  if (!run_slots_.TryTake()) {
    OcrResult result;
    result.error = std::string(internal::kOcrBusyError);
    ReplyLater(std::move(on_done), std::move(result));
    return;
  }
  on_done = run_slots_.ReleaseAfter(std::move(on_done));
  const base::FilePath tessdata =
      found.bundled ? GetBundledOcrDir() : base::FilePath();
  if (!document_parser_) {
    document_parser_ = std::make_unique<DocumentParserClient>();
  }
  internal::RenderAndOcrPdf(
      *document_parser_, base::as_byte_span(pdf_bytes), max_chars, deadline,
      base::BindRepeating(&internal::OcrImageFile, found.path, tessdata),
      base::BindOnce(&AddBinaryDetails,
                     std::string(found.bundled ? "bundled" : "system"),
                     found.path.AsUTF8Unsafe(), t0, std::move(on_done)));
}

namespace internal {

OcrRunSlots::OcrRunSlots() = default;
OcrRunSlots::~OcrRunSlots() = default;

bool OcrRunSlots::TryTake() {
  size_t in_flight = in_flight_.load();
  do {
    if (in_flight >= kMaxOcrRunsInFlight) {
      return false;
    }
  } while (!in_flight_.compare_exchange_weak(in_flight, in_flight + 1));
  return true;
}

base::OnceCallback<void(OcrResult)> OcrRunSlots::ReleaseAfter(
    base::OnceCallback<void(OcrResult)> done) {
  // The runner gives the slot back after |done| has run, or when the
  // wrapper is destroyed unrun.
  return base::BindOnce(
      [](base::ScopedClosureRunner /*release*/,
         base::OnceCallback<void(OcrResult)> done, OcrResult result) {
        std::move(done).Run(std::move(result));
      },
      base::ScopedClosureRunner(
          base::BindOnce(&OcrRunSlots::Release, base::Unretained(this))),
      std::move(done));
}

void OcrRunSlots::Release() {
  in_flight_.fetch_sub(1);
}

void DecodeImageForOcr(base::span<const uint8_t> image,
                       base::TimeDelta timeout,
                       DecodedImageCallback callback) {
  OcrImageDecode::Start(image, timeout, std::move(callback));
}

std::string WriteOcrPnm(const SkBitmap& bitmap, const base::FilePath& path) {
  if (bitmap.drawsNothing() || bitmap.width() <= 0 || bitmap.height() <= 0) {
    return "couldn't decode this image";
  }
  const uint32_t width = static_cast<uint32_t>(bitmap.width());
  const uint32_t height = static_cast<uint32_t>(bitmap.height());
  if (OcrPnmHeader(width, height, /*gray=*/true).empty()) {
    return std::string(kTooLargeForOcr);
  }
  // A row at a time as RGBA with straight alpha, whatever the bitmap holds.
  const SkImageInfo row_info = SkImageInfo::Make(
      bitmap.width(), 1, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
  std::string row(row_info.minRowBytes(), '\0');
  const auto read_row = [&bitmap, &row_info, &row](int y) {
    return bitmap.readPixels(row_info, row.data(), row.size(), 0, y);
  };
  // Gray (P5) when every pixel is, as leptonica would read a gray PNG.
  bool gray = true;
  for (int y = 0; y < bitmap.height() && gray; ++y) {
    if (!read_row(y)) {
      return "couldn't decode this image";
    }
    gray = IsGrayRow(row);
  }
  base::File file(path, base::File::FLAG_CREATE | base::File::FLAG_WRITE);
  const std::string header = OcrPnmHeader(width, height, gray);
  if (!file.IsValid() ||
      !file.WriteAtCurrentPosAndCheck(base::as_byte_span(header))) {
    return "could not write temp image";
  }
  std::string pixels;
  for (int y = 0; y < bitmap.height(); ++y) {
    if (!read_row(y)) {
      return "couldn't decode this image";
    }
    pixels.clear();
    AppendOcrPnmRow(row, gray, &pixels);
    if (!file.WriteAtCurrentPosAndCheck(base::as_byte_span(pixels))) {
      return "could not write temp image";
    }
  }
  return std::string();
}

void OcrImageFile(const base::FilePath& bin,
                  const base::FilePath& tessdata_parent,
                  base::span<const uint8_t> image,
                  size_t max_chars,
                  base::TimeDelta timeout,
                  base::OnceCallback<void(OcrResult)> done) {
  // Only what the chat takes reaches the decoder (and nothing reaches
  // tesseract but what the browser writes).
  const std::string_view unreadable =
      DescribeUnreadableImage(SniffOcrImageFormat(base::as_string_view(image)));
  if (!unreadable.empty()) {
    ReplyLater(std::move(done), Failure(std::string(unreadable)));
    return;
  }
  DecodeImageForOcr(
      image, timeout,
      base::BindOnce(&OnImageDecoded, bin, tessdata_parent, max_chars,
                     base::TimeTicks::Now() + timeout, std::move(done)));
}

void OcrRenderedPages(std::vector<RenderedPdfPage> pages,
                      uint32_t page_count,
                      size_t max_chars,
                      base::TimeTicks deadline,
                      const base::TickClock* clock,
                      ImageOcrFunction ocr_page,
                      base::OnceCallback<void(OcrResult)> on_done) {
  auto state = std::make_unique<PagesOcr>();
  state->pages = std::move(pages);
  state->page_count = page_count;
  state->max_chars = max_chars;
  state->deadline = deadline;
  state->clock = clock;
  state->ocr_page = std::move(ocr_page);
  state->on_done = std::move(on_done);
  // From a task of its own, as every later step.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&OcrNextPage, std::move(state)));
}

void RenderAndOcrPdf(DocumentParserClient& parser,
                     base::span<const uint8_t> pdf,
                     size_t max_chars,
                     base::TimeTicks deadline,
                     ImageOcrFunction ocr_page,
                     base::OnceCallback<void(OcrResult)> on_done) {
  parser.RenderPdfPages(
      pdf, kPdfOcrMaxPages, kPdfOcrDpi,
      base::BindOnce(&OnPdfPagesRendered, max_chars, deadline,
                     std::move(ocr_page), std::move(on_done)));
}

}  // namespace internal

}  // namespace ocr
}  // namespace molt_ai
