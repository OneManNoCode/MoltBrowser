// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/services/molt_ai/document_parser/pdf_page_renderer.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mojo/public/cpp/base/big_buffer.h"
#include "pdf/buildflags.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/geometry/size.h"

#if BUILDFLAG(ENABLE_PDF)
#include "third_party/pdfium/public/cpp/fpdf_scopers.h"  // nogncheck
#include "third_party/pdfium/public/fpdfview.h"          // nogncheck
#endif

namespace molt_ai {
namespace pdf_render {

namespace {

constexpr double kPointsPerInch = 72.0;

// |error| is one of this file's ASCII messages.
mojom::PdfPageImagesPtr Failure(std::string_view error,
                                uint32_t page_count = 0) {
  mojom::PdfPageImagesPtr result = mojom::PdfPageImages::New();
  result->success = false;
  result->page_count = page_count;
  result->error = std::string(error);
  return result;
}

#if BUILDFLAG(ENABLE_PDF)

// PDFium for the length of one call: no V8 isolate (so no JavaScript and no
// dynamic code), the AGG renderer, and an empty font path list, so PDFium
// uses only its built-in fonts and never looks for system ones (no font
// directory scans, no GDI on Windows). See pdf_page_renderer.h.
class ScopedPdfium {
 public:
  ScopedPdfium() {
    // PDFium keeps this pointer until FPDF_DestroyLibrary(): static.
    static const char* no_font_paths[] = {nullptr};
    FPDF_LIBRARY_CONFIG config = {};
    // Version 4 has the renderer type; the later fields (font backend,
    // Brotli) keep PDFium's defaults.
    config.version = 4;
    config.m_pUserFontPaths = no_font_paths;
    config.m_pIsolate = nullptr;
    config.m_v8EmbedderSlot = 0;
    config.m_pPlatform = nullptr;
    config.m_RendererType = FPDF_RENDERERTYPE_AGG;
    FPDF_InitLibraryWithConfig(&config);
  }
  ScopedPdfium(const ScopedPdfium&) = delete;
  ScopedPdfium& operator=(const ScopedPdfium&) = delete;
  ~ScopedPdfium() { FPDF_DestroyLibrary(); }
};

// |error| is FPDF_GetLastError()'s.
std::string_view LoadError(uint64_t error) {
  switch (error) {
    case FPDF_ERR_PASSWORD:
      return "the PDF is password-protected";
    case FPDF_ERR_SECURITY:
      return "the PDF uses an unsupported encryption scheme";
    case FPDF_ERR_FORMAT:
      return "not a PDF, or the PDF is damaged";
    default:
      return "the PDF could not be opened";
  }
}

// |page| drawn on white into a BGRA bitmap of |size| and encoded as PNG;
// nullopt if PDFium or the encoder fails (e.g. out of memory).
std::optional<std::vector<uint8_t>> RenderPageToPng(FPDF_PAGE page,
                                                     PageImageSize size) {
  const int width = static_cast<int>(size.width);
  const int height = static_cast<int>(size.height);
  // PDFium allocates the pixels and returns null if it can't.
  ScopedFPDFBitmap bitmap(FPDFBitmap_CreateEx(width, height, FPDFBitmap_BGRA,
                                              /*first_scan=*/nullptr,
                                              /*stride=*/0));
  if (!bitmap) {
    return std::nullopt;
  }
  FPDFBitmap_FillRect(bitmap.get(), 0, 0, width, height, 0xFFFFFFFF);
  // Annotations are part of what a reader sees. Never platform text output
  // (CoreGraphics, GDI).
  FPDF_RenderPageBitmap(
      bitmap.get(), page, /*start_x=*/0, /*start_y=*/0, width, height,
      /*rotate=*/0,
      FPDF_ANNOT | FPDF_NO_NATIVETEXT | FPDF_RENDER_LIMITEDIMAGECACHE);

  const void* pixels = FPDFBitmap_GetBuffer(bitmap.get());
  const int stride = FPDFBitmap_GetStride(bitmap.get());
  if (!pixels || stride / 4 < width) {
    return std::nullopt;
  }
  // PDFium's BGRA is B, G, R, A in memory, not premultiplied: FORMAT_BGRA.
  // Drawn on opaque white, every pixel is opaque, so the encoder writes RGB.
  return gfx::PNGCodec::Encode(static_cast<const unsigned char*>(pixels),
                               gfx::PNGCodec::FORMAT_BGRA,
                               gfx::Size(width, height), stride,
                               /*discard_transparency=*/false,
                               /*comments=*/{});
}

mojom::PdfPageImagesPtr RenderWithPdfium(base::span<const uint8_t> pdf,
                                         uint32_t max_pages,
                                         uint32_t dpi) {
  ScopedPdfium pdfium;
  // The empty password opens the many PDFs that are encrypted only with an
  // owner password.
  ScopedFPDFDocument document(
      FPDF_LoadMemDocument64(pdf.data(), pdf.size(), /*password=*/nullptr));
  if (!document) {
    return Failure(LoadError(FPDF_GetLastError()));
  }
  const int page_count = FPDF_GetPageCount(document.get());
  if (page_count <= 0) {
    return Failure("the PDF has no pages");
  }

  mojom::PdfPageImagesPtr result = mojom::PdfPageImages::New();
  result->page_count = static_cast<uint32_t>(page_count);
  const int pages_to_render =
      std::min(page_count, static_cast<int>(max_pages));
  size_t total_bytes = 0;
  bool over_budget = false;
  for (int index = 0; index < pages_to_render; ++index) {
    // A page that can't be loaded or drawn is left out; the rest may still
    // OCR.
    ScopedFPDFPage page(FPDF_LoadPage(document.get(), index));
    if (!page) {
      continue;
    }
    const std::optional<PageImageSize> size =
        PageImageSizeFor(FPDF_GetPageWidthF(page.get()),
                         FPDF_GetPageHeightF(page.get()), dpi);
    if (!size) {
      continue;
    }
    std::optional<std::vector<uint8_t>> png =
        RenderPageToPng(page.get(), *size);
    if (!png || png->empty()) {
      continue;
    }
    if (png->size() > size_t{mojom::kMaxPageImageBytes} - total_bytes) {
      // Stop rather than skip, so the pages sent are the first ones.
      over_budget = true;
      break;
    }
    total_bytes += png->size();
    mojom::PdfPageImagePtr image = mojom::PdfPageImage::New();
    image->page_index = static_cast<uint32_t>(index);
    image->width = size->width;
    image->height = size->height;
    image->png = mojo_base::BigBuffer(base::span<const uint8_t>(*png));
    result->pages.push_back(std::move(image));
  }
  if (result->pages.empty()) {
    return Failure(over_budget ? "the PDF's first page is too large to render"
                               : "none of the PDF's pages could be rendered",
                   result->page_count);
  }
  result->success = true;
  return result;
}

#endif  // BUILDFLAG(ENABLE_PDF)

}  // namespace

std::optional<PageImageSize> PageImageSizeFor(float width_pt,
                                              float height_pt,
                                              uint32_t dpi) {
  if (!std::isfinite(width_pt) || !std::isfinite(height_pt) ||
      width_pt <= 0 || height_pt <= 0 || dpi == 0) {
    return std::nullopt;
  }
  const double max_side = mojom::kMaxPageImageSide;
  const double max_pixels = mojom::kMaxPageImagePixels;
  double width = width_pt * (dpi / kPointsPerInch);
  double height = height_pt * (dpi / kPointsPerInch);
  // First each side, then (with both sides now finite and at most
  // max_side) the area.
  const double side_scale =
      std::min({1.0, max_side / width, max_side / height});
  width *= side_scale;
  height *= side_scale;
  if (width * height > max_pixels) {
    const double area_scale = std::sqrt(max_pixels / (width * height));
    width *= area_scale;
    height *= area_scale;
  }
  // Rounding down keeps the area within the limit. The tiny epsilon keeps a
  // side that was scaled to exactly a limit (10000 coming out as
  // 9999.999999999998) at the limit; it adds well under one pixel to the
  // area. A side below one pixel becomes one (the other is then at most
  // max_side, far below the area limit).
  constexpr double kEpsilon = 1e-6;
  PageImageSize size;
  size.width = static_cast<uint32_t>(
      std::clamp(std::floor(width + kEpsilon), 1.0, max_side));
  size.height = static_cast<uint32_t>(
      std::clamp(std::floor(height + kEpsilon), 1.0, max_side));
  if (uint64_t{size.width} * size.height > mojom::kMaxPageImagePixels) {
    return std::nullopt;  // Not reachable; the reply must never break it.
  }
  return size;
}

mojom::PdfPageImagesPtr RenderPdfPages(base::span<const uint8_t> pdf,
                                       uint32_t max_pages,
                                       uint32_t dpi) {
  max_pages = std::clamp(max_pages, uint32_t{1}, mojom::kMaxRenderPages);
  dpi = std::clamp(dpi, mojom::kMinRenderDpi, mojom::kMaxRenderDpi);
#if BUILDFLAG(ENABLE_PDF)
  if (pdf.empty()) {
    return Failure("the document is empty");
  }
  return RenderWithPdfium(pdf, max_pages, dpi);
#else
  return Failure("this build can't render PDF pages");
#endif
}

}  // namespace pdf_render
}  // namespace molt_ai
