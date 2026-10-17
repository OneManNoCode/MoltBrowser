// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Flat text of a .docx: in-memory unzip, then word/document.xml stripped to
// its <w:t> runs. The chat attachment fallback for a .docx that anydoc can't
// read. Runs only in the sandboxed document parser utility process
// (document_parser_impl.h); it moved here from the AI chat handler.

#ifndef CHROME_SERVICES_MOLT_AI_DOCUMENT_PARSER_DOCX_TEXT_EXTRACTOR_H_
#define CHROME_SERVICES_MOLT_AI_DOCUMENT_PARSER_DOCX_TEXT_EXTRACTOR_H_

#include <cstdint>
#include <string>

namespace molt_ai::docx {

// word/document.xml read cap: the decompressed XML can be far larger than the
// .docx itself (zip bomb), so the inflation is bounded.
inline constexpr uint64_t kMaxDocxXmlBytes = 20 * 1024 * 1024;

// Strips WordprocessingML down to plain text: keeps the character data
// inside <w:t> runs, maps </w:p> to \n, <w:tab/> to \t and <w:br/> / <w:cr/>
// to \n, and decodes the predefined XML entities.
std::string StripWordprocessingXml(const std::string& xml);

// The text of the .docx in |zip_bytes|. On failure returns "" and sets
// |*error| to a short diagnostic for the user. Never returns whitespace-only
// or non-UTF-8 text (both are errors).
std::string ExtractDocxText(const std::string& zip_bytes, std::string* error);

}  // namespace molt_ai::docx

#endif  // CHROME_SERVICES_MOLT_AI_DOCUMENT_PARSER_DOCX_TEXT_EXTRACTOR_H_
