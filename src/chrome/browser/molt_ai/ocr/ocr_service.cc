// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/ocr/ocr_service.h"

#include "build/build_config.h"

#include <algorithm>
#include <utility>

#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/time/default_tick_clock.h"
#include "base/time/tick_clock.h"
#include "base/time/time.h"
#include "chrome/browser/molt_ai/common/helper_binary.h"
#include "chrome/browser/molt_ai/common/helper_output.h"
#include "chrome/browser/molt_ai/common/helper_sandbox.h"
#include "chrome/services/molt_ai/document_parser/public/mojom/document_parser.mojom.h"

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

// The input file's extension: 1 to 8 lowercase ASCII letters or digits, or
// "img" (tesseract sniffs the format anyway).
std::string InputExtension(std::string_view extension) {
  if (extension.empty() || extension.size() > 8 ||
      !std::all_of(extension.begin(), extension.end(), [](char c) {
        return base::IsAsciiLower(c) || base::IsAsciiDigit(c);
      })) {
    return "img";
  }
  return std::string(extension);
}

// tesseract on one rendered page (a PNG).
OcrResult OcrPagePng(const base::FilePath& bin,
                     const base::FilePath& tessdata_parent,
                     base::span<const uint8_t> png,
                     size_t max_chars,
                     base::TimeDelta timeout) {
  return OcrImageWithTesseract(bin, tessdata_parent, png, "png", max_chars,
                               timeout);
}

OcrResult OcrRenderedPagesNow(std::vector<RenderedPdfPage> pages,
                              uint32_t page_count,
                              size_t max_chars,
                              base::TimeTicks deadline,
                              internal::PageOcrFunction ocr_page) {
  return internal::OcrRenderedPages(pages, page_count, max_chars, deadline,
                                    base::DefaultTickClock::GetInstance(),
                                    ocr_page);
}

void OnPdfPagesRendered(size_t max_chars,
                        base::TimeTicks deadline,
                        internal::PageOcrFunction ocr_page,
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
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&OcrRenderedPagesNow, std::move(rendered.pages),
                     rendered.page_count, max_chars, deadline,
                     std::move(ocr_page)),
      std::move(on_done));
}

}  // namespace

HelperRunResult RunSandboxedTesseract(const base::FilePath& bin,
                                      const base::FilePath& tessdata_parent,
                                      const base::FilePath& scratch_dir,
                                      const base::FilePath& input,
                                      const base::FilePath& out_stem,
                                      base::TimeDelta timeout) {
  base::CommandLine cmd(bin);
  cmd.AppendArgPath(input);
  cmd.AppendArgPath(out_stem);
  cmd.AppendArg("-l");
  cmd.AppendArg("eng");
  HelperSandboxPolicy policy;
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
  policy.writable_dir = scratch_dir;
  return RunSandboxedHelper(cmd, policy, timeout, kMaxTesseractOutputBytes);
}

