// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/ocr/ocr_service.h"

#include "build/build_config.h"

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
#include "chrome/browser/molt_ai/common/helper_binary.h"
#include "chrome/browser/molt_ai/common/helper_output.h"

namespace molt_ai {
namespace ocr {

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
// those are writable without root, and tesseract is fed the user's PDFs.
std::vector<std::string> SystemTesseractPaths() {
  return {"/usr/bin/tesseract"};
}

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
              SanitizeHelperText(combined, 200);
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

  // tesseract read an untrusted PDF: its text need not be UTF-8, and a
  // byte cut could split a character.
  text = SanitizeHelperText(text, max_chars);
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

void OcrService::OcrPdf(std::string pdf_bytes, size_t max_chars,
                         base::OnceCallback<void(OcrResult)> on_done) {
  const molt_ai::HelperBinary found = FindTesseract();
  if (!found.found()) {
    LOG(WARNING) << "[MoltOCR] no usable tesseract: " << found.error;
  }
  const base::FilePath& bin = found.path;
  const bool bundled = found.bundled;
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
