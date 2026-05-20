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

#ifndef CHROME_BROWSER_MOLT_AI_PDF_PDF_TEXT_SCRAPER_H_
#define CHROME_BROWSER_MOLT_AI_PDF_PDF_TEXT_SCRAPER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace molt_ai {
namespace pdf {

// Extract plain text from |pdf_bytes|. Stops collecting once |max_chars|
// is reached. Returns empty string if the bytes don't look like a PDF
// or no readable text was found.
std::string ExtractText(const std::vector<uint8_t>& pdf_bytes,
                        size_t max_chars = 50000);

}  // namespace pdf
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_PDF_PDF_TEXT_SCRAPER_H_
