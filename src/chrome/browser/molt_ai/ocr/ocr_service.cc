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
  // Point tesseract at our bundled traineddata when we're using a
  // bundled binary. We pass --tessdata-dir on the command line rather
  // than the TESSDATA_PREFIX env var: GetAppOutputWithExitCode() does
  // not take LaunchOptions, so an env entry would be silently dropped.
  // |tessdata| is the bundle dir (the *parent* of tessdata/, matching
  // the TESSDATA_PREFIX convention), so the flag gets the tessdata/
  // subdirectory that actually holds eng.traineddata. This is
  // cross-platform; on Windows it resolves <DIR_EXE>\ocr\tessdata.
  base::FilePath out_stem = named.RemoveExtension();
  base::CommandLine cmd(bin);
  cmd.AppendArgPath(named);
  cmd.AppendArgPath(out_stem);
  cmd.AppendArg("-l");
  cmd.AppendArg("eng");
  if (!tessdata.value().empty()) {
    cmd.AppendArg("--tessdata-dir");
    cmd.AppendArgPath(tessdata.AppendASCII("tessdata"));
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
#elif BUILDFLAG(IS_WIN)
  // Windows ships a flat install dir, so the bundle lives next to the
  // executable: <DIR_EXE>\ocr\ (binary tesseract.exe + tessdata\).
  // DIR_EXE is the directory containing the running executable.
  base::FilePath exe_dir;
  if (!base::PathService::Get(base::DIR_EXE, &exe_dir)) return base::FilePath();
  return exe_dir.Append(FILE_PATH_LITERAL("ocr"));
#else
  return base::FilePath();
#endif
}

base::FilePath OcrService::BundledTesseractPath() const {
  base::FilePath dir = GetBundledOcrDir();
  if (dir.value().empty()) return base::FilePath();
#if BUILDFLAG(IS_WIN)
  return dir.Append(FILE_PATH_LITERAL("tesseract.exe"));
#else
  return dir.AppendASCII("tesseract");
#endif
}

base::FilePath OcrService::ResolveTesseractBinary() const {
  // Prefer the binary bundled next to the executable. access()/X_OK are
  // POSIX-only, so existence is checked with base::PathExists and the
  // executable-bit check is guarded to non-Windows platforms.
  base::FilePath bundled = BundledTesseractPath();
  if (!bundled.value().empty() && base::PathExists(bundled)
#if !BUILDFLAG(IS_WIN)
      && access(bundled.value().c_str(), X_OK) == 0
#endif
  ) {
    return bundled;
  }
#if BUILDFLAG(IS_WIN)
  // The POSIX candidate paths (/usr/bin, ...) never exist on Windows.
  // Fall back to a bare "tesseract.exe" and let the OS PATH search in
  // base::LaunchProcess resolve it if the user installed tesseract.
  return base::FilePath(FILE_PATH_LITERAL("tesseract.exe"));
#else
  for (const char* p : kCandidatePaths) {
    base::FilePath fp = base::FilePath::FromUTF8Unsafe(p);
    if (base::PathExists(fp) && access(fp.value().c_str(), X_OK) == 0) {
      return fp;
    }
  }
  return base::FilePath();
#endif
}

bool OcrService::IsUsingBundledTesseract() const {
  base::FilePath bundled = BundledTesseractPath();
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
