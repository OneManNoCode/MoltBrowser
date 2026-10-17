// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/services/molt_ai/document_parser/document_parser_impl.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/numerics/byte_conversions.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "chrome/services/molt_ai/document_parser/pdf_page_renderer.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "pdf/buildflags.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/zlib/zlib.h"
#include "ui/gfx/codec/png_codec.h"

namespace molt_ai {
namespace {

using document_parser_internal::SanitizeExtension;
using document_parser_internal::ToValidUtf8;
using pdf_render::PageImageSize;
using pdf_render::PageImageSizeFor;

constexpr char kReplacement[] = "\xEF\xBF\xBD";  // U+FFFD

mojo_base::BigBuffer Buffer(const std::string& bytes) {
  return mojo_base::BigBuffer(base::as_byte_span(bytes));
}

// A PDF whose only (uncompressed) content stream is |content|. The scraper
// walks stream ... endstream blocks, so no xref table is needed.
std::string MakePdf(const std::string& content) {
  return "%PDF-1.4\n1 0 obj\n<<>>\nstream\n" + content +
         "\nendstream\nendobj\n%%EOF\n";
}

void AppendLe16(std::string& out, uint16_t v) {
  out.push_back(static_cast<char>(v & 0xff));
  out.push_back(static_cast<char>(v >> 8));
}

void AppendLe32(std::string& out, uint32_t v) {
  AppendLe16(out, static_cast<uint16_t>(v & 0xffff));
  AppendLe16(out, static_cast<uint16_t>(v >> 16));
}

// A minimal ZIP archive with |files| (name, contents) stored uncompressed.
std::string MakeStoredZip(
    const std::vector<std::pair<std::string, std::string>>& files) {
  std::string out;
  std::string central;
  for (const auto& [name, data] : files) {
    const uint32_t crc = static_cast<uint32_t>(
        crc32(0L, reinterpret_cast<const Bytef*>(data.data()),
              static_cast<uInt>(data.size())));
    const uint32_t offset = static_cast<uint32_t>(out.size());
    // Local file header.
    AppendLe32(out, 0x04034b50);
    AppendLe16(out, 20);    // version needed
    AppendLe16(out, 0);     // flags
    AppendLe16(out, 0);     // method: stored
    AppendLe16(out, 0);     // time
    AppendLe16(out, 0x21);  // date: 1980-01-01
    AppendLe32(out, crc);
    AppendLe32(out, static_cast<uint32_t>(data.size()));
    AppendLe32(out, static_cast<uint32_t>(data.size()));
    AppendLe16(out, static_cast<uint16_t>(name.size()));
    AppendLe16(out, 0);  // extra length
    out += name;
    out += data;
    // Central directory entry.
    AppendLe32(central, 0x02014b50);
    AppendLe16(central, 20);  // version made by
    AppendLe16(central, 20);  // version needed
    AppendLe16(central, 0);
    AppendLe16(central, 0);
    AppendLe16(central, 0);
    AppendLe16(central, 0x21);
    AppendLe32(central, crc);
    AppendLe32(central, static_cast<uint32_t>(data.size()));
    AppendLe32(central, static_cast<uint32_t>(data.size()));
    AppendLe16(central, static_cast<uint16_t>(name.size()));
    AppendLe16(central, 0);  // extra length
    AppendLe16(central, 0);  // comment length
    AppendLe16(central, 0);  // disk number
    AppendLe16(central, 0);  // internal attributes
    AppendLe32(central, 0);  // external attributes
    AppendLe32(central, offset);
    central += name;
  }
  const uint32_t central_offset = static_cast<uint32_t>(out.size());
  out += central;
  // End of central directory.
  AppendLe32(out, 0x06054b50);
  AppendLe16(out, 0);
  AppendLe16(out, 0);
  AppendLe16(out, static_cast<uint16_t>(files.size()));
  AppendLe16(out, static_cast<uint16_t>(files.size()));
  AppendLe32(out, static_cast<uint32_t>(central.size()));
  AppendLe32(out, central_offset);
  AppendLe16(out, 0);
  return out;
}

// A black box from (50, 25) to (150, 75) in PDF space (origin bottom left).
constexpr char kBlackBox[] = "0 0 0 rg 50 25 100 50 re f";

// A PDF with |page_count| pages of |media_box| (points) that each draw
// |content|, with a correct cross-reference table, so PDFium opens it
// without repairing it. |extra_trailer| goes into the trailer dictionary.
std::string MakeRenderablePdf(int page_count,
                              const std::string& media_box = "0 0 200 100",
                              const std::string& content = kBlackBox,
                              const std::string& extra_trailer = "") {
  // objects[i] is object number i + 1: the catalog, the page tree, then a
  // page and its content stream for each page.
  std::vector<std::string> objects;
  std::string kids;
  for (int i = 0; i < page_count; ++i) {
    kids += base::StringPrintf("%d 0 R ", 3 + 2 * i);
  }
  objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
  objects.push_back("<< /Type /Pages /Kids [" + kids + "] /Count " +
                    base::NumberToString(page_count) + " >>");
  for (int i = 0; i < page_count; ++i) {
    objects.push_back(base::StringPrintf(
        "<< /Type /Page /Parent 2 0 R /MediaBox [%s] /Resources << >> "
        "/Contents %d 0 R >>",
        media_box.c_str(), 4 + 2 * i));
    objects.push_back(base::StringPrintf("<< /Length %zu >>\nstream\n",
                                         content.size()) +
                      content + "\nendstream");
  }
  std::string pdf = "%PDF-1.4\n";
  std::vector<size_t> offsets;
  for (size_t i = 0; i < objects.size(); ++i) {
    offsets.push_back(pdf.size());
    pdf += base::StringPrintf("%zu 0 obj\n", i + 1) + objects[i] +
           "\nendobj\n";
  }
  const size_t xref_offset = pdf.size();
  // Each entry is exactly 20 bytes.
  pdf += base::StringPrintf("xref\n0 %zu\n0000000000 65535 f \n",
                            objects.size() + 1);
  for (size_t offset : offsets) {
    pdf += base::StringPrintf("%010zu 00000 n \n", offset);
  }
  pdf += base::StringPrintf(
      "trailer\n<< /Size %zu /Root 1 0 R %s>>\nstartxref\n%zu\n%%%%EOF\n",
      objects.size() + 1, extra_trailer.c_str(), xref_offset);
  return pdf;
}

// The width and height in a PNG's IHDR (which must be the first chunk).
PageImageSize PngSize(const mojo_base::BigBuffer& png) {
  const base::span<const uint8_t> bytes(png);
  PageImageSize size;
  if (bytes.size() < 24) {
    return size;
  }
  size.width = base::U32FromBigEndian(bytes.subspan(16u).first<4u>());
  size.height = base::U32FromBigEndian(bytes.subspan(20u).first<4u>());
  return size;
}

bool HasPngSignature(const mojo_base::BigBuffer& png) {
  const base::span<const uint8_t> bytes(png);
  static constexpr uint8_t kSignature[] = {0x89, 'P',  'N',  'G',
                                           '\r', '\n', 0x1A, '\n'};
  return bytes.size() >= 8 && std::ranges::equal(bytes.first(8u), kSignature);
}

class DocumentParserImplTest : public testing::Test {
 protected:
  // The service is called directly (its receiver is unbound), as the utility
  // process's mojo dispatch would.
  mojom::ParseResultPtr ExtractPdfText(const std::string& bytes,
                                       uint32_t max_chars = 50000) {
    base::test::TestFuture<mojom::ParseResultPtr> future;
    parser_.ExtractPdfText(Buffer(bytes), max_chars, future.GetCallback());
    return future.Take();
  }