OcrResult OcrImageWithTesseract(const base::FilePath& bin,
                                const base::FilePath& tessdata_parent,
                                base::span<const uint8_t> image,
                                std::string_view extension,
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
  if (image.empty()) {
    r.error = "empty image";
    return finish();
  }

  // A private 0700 scratch directory per run, so the input and tesseract's
  // .txt output can't be raced by a local-user symlink attack (code review
  // MEDIUM #5+#6), and one run can't touch another's files.
  base::FilePath scratch_dir;
  if (!base::CreateNewTempDirectory(FILE_PATH_LITERAL("molt_ocr"),
                                    &scratch_dir)) {
    r.error = "could not create scratch directory";
    return finish();
  }
  const base::FilePath input =
      scratch_dir.AppendASCII("input." + InputExtension(extension));
  if (!base::WriteFile(input, image)) {
    base::DeletePathRecursively(scratch_dir);
    r.error = "could not write temp image";
    return finish();
  }

  // tesseract <input> <output-stem> [args], sandboxed, with a deadline.
  const base::FilePath out_stem = input.RemoveExtension();
  const HelperRunResult run = RunSandboxedTesseract(
      bin, tessdata_parent, scratch_dir, input, out_stem, timeout);
  const std::string failure =
      DescribeHelperRunFailure(run, "tesseract", "failed to launch tesseract");
  if (!failure.empty()) {
    base::DeletePathRecursively(scratch_dir);
    r.error = failure;
    return finish();
  }
  // tesseract writes <stem>.txt (read only if it is a plain file it made).
  std::string text;
  ReadHelperOutputFile(out_stem.AddExtensionASCII(".txt"),
                       kMaxTesseractTextFileBytes, &text);
  base::DeletePathRecursively(scratch_dir);

  // tesseract ends each page with a form feed; a page without text is just
  // that.
  if (text.find_first_not_of(" \t\r\n\f\v") == std::string::npos) {
    r.error = "OCR returned no text";
    return finish();
  }
  // tesseract read an untrusted file: its text need not be UTF-8, and a
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
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(std::move(on_done), std::move(result)));
    return;
  }
  // bundle-tesseract.sh drops eng.traineddata into Resources/ocr/tessdata/;
  // tesseract takes the *parent* of the tessdata dir (TESSDATA_PREFIX).
  const base::FilePath tessdata =
      found.bundled ? GetBundledOcrDir() : base::FilePath();
  if (!document_parser_) {
    document_parser_ = std::make_unique<DocumentParserClient>();
  }
  internal::RenderAndOcrPdf(
      *document_parser_, base::as_byte_span(pdf_bytes), max_chars, deadline,
      base::BindRepeating(&OcrPagePng, found.path, tessdata),
      base::BindOnce(
          [](std::string source, std::string path, base::TimeTicks t0,
             base::OnceCallback<void(OcrResult)> cb, OcrResult r) {
            r.binary_source = std::move(source);
            r.binary_path = std::move(path);
            r.duration_ms = static_cast<int>(
                (base::TimeTicks::Now() - t0).InMilliseconds());
            std::move(cb).Run(std::move(r));
          },
          std::string(found.bundled ? "bundled" : "system"),
          found.path.AsUTF8Unsafe(), t0, std::move(on_done)));
}

namespace internal {

OcrResult OcrRenderedPages(const std::vector<RenderedPdfPage>& pages,
                           uint32_t page_count,
                           size_t max_chars,
                           base::TimeTicks deadline,
                           const base::TickClock* clock,
                           const PageOcrFunction& ocr_page) {
  std::string text;
  std::string last_error;
  bool out_of_time = false;
  size_t pages_read = 0;
  for (const RenderedPdfPage& page : pages) {
    if (text.size() >= max_chars) {
      break;
    }
    const base::TimeDelta left = deadline - clock->NowTicks();
    if (left < kMinPageOcrTime) {
      out_of_time = true;
      break;
    }
    const OcrResult page_result =
        ocr_page.Run(page.png, max_chars, std::min(kImageOcrTimeout, left));
    ++pages_read;
    if (!page_result.success) {
      last_error = page_result.error;
      continue;
    }
    const std::string_view page_text =
        base::TrimWhitespaceASCII(page_result.text, base::TRIM_ALL);
    if (page_text.empty()) {
      continue;
    }
    if (!text.empty()) {
      text += "\n\n";
    }
    text += "--- Page " + base::NumberToString(page.page_index + 1u) +
            " ---\n";
    text += page_text;
  }

  OcrResult result;
  if (text.empty()) {
    result.error = !last_error.empty() ? last_error
                   : out_of_time       ? "OCR ran out of time"
                                       : "OCR returned no text";
    return result;
  }
  if (pages_read < page_count) {
    // Tell the reader (and the model) that the document goes on.
    const std::string note = "\n\n--- OCR read " +
                             base::NumberToString(pages_read) + " of " +
                             base::NumberToString(page_count) + " pages ---";
    if (text.size() + note.size() <= max_chars) {
      text += note;
    }
  }
  // Each page's text was sanitized already; this makes the cut safe.
  result.text = SanitizeHelperText(text, max_chars);
  result.success = !result.text.empty();
  if (!result.success) {
    result.error = "OCR returned no text";
  }
  return result;
}

void RenderAndOcrPdf(DocumentParserClient& parser,
                     base::span<const uint8_t> pdf,
                     size_t max_chars,
                     base::TimeTicks deadline,
                     PageOcrFunction ocr_page,
                     base::OnceCallback<void(OcrResult)> on_done) {
  parser.RenderPdfPages(
      pdf, kPdfOcrMaxPages, kPdfOcrDpi,
      base::BindOnce(&OnPdfPagesRendered, max_chars, deadline,
                     std::move(ocr_page), std::move(on_done)));
}

}  // namespace internal

}  // namespace ocr
}  // namespace molt_ai
