// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/services/molt_ai/document_parser/document_parser_impl.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/strings/string_util.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/zlib/zlib.h"

namespace molt_ai {
namespace {

using document_parser_internal::SanitizeExtension;
using document_parser_internal::ToValidUtf8;

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