  mojom::ParseResultPtr ConvertToMarkdown(const std::string& bytes,
                                          const std::string& extension,
                                          uint32_t max_chars = 50000) {
    base::test::TestFuture<mojom::ParseResultPtr> future;
    parser_.ConvertToMarkdown(Buffer(bytes), extension, max_chars,
                              future.GetCallback());
    return future.Take();
  }

  mojom::ParseResultPtr ExtractDocxText(const std::string& bytes,
                                        uint32_t max_chars = 50000) {
    base::test::TestFuture<mojom::ParseResultPtr> future;
    parser_.ExtractDocxText(Buffer(bytes), max_chars, future.GetCallback());
    return future.Take();
  }

  mojom::PdfPageImagesPtr RenderPdfPages(const std::string& bytes,
                                         uint32_t max_pages = 20,
                                         uint32_t dpi = 72) {
    base::test::TestFuture<mojom::PdfPageImagesPtr> future;
    parser_.RenderPdfPages(Buffer(bytes), max_pages, dpi,
                           future.GetCallback());
    return future.Take();
  }

  // Checks what every RenderPdfPages failure looks like.
  void ExpectRenderFailure(const std::string& bytes) {
    mojom::PdfPageImagesPtr result = RenderPdfPages(bytes);
    ASSERT_TRUE(result);
    EXPECT_FALSE(result->success);
    EXPECT_TRUE(result->pages.empty());
    EXPECT_FALSE(result->error.empty());
    EXPECT_TRUE(base::IsStringUTF8(result->error));
  }

