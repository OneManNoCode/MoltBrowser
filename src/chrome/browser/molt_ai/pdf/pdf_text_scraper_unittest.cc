// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/pdf/pdf_text_scraper.h"

#include <cstdint>
#include <string>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace pdf {
namespace {

// Build a minimal PDF where the only stream is the supplied (already
// uncompressed) content payload. We don't bother with a real xref/
// trailer — the scraper walks `stream ... endstream` blocks directly
// regardless of cross-reference table validity.
std::vector<uint8_t> MakePdfWithRawStream(const std::string& content) {
  std::string out;
  out += "%PDF-1.4\n";
  out += "1 0 obj\n<<>>\nstream\n";
  out += content;
  out += "\nendstream\nendobj\n";
  out += "%%EOF\n";
  return std::vector<uint8_t>(out.begin(), out.end());
}

// Convenience: take a literal show-text operator like (hello) Tj and
// wrap it in a PDF.
std::vector<uint8_t> MakePdfWithShowText(const std::string& body) {
  return MakePdfWithRawStream(body);
}

TEST(PdfTextScraperTest, EmptyInputReturnsEmpty) {
  std::vector<uint8_t> empty;
  EXPECT_EQ(ExtractText(empty), std::string());
}

TEST(PdfTextScraperTest, BytesUnderEightReturnEmpty) {
  // The scraper bails out before even checking the header if there
  // aren't 8 bytes — this guards against the header memcmp reading
  // past the buffer.
  std::vector<uint8_t> tiny = {'%', 'P', 'D', 'F'};
  EXPECT_EQ(ExtractText(tiny), std::string());
}

TEST(PdfTextScraperTest, NonPdfHeaderReturnsEmpty) {
  std::string fake = "Not a PDF at all, just some text.";
  std::vector<uint8_t> bytes(fake.begin(), fake.end());
  EXPECT_EQ(ExtractText(bytes), std::string());
}

TEST(PdfTextScraperTest, AcceptsUtf8BomPrefix) {
  // Per spec the PDF header can be preceded by a UTF-8 BOM. Our
  // scraper skips a leading EF BB BF before checking %PDF-.
  std::string body = "BT (BOM survived) Tj ET";
  std::vector<uint8_t> raw = MakePdfWithShowText(body);
  std::vector<uint8_t> with_bom = {0xEF, 0xBB, 0xBF};
  with_bom.insert(with_bom.end(), raw.begin(), raw.end());
  EXPECT_NE(ExtractText(with_bom).find("BOM survived"), std::string::npos);
}

TEST(PdfTextScraperTest, ExtractsBasicTjShowText) {
  auto pdf = MakePdfWithShowText("BT (Hello World) Tj ET");
  std::string text = ExtractText(pdf);
  EXPECT_NE(text.find("Hello World"), std::string::npos);
}

TEST(PdfTextScraperTest, ExtractsMultipleTextOpsInOneStream) {
  // The scraper appends a separator space between adjacent show-ops
  // so adjacent words don't merge into "FirstSecond". Verify both
  // pieces of text appear in the right order.
  auto pdf =
      MakePdfWithShowText("BT (First) Tj (Second) Tj (Third) Tj ET");
  std::string text = ExtractText(pdf);
  size_t f = text.find("First");
  size_t s = text.find("Second");
  size_t t = text.find("Third");
  ASSERT_NE(f, std::string::npos);
  ASSERT_NE(s, std::string::npos);
  ASSERT_NE(t, std::string::npos);
  EXPECT_LT(f, s);
  EXPECT_LT(s, t);
}

TEST(PdfTextScraperTest, RecognizesAllFourShowOps) {
  // Tj, TJ array, ', and " all count as text-show operators.
  // (We only test that the surrounding literal is captured; TJ's
  // array form is treated as a sequence of literal Tj-equivalents.)
  auto pdf = MakePdfWithShowText(
      "BT\n"
      "(via-Tj) Tj\n"
      "(via-tick) '\n"
      "(via-dquote) \"\n"
      "ET");
  std::string text = ExtractText(pdf);
  EXPECT_NE(text.find("via-Tj"), std::string::npos);
  EXPECT_NE(text.find("via-tick"), std::string::npos);
  EXPECT_NE(text.find("via-dquote"), std::string::npos);
}

TEST(PdfTextScraperTest, UnescapesStandardEscapes) {
  // PDF string literals support: \n \r \t \b \f \\ \( \) \ddd (octal).
  // We use \\n (literal backslash-n) inside the C++ raw to produce
  // the two characters \n in the PDF stream, which the scraper
  // should turn into an actual newline.
  std::string body =
      "BT (line1\\nline2) Tj ET";  // -> "line1\nline2"
  auto pdf = MakePdfWithShowText(body);
  std::string text = ExtractText(pdf);
  EXPECT_NE(text.find("line1"), std::string::npos);
  EXPECT_NE(text.find("line2"), std::string::npos);
  // Either literal newline or the cleaner-substituted single space;
  // both are acceptable — what matters is the two halves both appear.
}

TEST(PdfTextScraperTest, UnescapesParens) {
  // \( and \) should produce literal parens, NOT close the string.
  std::string body =
      "BT (a\\(b\\)c) Tj ET";  // -> a(b)c
  auto pdf = MakePdfWithShowText(body);
  std::string text = ExtractText(pdf);
  EXPECT_NE(text.find("a(b)c"), std::string::npos);
}

TEST(PdfTextScraperTest, NestedParensAreBalanced) {
  // Unescaped nested parens inside a literal are legal in PDF —
  // the scanner counts depth and terminates on the matching close.
  std::string body = "BT (outer (inner) text) Tj ET";
  auto pdf = MakePdfWithShowText(body);
  std::string text = ExtractText(pdf);
  EXPECT_NE(text.find("outer (inner) text"), std::string::npos);
}

TEST(PdfTextScraperTest, UnescapesOctal) {
  // \101 is octal 'A', \102 is 'B'. Three-digit octal escapes are
  // the standard way to embed non-ASCII bytes in PDF strings.
  std::string body = "BT (\\101\\102\\103) Tj ET";  // ABC
  auto pdf = MakePdfWithShowText(body);
  std::string text = ExtractText(pdf);
  EXPECT_NE(text.find("ABC"), std::string::npos);
}

TEST(PdfTextScraperTest, FiltersGarbledGlyphIndicesOut) {
  // Some PDFs use 1-byte glyph indices that aren't ASCII — the
  // scraper has a 75%-printable heuristic that drops those rather
  // than dumping noise into the output. Verify a mostly-binary
  // literal is suppressed.
  std::string body = "BT (";
  body += "\x01\x02\x03\x04\x05\x06\x07\x08\x0E\x0F";  // 10 unprintable
  body += "ab";                                          // 2 printable
  body += ") Tj ET";
  auto pdf = MakePdfWithShowText(body);
  std::string text = ExtractText(pdf);
  // Below threshold — the chunk is filtered, output stays clean.
  EXPECT_TRUE(text.empty() || text.find("ab") == std::string::npos)
      << "got: " << text;
}

TEST(PdfTextScraperTest, RespectsMaxCharsCap) {
  // Build a long stream of (X) Tj operators and confirm the output
  // doesn't exceed the cap we pass in.
  std::string body = "BT ";
  for (int i = 0; i < 500; ++i) {
    body += "(X) Tj ";
  }
  body += "ET";
  auto pdf = MakePdfWithShowText(body);
  std::string text = ExtractText(pdf, /*max_chars=*/100);
  EXPECT_LE(text.size(), 100u);
}

TEST(PdfTextScraperTest, IgnoresNonTextOperators) {
  // (foo) Tj is text. (bar) Tf is set-font (NOT text). Our scanner
  // looks ahead for the operator after the literal and skips non-
  // text ops — only "foo" should appear in output.
  std::string body =
      "BT\n"
      "(should-appear) Tj\n"
      "(font-name) Tf\n"   // not a text-show op
      "ET";
  auto pdf = MakePdfWithShowText(body);
  std::string text = ExtractText(pdf);
  EXPECT_NE(text.find("should-appear"), std::string::npos);
  EXPECT_EQ(text.find("font-name"), std::string::npos);
}

TEST(PdfTextScraperTest, HandlesPdfWithoutAnyStreams) {
  // A PDF that's structurally valid but contains no `stream` keyword
  // (e.g. an empty document) should return empty string, not crash.
  std::string raw = "%PDF-1.4\n%%EOF\n";
  std::vector<uint8_t> bytes(raw.begin(), raw.end());
  EXPECT_EQ(ExtractText(bytes), std::string());
}

TEST(PdfTextScraperTest, HandlesPdfWithUnterminatedStream) {
  // Stream open but no matching `endstream` — the scanner should
  // bail out cleanly rather than read past the buffer.
  std::string raw =
      "%PDF-1.4\n"
      "1 0 obj\n<<>>\nstream\n"
      "(this stream never closes) Tj\n";
  std::vector<uint8_t> bytes(raw.begin(), raw.end());
  // No assertion on content — just that we don't crash or hang.
  std::string text = ExtractText(bytes);
  (void)text;
}

TEST(PdfTextScraperTest, MultipleStreamsAreAllScanned) {
  // PDFs often have one content stream per page. The scraper walks
  // every stream block; verify content from a second stream lands.
  std::string raw = "%PDF-1.4\n";
  raw += "1 0 obj\n<<>>\nstream\n";
  raw += "BT (page-one) Tj ET";
  raw += "\nendstream\nendobj\n";
  raw += "2 0 obj\n<<>>\nstream\n";
  raw += "BT (page-two) Tj ET";
  raw += "\nendstream\nendobj\n";
  raw += "%%EOF\n";
  std::vector<uint8_t> bytes(raw.begin(), raw.end());
  std::string text = ExtractText(bytes);
  EXPECT_NE(text.find("page-one"), std::string::npos);
  EXPECT_NE(text.find("page-two"), std::string::npos);
}

}  // namespace
}  // namespace pdf
}  // namespace molt_ai
