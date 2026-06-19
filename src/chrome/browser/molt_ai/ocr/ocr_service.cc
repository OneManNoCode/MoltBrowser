// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/ocr/ocr_service.h"

#include "build/build_config.h"

#if !BUILDFLAG(IS_WIN)
#include <unistd.h>
#endif

#include <utility>

#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"

namespace molt_ai {
namespace ocr {

OcrResult::OcrResult() = default;
OcrResult::OcrResult(const OcrResult&) = default;
OcrResult::OcrResult(OcrResult&&) = default;
OcrResult& OcrResult::operator=(const OcrResult&) = default;
OcrResult& OcrResult::operator=(OcrResult&&) = default;
OcrResult::~OcrResult() = default;

namespace {

const char* const kCandidatePaths[] = {
    "/opt/homebrew/bin/tesseract",
    "/usr/local/bin/tesseract",
    "/usr/bin/tesseract",
    "/opt/local/bin/tesseract",
};

OcrResult RunTesseractBlocking(std::string pdf_bytes,
                                base::FilePath bin,
                                base::FilePath tessdata,
                                size_t max_chars) {
  OcrResult r;
  r.binary_path = bin.AsUTF8Unsafe();
  const base::TimeTicks t0 = base::TimeTicks::Now();

  if (bin.value().empty()) {
    r.error = "no tesseract binary available";
    return r;
  }
  if (pdf_bytes.empty()) {
    r.error = "empty pdf";
    return r;
  }

  // Private 0700 scratch dir so the PDF + tesseract's .txt output
  // can't be raced by a local-user symlink attack. Code-review
  // MEDIUM #5+#6.
  base::FilePath scratch_dir;
  if (!base::CreateNewTempDirectory(FILE_PATH_LITERAL("molt_ocr"),
                                     &scratch_dir)) {
    r.error = "could not create scratch directory";
    return r;
  }
  base::FilePath named = scratch_dir.AppendASCII("input.pdf");
  if (!base::WriteFile(named, pdf_bytes)) {
    r.error = "could not write temp pdf";
    base::DeletePathRecursively(scratch_dir);
    return r;
  }

  // tesseract <input> <output-stem> [args]
  // Setting TESSDATA_PREFIX env var points it at our bundled
  // traineddata directory when we're using a bundled binary.
  base::FilePath out_stem = named.RemoveExtension();
  base::CommandLine cmd(bin);
  cmd.AppendArgPath(named);
  cmd.AppendArgPath(out_stem);
  cmd.AppendArg("-l");
  cmd.AppendArg("eng");
  base::LaunchOptions opts;
  if (!tessdata.value().empty()) {
    opts.environment["TESSDATA_PREFIX"] = tessdata.value();
  }
  std::string combined;
  int exit_code = -1;
  bool launched = base::GetAppOutputWithExitCode(cmd, &combined, &exit_code);
  if (!launched) {
    r.error = "failed to launch tesseract";
    base::DeletePathRecursively(scratch_dir);
    return r;
  }
  if (exit_code != 0) {
    r.error = "tesseract exit=" + std::to_string(exit_code) + ": " +
              combined.substr(0, 200);
    base::DeletePathRecursively(scratch_dir);
    return r;
  }
  // tesseract writes <stem>.txt
  base::FilePath txt = out_stem.AddExtensionASCII(".txt");
  std::string text;
  if (base::PathExists(txt)) {
    base::ReadFileToString(txt, &text);
  }
  base::DeletePathRecursively(scratch_dir);

  if (text.size() > max_chars) text.resize(max_chars);
  r.success = !text.empty();
  r.text = std::move(text);
  if (!r.success && r.error.empty()) {
    r.error = "OCR returned no text";
  }
  r.duration_ms =
      static_cast<int>((base::TimeTicks::Now() - t0).InMilliseconds());
  return r;
}

}  // namespace

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
  return base::FilePath();
#endif
}

base::FilePath OcrService::ResolveTesseractBinary() const {
  // On Windows there is no bundled binary and the POSIX candidate paths do
  // not exist; access()/X_OK are POSIX-only, so fall back to a plain
  // existence check (OCR is effectively unavailable on Windows here).
  base::FilePath bundled = GetBundledOcrDir().AppendASCII("tesseract");
  if (!bundled.value().empty() && base::PathExists(bundled)
#if !BUILDFLAG(IS_WIN)
      && access(bundled.value().c_str(), X_OK) == 0
#endif
  ) {
    return bundled;
  }
  for (const char* p : kCandidatePaths) {
    base::FilePath fp(p);
    if (base::PathExists(fp)
#if !BUILDFLAG(IS_WIN)
        && access(fp.value().c_str(), X_OK) == 0
#endif
    ) {
      return fp;
    }
  }
  return base::FilePath();
}

bool OcrService::IsUsingBundledTesseract() const {
  base::FilePath bundled = GetBundledOcrDir().AppendASCII("tesseract");
  base::FilePath resolved = ResolveTesseractBinary();
  return !bundled.value().empty() && !resolved.value().empty() &&
         resolved.value() == bundled.value();
}

void OcrService::OcrPdf(std::string pdf_bytes, size_t max_chars,
                         base::OnceCallback<void(OcrResult)> on_done) {
  base::FilePath bin = ResolveTesseractBinary();
  bool bundled = IsUsingBundledTesseract();
  base::FilePath tessdata;
  if (bundled) {
    // bundle-tesseract.sh drops eng.traineddata into
    // Resources/ocr/tessdata/. Tesseract reads TESSDATA_PREFIX as
    // the *parent* of the tessdata dir.
    tessdata = GetBundledOcrDir();
  }
  std::string src =
      bin.value().empty() ? "none" : (bundled ? "bundled" : "system");
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&RunTesseractBlocking, std::move(pdf_bytes), bin,
                     tessdata, max_chars),
      base::BindOnce(
          [](std::string src,
             base::OnceCallback<void(OcrResult)> cb, OcrResult r) {
            r.binary_source = src;
            std::move(cb).Run(std::move(r));
          },
          src, std::move(on_done)));
}

}  // namespace ocr
}  // namespace molt_ai
