// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/services/molt_ai/document_parser/document_parser_impl.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/strings/string_util.h"
#include "base/strings/string_view_util.h"
#include "base/strings/utf_string_conversion_utils.h"
#include "base/third_party/icu/icu_utf.h"
#include "chrome/services/molt_ai/document_parser/anydoc/anydoc_ffi.rs.h"
#include "chrome/services/molt_ai/document_parser/docx_text_extractor.h"
#include "chrome/services/molt_ai/document_parser/pdf/pdf_text_scraper.h"
#include "third_party/rust/cxx/v1/cxx.h"

namespace molt_ai {

namespace document_parser_internal {

std::string ToValidUtf8(std::string_view in, size_t max_bytes) {
  static constexpr std::string_view kReplacementCharacter = "\xEF\xBF\xBD";
  std::string out;
  out.reserve(std::min(in.size(), max_bytes));
  for (size_t i = 0; i < in.size(); ++i) {
    const size_t start = i;
    base_icu::UChar32 code_point;
    // On return |i| is the last byte read, valid or not.
    const bool valid = base::ReadUnicodeCharacter(in, &i, &code_point) &&
                       base::IsValidCharacter(code_point);
    const std::string_view piece =
        valid ? in.substr(start, i - start + 1) : kReplacementCharacter;
    if (out.size() + piece.size() > max_bytes) {
      break;
    }
    out.append(piece);
  }
  return out;
}

std::string SanitizeExtension(std::string_view extension) {
  if (extension.empty() || extension.size() > 8 ||
      !std::all_of(extension.begin(), extension.end(),
                   [](char c) { return base::IsAsciiAlphaNumeric(c); })) {
    return std::string();
  }
  return base::ToLowerASCII(extension);
}

}  // namespace document_parser_internal

namespace {

using document_parser_internal::ToValidUtf8;

mojom::ParseResultPtr Failure(std::string_view error) {
  mojom::ParseResultPtr result = mojom::ParseResult::New();
  result->success = false;
  result->error = ToValidUtf8(error, mojom::kMaxErrorBytes);
  return result;
}

// |raw| as a reply: valid UTF-8 of at most |max_chars| bytes (clamped to
// mojom::kMaxOutputChars). Text that ends up empty is a failure with
// |empty_error|.
mojom::ParseResultPtr TextResult(std::string_view raw,
                                 uint32_t max_chars,
                                 std::string_view empty_error) {
  std::string text =
      ToValidUtf8(raw, std::min<size_t>(max_chars, mojom::kMaxOutputChars));
  if (text.empty()) {
    return Failure(empty_error);
  }
  mojom::ParseResultPtr result = mojom::ParseResult::New();
  result->success = true;
  result->text = std::move(text);
  return result;
}

// A refusal for what no parser should see, or null to go ahead. A buffer the
// service could not map (BigBuffer's invalid state) reads as empty.
mojom::ParseResultPtr CheckDocument(const mojo_base::BigBuffer& document) {
  if (document.size() == 0) {
    return Failure("the document is empty");
  }
  if (document.size() > mojom::kMaxDocumentBytes) {
    return Failure("the document is too large to read (over 32 MiB)");
  }
  return nullptr;
}

}  // namespace

DocumentParserImpl::DocumentParserImpl(
    mojo::PendingReceiver<mojom::DocumentParser> receiver)
    : receiver_(this, std::move(receiver)) {}

DocumentParserImpl::~DocumentParserImpl() = default;

void DocumentParserImpl::ExtractPdfText(mojo_base::BigBuffer pdf,
                                        uint32_t max_chars,
                                        ExtractPdfTextCallback callback) {
  if (mojom::ParseResultPtr refusal = CheckDocument(pdf)) {
    std::move(callback).Run(std::move(refusal));
    return;
  }
  const base::span<const uint8_t> view(pdf);
  const std::vector<uint8_t> bytes(view.begin(), view.end());
  // The scraper keeps its own inflate caps (pdf_text_scraper.h) and stops at
  // max_chars; it returns raw glyph bytes, which TextResult makes UTF-8.
  const std::string text = pdf::ExtractText(
      bytes, std::min<size_t>(max_chars, mojom::kMaxOutputChars));
  std::move(callback).Run(
      TextResult(text, max_chars, "the PDF has no readable text layer"));
}

void DocumentParserImpl::ConvertToMarkdown(mojo_base::BigBuffer document,
                                           const std::string& extension,
                                           uint32_t max_chars,
                                           ConvertToMarkdownCallback callback) {
  if (mojom::ParseResultPtr refusal = CheckDocument(document)) {
    std::move(callback).Run(std::move(refusal));
    return;
  }
  const base::span<const uint8_t> view(document);
  const std::string ext =
      document_parser_internal::SanitizeExtension(extension);
  const anydoc_ffi::ConvertResult converted = anydoc_ffi::convert_to_markdown(
      rust::Slice<const uint8_t>(view.data(), view.size()), rust::Str(ext));
  if (!converted.ok) {
    std::move(callback).Run(Failure(
        std::string_view(converted.error.data(), converted.error.size())));
    return;
  }
  // Rust strings are UTF-8 already; TextResult still drops non-characters
  // and cuts at max_chars without copying the whole document.
  std::move(callback).Run(TextResult(
      std::string_view(converted.markdown.data(), converted.markdown.size()),
      max_chars, /*empty_error=*/""));
}

void DocumentParserImpl::ExtractDocxText(mojo_base::BigBuffer docx,
                                         uint32_t max_chars,
                                         ExtractDocxTextCallback callback) {
  if (mojom::ParseResultPtr refusal = CheckDocument(docx)) {
    std::move(callback).Run(std::move(refusal));
    return;
  }
  std::string error;
  const std::string text = docx::ExtractDocxText(
      std::string(base::as_string_view(base::span<const uint8_t>(docx))),
      &error);
  if (text.empty()) {
    std::move(callback).Run(Failure(error));
    return;
  }
  std::move(callback).Run(TextResult(text, max_chars, error));
}

}  // namespace molt_ai
