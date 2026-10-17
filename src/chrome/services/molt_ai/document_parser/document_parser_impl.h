// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// DocumentParserImpl: molt_ai.mojom.DocumentParser, the only place
// MoltBrowser parses untrusted documents (audit S5, the "Rule of 2"). It runs
// in a utility process under the most restrictive sandbox
// (sandbox.mojom.Sandbox.kService: no file system, no network), registered in
// //chrome/utility/services.cc and launched, one process per call, by
// //chrome/browser/molt_ai/document_parser/document_parser_client.h:
//   - PDF text layer:        pdf/pdf_text_scraper.h (hand-written C++, zlib)
//   - Office / e-book / CSV: the anydoc Rust crate (anydoc/anydoc_ffi.rs).
//                            Chromium builds Rust with panic=abort, so a
//                            panic kills this process, never the browser.
//   - .docx flat text:       docx_text_extractor.h (zip::ZipReader)
//
// The service enforces its own limits whatever the browser asks for
// (mojom::kMaxDocumentBytes in, mojom::kMaxOutputChars out), and every string
// it returns is valid UTF-8. The browser still validates every reply.
//
// Not in this sandbox: OCR of scans and images runs tesseract as a helper
// process under its own, looser OS-level sandbox and deadline
// (molt_ai/ocr/ocr_service.h, molt_ai/common/helper_sandbox.h). Still
// outside a sandbox (tracked separately): llama.cpp inference runs in the
// browser process (molt_ai/runtime).

#ifndef CHROME_SERVICES_MOLT_AI_DOCUMENT_PARSER_DOCUMENT_PARSER_IMPL_H_
#define CHROME_SERVICES_MOLT_AI_DOCUMENT_PARSER_DOCUMENT_PARSER_IMPL_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "chrome/services/molt_ai/document_parser/public/mojom/document_parser.mojom.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/receiver.h"

namespace molt_ai {

class DocumentParserImpl : public mojom::DocumentParser {
 public:
  // Serves |receiver|. Tests may pass an unbound receiver and call the
  // methods directly.
  explicit DocumentParserImpl(
      mojo::PendingReceiver<mojom::DocumentParser> receiver);
  DocumentParserImpl(const DocumentParserImpl&) = delete;
  DocumentParserImpl& operator=(const DocumentParserImpl&) = delete;
  ~DocumentParserImpl() override;

  // mojom::DocumentParser:
  void ExtractPdfText(mojo_base::BigBuffer pdf,
                      uint32_t max_chars,
                      ExtractPdfTextCallback callback) override;
  void ConvertToMarkdown(mojo_base::BigBuffer document,
                         const std::string& extension,
                         uint32_t max_chars,
                         ConvertToMarkdownCallback callback) override;
  void ExtractDocxText(mojo_base::BigBuffer docx,
                       uint32_t max_chars,
                       ExtractDocxTextCallback callback) override;

 private:
  mojo::Receiver<mojom::DocumentParser> receiver_;
};

namespace document_parser_internal {

// Exposed for tests.
//
// |in| as valid UTF-8 (base::IsStringUTF8): every invalid sequence and every
// non-character becomes U+FFFD. Stops before the character that would take
// the result past |max_bytes|, so a long input is never copied whole.
std::string ToValidUtf8(std::string_view in, size_t max_bytes);

// anydoc's extension hint: 1 to 8 ASCII letters or digits, lowercased;
// anything else becomes "" (anydoc knows only short ASCII extensions, and
// rust::Str aborts the process on bytes that are not UTF-8).
std::string SanitizeExtension(std::string_view extension);

}  // namespace document_parser_internal

}  // namespace molt_ai

#endif  // CHROME_SERVICES_MOLT_AI_DOCUMENT_PARSER_DOCUMENT_PARSER_IMPL_H_