  base::test::TaskEnvironment task_environment_;
  DocumentParserImpl parser_{mojo::PendingReceiver<mojom::DocumentParser>()};
};

TEST_F(DocumentParserImplTest, PdfTextLayer) {
  mojom::ParseResultPtr result =
      ExtractPdfText(MakePdf("BT (Hello from the sandbox) Tj ET"));
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->success);
  EXPECT_NE(result->text.find("Hello from the sandbox"), std::string::npos);
  EXPECT_TRUE(result->error.empty());
}

TEST_F(DocumentParserImplTest, PdfGarbageFails) {
  static constexpr char kBinaryStream[] =
      "%PDF-1.7\n\xff\xfe\x00\x01 stream\n\x01\x02 endstream";
  for (const std::string& garbage :
       {std::string("this is not a PDF at all"),
        std::string(kBinaryStream, sizeof(kBinaryStream) - 1),
        std::string(1000, '\0')}) {
    mojom::ParseResultPtr result = ExtractPdfText(garbage);
    ASSERT_TRUE(result);
    EXPECT_FALSE(result->success);
    EXPECT_TRUE(result->text.empty());
    EXPECT_FALSE(result->error.empty());
  }
}

TEST_F(DocumentParserImplTest, PdfGlyphBytesBecomeValidUtf8) {
  // \351 is a Latin-1 'é': a raw glyph byte, not UTF-8.
  mojom::ParseResultPtr result = ExtractPdfText(MakePdf("(caf\\351) Tj"));
  ASSERT_TRUE(result);
  ASSERT_TRUE(result->success);
  EXPECT_TRUE(base::IsStringUTF8(result->text));
  EXPECT_NE(result->text.find(std::string("caf") + kReplacement),
            std::string::npos);
}

TEST_F(DocumentParserImplTest, PdfRespectsMaxChars) {
  std::string content;
  for (int i = 0; i < 200; ++i) {
    content += "(0123456789) Tj\n";
  }
  mojom::ParseResultPtr result = ExtractPdfText(MakePdf(content), 25);
  ASSERT_TRUE(result);
  ASSERT_TRUE(result->success);
  EXPECT_LE(result->text.size(), 25u);
}

TEST_F(DocumentParserImplTest, MaxCharsIsClampedToTheServiceLimit) {
  std::string content;
  const std::string run = "(" + std::string(1000, 'x') + ") Tj\n";
  while (content.size() < 2 * mojom::kMaxOutputChars) {
    content += run;
  }
  mojom::ParseResultPtr result =
      ExtractPdfText(MakePdf(content), std::numeric_limits<uint32_t>::max());
  ASSERT_TRUE(result);
  ASSERT_TRUE(result->success);
  EXPECT_LE(result->text.size(), size_t{mojom::kMaxOutputChars});
  EXPECT_GT(result->text.size(), size_t{mojom::kMaxOutputChars} / 2);
}

TEST_F(DocumentParserImplTest, OversizeDocumentsAreRefused) {
  // One byte over the limit, and starting like a real PDF / ZIP.
  std::string huge(mojom::kMaxDocumentBytes + 1, ' ');
  huge.replace(0, 5, "%PDF-");
  mojom::ParseResultPtr pdf = ExtractPdfText(huge);
  ASSERT_TRUE(pdf);
  EXPECT_FALSE(pdf->success);
  EXPECT_NE(pdf->error.find("too large"), std::string::npos);

  huge.replace(0, 5, "PK\x03\x04 ");
  mojom::ParseResultPtr markdown = ConvertToMarkdown(huge, "docx");
  ASSERT_TRUE(markdown);
  EXPECT_FALSE(markdown->success);
  EXPECT_NE(markdown->error.find("too large"), std::string::npos);

  mojom::ParseResultPtr docx = ExtractDocxText(huge);
  ASSERT_TRUE(docx);
  EXPECT_FALSE(docx->success);
  EXPECT_NE(docx->error.find("too large"), std::string::npos);
}

TEST_F(DocumentParserImplTest, EmptyDocumentsAreRefused) {
  EXPECT_FALSE(ExtractPdfText(std::string())->success);
  EXPECT_FALSE(ConvertToMarkdown(std::string(), "csv")->success);
  EXPECT_FALSE(ExtractDocxText(std::string())->success);
}

TEST_F(DocumentParserImplTest, CsvToMarkdown) {
  mojom::ParseResultPtr result =
      ConvertToMarkdown("item,qty\napple,3\npear,12\n", "csv");
  ASSERT_TRUE(result);
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_TRUE(base::IsStringUTF8(result->text));
  EXPECT_NE(result->text.find("apple"), std::string::npos);
  EXPECT_NE(result->text.find("pear"), std::string::npos);
}

TEST_F(DocumentParserImplTest, MarkdownOfGarbageFails) {
  mojom::ParseResultPtr result =
      ConvertToMarkdown("definitely not a word document", "docx");
  ASSERT_TRUE(result);
  EXPECT_FALSE(result->success);
  EXPECT_TRUE(result->text.empty());
  EXPECT_FALSE(result->error.empty());
  EXPECT_TRUE(base::IsStringUTF8(result->error));
}

TEST_F(DocumentParserImplTest, BadExtensionHintIsDropped) {
  // Not UTF-8: rust::Str would abort on it. Dropped, CSV can't be detected
  // from the bytes, so the conversion fails cleanly.
  mojom::ParseResultPtr result =
      ConvertToMarkdown("item,qty\napple,3\n", "\xff\xfe");
  ASSERT_TRUE(result);
  EXPECT_FALSE(result->success);
}

TEST_F(DocumentParserImplTest, DocxFlatText) {
  const std::string xml =
      "<?xml version=\"1.0\"?><w:document><w:body>"
      "<w:p><w:r><w:t>Hello</w:t></w:r></w:p>"
      "<w:p><w:r><w:t xml:space=\"preserve\">fish &amp; chips</w:t></w:r>"
      "</w:p></w:body></w:document>";
  mojom::ParseResultPtr result = ExtractDocxText(MakeStoredZip(
      {{"[Content_Types].xml", "<Types/>"}, {"word/document.xml", xml}}));
  ASSERT_TRUE(result);
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_EQ(result->text, "Hello\nfish & chips\n");
}

TEST_F(DocumentParserImplTest, DocxErrors) {
  mojom::ParseResultPtr not_zip = ExtractDocxText("not a zip archive");
  ASSERT_TRUE(not_zip);
  EXPECT_FALSE(not_zip->success);
  EXPECT_NE(not_zip->error.find("not a valid .docx"), std::string::npos);

  mojom::ParseResultPtr no_document =
      ExtractDocxText(MakeStoredZip({{"word/styles.xml", "<w:styles/>"}}));
  ASSERT_TRUE(no_document);
  EXPECT_FALSE(no_document->success);
  EXPECT_NE(no_document->error.find("no word/document.xml"), std::string::npos);
}

// RenderPdfPages refuses what no parser should see, with or without PDF
// support in the build.
TEST_F(DocumentParserImplTest, RenderRefusesEmptyAndOversizeDocuments) {
  ExpectRenderFailure(std::string());

  std::string huge = MakeRenderablePdf(1);
  huge.resize(size_t{mojom::kMaxDocumentBytes} + 1, ' ');
  mojom::PdfPageImagesPtr result = RenderPdfPages(huge);
  ASSERT_TRUE(result);
  EXPECT_FALSE(result->success);
  EXPECT_TRUE(result->pages.empty());
  EXPECT_NE(result->error.find("too large"), std::string::npos);
}

#if BUILDFLAG(ENABLE_PDF)

TEST_F(DocumentParserImplTest, RenderDrawsThePage) {
  mojom::PdfPageImagesPtr result = RenderPdfPages(MakeRenderablePdf(1));
  ASSERT_TRUE(result);
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_TRUE(result->error.empty());
  EXPECT_EQ(result->page_count, 1u);
  ASSERT_EQ(result->pages.size(), 1u);
  const mojom::PdfPageImagePtr& page = result->pages[0];
  EXPECT_EQ(page->page_index, 0u);
  // 200 x 100 points at 72 dpi.
  EXPECT_EQ(page->width, 200u);
  EXPECT_EQ(page->height, 100u);
  ASSERT_TRUE(HasPngSignature(page->png));
  EXPECT_EQ(PngSize(page->png).width, 200u);
  EXPECT_EQ(PngSize(page->png).height, 100u);

  // The box is black, the rest white. PDF y runs upwards: the box covers
  // rows 25 to 75 from the top.
  const SkBitmap bitmap =
      gfx::PNGCodec::Decode(base::span<const uint8_t>(page->png));
  ASSERT_FALSE(bitmap.isNull());
  ASSERT_EQ(bitmap.width(), 200);
  ASSERT_EQ(bitmap.height(), 100);
  const SkColor inside = bitmap.getColor(100, 50);
  EXPECT_LT(SkColorGetR(inside), 32u);
  EXPECT_LT(SkColorGetG(inside), 32u);
  EXPECT_LT(SkColorGetB(inside), 32u);
  for (const auto [x, y] : {std::pair{10, 10}, std::pair{190, 90},
                            std::pair{100, 10}, std::pair{20, 50}}) {
    const SkColor outside = bitmap.getColor(x, y);
    EXPECT_GT(SkColorGetR(outside), 224u) << x << "," << y;
    EXPECT_GT(SkColorGetG(outside), 224u) << x << "," << y;
    EXPECT_GT(SkColorGetB(outside), 224u) << x << "," << y;
    EXPECT_EQ(SkColorGetA(outside), 0xFFu) << x << "," << y;
  }
}

TEST_F(DocumentParserImplTest, RenderClampsTheResolution) {
  // 200 x 100 points: at most kMaxRenderDpi (200) ...
  mojom::PdfPageImagesPtr high = RenderPdfPages(MakeRenderablePdf(1), 20,
                                                /*dpi=*/1000);
  ASSERT_TRUE(high && high->success);
  EXPECT_EQ(high->pages[0]->width, 555u);   // 200 * 200 / 72 = 555.6
  EXPECT_EQ(high->pages[0]->height, 277u);  // 100 * 200 / 72 = 277.8
  EXPECT_EQ(PngSize(high->pages[0]->png).width, 555u);

  // ... and at least kMinRenderDpi (50).
  mojom::PdfPageImagesPtr low = RenderPdfPages(MakeRenderablePdf(1), 20,
                                               /*dpi=*/0);
  ASSERT_TRUE(low && low->success);
  EXPECT_EQ(low->pages[0]->width, 138u);  // 200 * 50 / 72 = 138.9
  EXPECT_EQ(low->pages[0]->height, 69u);  // 100 * 50 / 72 = 69.4
}

TEST_F(DocumentParserImplTest, RenderCapsThePageCount) {
  const std::string pdf = MakeRenderablePdf(25);

  mojom::PdfPageImagesPtr all = RenderPdfPages(pdf, /*max_pages=*/1000);
  ASSERT_TRUE(all && all->success);
  EXPECT_EQ(all->page_count, 25u);
  ASSERT_EQ(all->pages.size(), size_t{mojom::kMaxRenderPages});
  for (size_t i = 0; i < all->pages.size(); ++i) {
    EXPECT_EQ(all->pages[i]->page_index, i);
    EXPECT_TRUE(HasPngSignature(all->pages[i]->png));
  }

  mojom::PdfPageImagesPtr three = RenderPdfPages(pdf, /*max_pages=*/3);
  ASSERT_TRUE(three && three->success);
  EXPECT_EQ(three->page_count, 25u);
  EXPECT_EQ(three->pages.size(), 3u);

  // 0 pages is clamped to 1.
  mojom::PdfPageImagesPtr none = RenderPdfPages(pdf, /*max_pages=*/0);
  ASSERT_TRUE(none && none->success);
  EXPECT_EQ(none->pages.size(), 1u);
}

TEST_F(DocumentParserImplTest, RenderScalesHugePagesDown) {
  // 14400 points (200 inches, the PDF maximum) square: 40000 pixels a side
  // at 200 dpi, so scaled to 10000 a side and then to 25 MP: 5000 x 5000.
  // The content is empty, so the PNG stays small.
  mojom::PdfPageImagesPtr result = RenderPdfPages(
      MakeRenderablePdf(1, "0 0 14400 14400", /*content=*/"q Q"), 20, 200);
  ASSERT_TRUE(result && result->success) << (result ? result->error : "");
  const mojom::PdfPageImagePtr& page = result->pages[0];
  EXPECT_LE(page->width, mojom::kMaxPageImageSide);
  EXPECT_LE(page->height, mojom::kMaxPageImageSide);
  EXPECT_LE(uint64_t{page->width} * page->height,
            uint64_t{mojom::kMaxPageImagePixels});
  EXPECT_GE(page->width, 4999u);
  EXPECT_EQ(page->width, page->height);
  EXPECT_EQ(PngSize(page->png).width, page->width);
  EXPECT_EQ(PngSize(page->png).height, page->height);
}

TEST_F(DocumentParserImplTest, RenderFailsCleanlyOnBadInput) {
  // Not a PDF.
  ExpectRenderFailure("this is not a PDF at all");
  ExpectRenderFailure(std::string(4096, '\0'));
  // A header and nothing that parses.
  static constexpr char kBinary[] = "%PDF-1.7\n\xff\xfe\x00\x01 obj\n<<";
  ExpectRenderFailure(std::string(kBinary, sizeof(kBinary) - 1));
  // No pages.
  ExpectRenderFailure(MakeRenderablePdf(0));
}

TEST_F(DocumentParserImplTest, RenderRefusesAPasswordProtectedPdf) {
  // Standard security handler, revision 2, with a /U that the empty user
  // password doesn't produce: opening needs a password.
  const std::string kHex32(64, 'a');
  const std::string encrypt =
      "/Encrypt << /Filter /Standard /V 1 /R 2 /Length 40 /P -4 /O <" +
      kHex32 + "> /U <" + kHex32 +
      "> >> /ID [<0123456789abcdef0123456789abcdef> "
      "<0123456789abcdef0123456789abcdef>] ";
  mojom::PdfPageImagesPtr result =
      RenderPdfPages(MakeRenderablePdf(1, "0 0 200 100", kBlackBox, encrypt));
  ASSERT_TRUE(result);
  EXPECT_FALSE(result->success);
  EXPECT_TRUE(result->pages.empty());
  EXPECT_NE(result->error.find("password"), std::string::npos)
      << result->error;
}

TEST_F(DocumentParserImplTest, RenderSurvivesTruncatedPdfs) {
  // Every prefix of a two-page PDF: PDFium may repair some of them; each
  // answer must be well formed either way.
  const std::string pdf = MakeRenderablePdf(2);
  for (size_t length = 1; length < pdf.size(); length += 7) {
    mojom::PdfPageImagesPtr result = RenderPdfPages(pdf.substr(0, length));
    ASSERT_TRUE(result);
    if (result->success) {
      EXPECT_FALSE(result->pages.empty());
      EXPECT_LE(result->pages.size(), size_t{result->page_count});
      for (const mojom::PdfPageImagePtr& page : result->pages) {
        EXPECT_TRUE(HasPngSignature(page->png));
      }
    } else {
      EXPECT_TRUE(result->pages.empty());
      EXPECT_FALSE(result->error.empty());
    }
  }
}

#else  // !BUILDFLAG(ENABLE_PDF)

TEST_F(DocumentParserImplTest, RenderIsUnsupportedWithoutPdf) {
  ExpectRenderFailure(MakeRenderablePdf(1));
}

#endif  // BUILDFLAG(ENABLE_PDF)

TEST(PdfPageImageSizeTest, ScalesPointsToPixels) {
  std::optional<PageImageSize> size = PageImageSizeFor(200, 100, 72);
  ASSERT_TRUE(size);
  EXPECT_EQ(size->width, 200u);
  EXPECT_EQ(size->height, 100u);

  // US Letter at 200 dpi.
  size = PageImageSizeFor(612, 792, 200);
  ASSERT_TRUE(size);
  EXPECT_EQ(size->width, 1700u);
  EXPECT_EQ(size->height, 2200u);
}

TEST(PdfPageImageSizeTest, CapsEachSide) {
  // 5000 x 100 points at 200 dpi: 13889 x 278, scaled to 10000 x 200.
  std::optional<PageImageSize> size = PageImageSizeFor(5000, 100, 200);
  ASSERT_TRUE(size);
  EXPECT_EQ(size->width, mojom::kMaxPageImageSide);
  EXPECT_EQ(size->height, 200u);

  // An absurd aspect ratio: the long side is capped, the short one is at
  // least a pixel.
  size = PageImageSizeFor(1e9f, 1e-3f, 200);
  ASSERT_TRUE(size);
  EXPECT_EQ(size->width, mojom::kMaxPageImageSide);
  EXPECT_EQ(size->height, 1u);
}

TEST(PdfPageImageSizeTest, CapsTheArea) {
  std::optional<PageImageSize> size = PageImageSizeFor(14400, 14400, 200);
  ASSERT_TRUE(size);
  EXPECT_LE(uint64_t{size->width} * size->height,
            uint64_t{mojom::kMaxPageImagePixels});
  EXPECT_GE(size->width, 4999u);
  EXPECT_EQ(size->width, size->height);

  // The largest float: still within the limits, no overflow.
  size = PageImageSizeFor(std::numeric_limits<float>::max(),
                          std::numeric_limits<float>::max(), 200);
  ASSERT_TRUE(size);
  EXPECT_LE(size->width, mojom::kMaxPageImageSide);
  EXPECT_LE(uint64_t{size->width} * size->height,
            uint64_t{mojom::kMaxPageImagePixels});
}

TEST(PdfPageImageSizeTest, RejectsDegeneratePages) {
  EXPECT_FALSE(PageImageSizeFor(0, 100, 72));
  EXPECT_FALSE(PageImageSizeFor(100, -1, 72));
  EXPECT_FALSE(PageImageSizeFor(std::numeric_limits<float>::infinity(), 100,
                                72));
  EXPECT_FALSE(
      PageImageSizeFor(std::numeric_limits<float>::quiet_NaN(), 100, 72));
  EXPECT_FALSE(PageImageSizeFor(100, 100, 0));
  // A tiny page is one pixel, not zero.
  std::optional<PageImageSize> size = PageImageSizeFor(0.01f, 0.01f, 72);
  ASSERT_TRUE(size);
  EXPECT_EQ(size->width, 1u);
  EXPECT_EQ(size->height, 1u);
}

TEST(DocumentParserInternalTest, ToValidUtf8) {
  EXPECT_EQ(ToValidUtf8("plain", 100), "plain");
  EXPECT_EQ(ToValidUtf8("a\xff"
                        "b",
                        100),
            std::string("a") + kReplacement + "b");
  // A non-character (U+FFFE) is valid UTF-8 but fails base::IsStringUTF8.
  EXPECT_EQ(ToValidUtf8("x\xEF\xBF\xBEy", 100),
            std::string("x") + kReplacement + "y");
  // The cut never splits a character: "aé" (3 bytes) cut at 2 is "a".
  EXPECT_EQ(ToValidUtf8("a\xC3\xA9", 2), "a");
  EXPECT_EQ(ToValidUtf8("a\xC3\xA9", 3), "a\xC3\xA9");
  // A truncated sequence at the end is one U+FFFD.
  EXPECT_EQ(ToValidUtf8("ok\xF0\x9F", 100), std::string("ok") + kReplacement);
  EXPECT_EQ(ToValidUtf8("anything", 0), "");
  EXPECT_TRUE(
      base::IsStringUTF8(ToValidUtf8("\x80\x81\xC0\xAF\xED\xA0\x80", 100)));
}

TEST(DocumentParserInternalTest, SanitizeExtension) {
  EXPECT_EQ(SanitizeExtension("docx"), "docx");
  EXPECT_EQ(SanitizeExtension("XLSX"), "xlsx");
  EXPECT_EQ(SanitizeExtension(""), "");
  EXPECT_EQ(SanitizeExtension("tar.gz"), "");
  EXPECT_EQ(SanitizeExtension("toolongext"), "");
  EXPECT_EQ(SanitizeExtension("\xc3\xa9"), "");
}

}  // namespace
}  // namespace molt_ai
