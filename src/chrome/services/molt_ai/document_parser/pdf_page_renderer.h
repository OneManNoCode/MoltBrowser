// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// PDF pages to PNG for OCR (molt_ai.mojom.DocumentParser.RenderPdfPages).
// tesseract can't read PDFs (it decodes images through leptonica, which can
// write PDFs but not read them), so for a scan with no text layer the
// browser has the pages rendered here and OCRs the PNGs.
//
// Runs only in the sandboxed document parser process
// (document_parser_impl.h), under kService (no file system, no network; on
// Windows also no win32k and no dynamic code). That is enough because of how
// PDFium is driven:
//   - Through its C API, with no V8 isolate and no form-fill environment: a
//     PDF's JavaScript never runs and nothing needs dynamic code. (//pdf's
//     chrome_pdf::RenderPDFPageToBitmap would create a V8 isolate per call,
//     which needs JIT memory that kService refuses on Windows and macOS.)
//   - With an empty font path list: PDFium never scans font directories
//     (the sandbox would refuse them anyway) and on Windows never calls GDI.
//     Fonts a PDF doesn't embed are drawn with PDFium's built-in fonts,
//     which is fine for OCR of what is mostly scans.
//   - AGG into BGRA bitmaps, FPDF_NO_NATIVETEXT: no CoreGraphics or GDI
//     text output.
// The service handles one document per process, so PDFium is initialized and
// torn down per call. A hostile PDF that makes PDFium loop or exhaust memory
// costs only this process; the browser's timeout terminates it.

#ifndef CHROME_SERVICES_MOLT_AI_DOCUMENT_PARSER_PDF_PAGE_RENDERER_H_
#define CHROME_SERVICES_MOLT_AI_DOCUMENT_PARSER_PDF_PAGE_RENDERER_H_

#include <cstdint>
#include <optional>

#include "base/containers/span.h"
#include "chrome/services/molt_ai/document_parser/public/mojom/document_parser.mojom.h"

namespace molt_ai {
namespace pdf_render {

struct PageImageSize {
  uint32_t width = 0;
  uint32_t height = 0;
};

// The pixel size of a |width_pt| x |height_pt| page (PDF points, 1/72 inch)
// at |dpi|, scaled down with its aspect ratio kept so that each side is at
// most mojom::kMaxPageImageSide and the area at most
// mojom::kMaxPageImagePixels; each side is at least 1. nullopt for a side
// that is not finite or not positive, or a |dpi| of 0. Exposed for tests.
std::optional<PageImageSize> PageImageSizeFor(float width_pt,
                                              float height_pt,
                                              uint32_t dpi);

// Renders the first |max_pages| pages of |pdf| at |dpi| (clamped as the
// mojom says) to PNGs. Anything PDFium can't open is a failure with an error;
// nothing in |pdf| can make it CHECK. Without PDF support in the build
// (enable_pdf=false), every call fails.
mojom::PdfPageImagesPtr RenderPdfPages(base::span<const uint8_t> pdf,
                                       uint32_t max_pages,
                                       uint32_t dpi);

}  // namespace pdf_render
}  // namespace molt_ai

#endif  // CHROME_SERVICES_MOLT_AI_DOCUMENT_PARSER_PDF_PAGE_RENDERER_H_
