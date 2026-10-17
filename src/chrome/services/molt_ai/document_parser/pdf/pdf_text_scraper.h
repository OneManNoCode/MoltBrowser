// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// PdfTextScraper — minimal text extractor for PDF byte streams.
//
// Strategy: PDFs are a series of `obj ... endobj` records. Text content
// lives in `stream ... endstream` blocks, which may be deflate-compressed
// (the dominant case in modern PDFs). Inside the decompressed content
// stream, text is shown via:
//   (literal) Tj
//   [(s1) num (s2) ...] TJ
//   (literal) '
//   (literal) "
//
// We scan for these operators, unescape PDF string literals, and
// concatenate. This works on the ~70% of PDFs whose text is encoded as
// 1-byte glyph indices that happen to match ASCII/Latin-1. Image-only
// scanned PDFs and CID-encoded CJK PDFs return empty — those need OCR
// (Tier 2 of the roadmap).
//
// This scraper does NOT depend on PDFium so we can run it on the UI
// thread inside a ScopedAllowBlockingForMolt without dragging in the
// full PDF rendering pipeline.

#ifndef CHROME_SERVICES_MOLT_AI_DOCUMENT_PARSER_PDF_PDF_TEXT_SCRAPER_H_
#define CHROME_SERVICES_MOLT_AI_DOCUMENT_PARSER_PDF_PDF_TEXT_SCRAPER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace molt_ai {
namespace pdf {

// Caps on zlib output, so a crafted PDF (a "decompression bomb") can't make
// the browser process allocate gigabytes. A stream that inflates past the
// per-stream cap is truncated (its prefix is still scanned); once the total
// cap is spent, scanning stops.
inline constexpr size_t kMaxStreamInflatedBytes = 16 * 1024 * 1024;
inline constexpr size_t kMaxTotalInflatedBytes = 64 * 1024 * 1024;

// Extract plain text from |pdf_bytes|. Stops collecting once |max_chars|
// is reached. Returns empty string if the bytes don't look like a PDF
// or no readable text was found.
std::string ExtractText(const std::vector<uint8_t>& pdf_bytes,
                        size_t max_chars = 50000);

}  // namespace pdf
}  // namespace molt_ai

#endif  // CHROME_SERVICES_MOLT_AI_DOCUMENT_PARSER_PDF_PDF_TEXT_SCRAPER_H_
