// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// OcrService — local OCR via bundled tesseract.
//
// Used by HandleExtractPdfText when our cheap PDF text scraper
// returns empty (image-only / scanned PDFs). We feed the raw PDF
// bytes through tesseract directly — modern tesseract handles PDF
// input natively via its embedded Leptonica + Poppler dependency
// (the macOS Homebrew formula bundles Leptonica with PDF support).
//
// Resolution (FindHelperBinary, helper_binary.h — same as VoiceService and
// TorManager):
//   1. the bundled copy: MoltBrowser.app/Contents/Resources/ocr/tesseract on
//      macOS, <install dir>/ocr/tesseract[.exe] on Linux and Windows;
//   2. Linux only: /usr/bin/tesseract, if root owns it and every directory
//      above it.
// No Homebrew, /usr/local, /opt/local or PATH fallback (audit S18).
//
// Threading: blocking subprocess on a MayBlock thread-pool task.

#ifndef CHROME_BROWSER_MOLT_AI_OCR_OCR_SERVICE_H_
#define CHROME_BROWSER_MOLT_AI_OCR_OCR_SERVICE_H_

#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "chrome/browser/molt_ai/common/helper_binary.h"

namespace base {
template <typename T>
class NoDestructor;
}  // namespace base

namespace molt_ai {
namespace ocr {

struct OcrResult {
  OcrResult();
  OcrResult(const OcrResult&);
  OcrResult(OcrResult&&);
  OcrResult& operator=(const OcrResult&);
  OcrResult& operator=(OcrResult&&);
  ~OcrResult();

  bool success = false;
  std::string text;
  std::string error;
  std::string binary_source;  // "bundled" | "system" | "none"
  std::string binary_path;
  int duration_ms = 0;
};

class OcrService {
 public:
  static OcrService* Get();

  OcrService(const OcrService&) = delete;
  OcrService& operator=(const OcrService&) = delete;

  base::FilePath ResolveTesseractBinary() const;
  bool IsUsingBundledTesseract() const;
  base::FilePath GetBundledOcrDir() const;

  // OCR a PDF given its raw bytes. We write a temp file, invoke
  // tesseract with -l eng (extend later for multi-language), read
  // the produced .txt back. Capped at |max_chars| since OCR of
  // 200-page PDFs would tank the chat context budget.
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
};

}  // namespace ocr
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_OCR_OCR_SERVICE_H_
