// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/pdf/pdf_text_scraper.h"

#include <cstring>
#include <string_view>

#include "third_party/zlib/zlib.h"

namespace molt_ai {
namespace pdf {

namespace {

// Locate the next occurrence of |needle| in |haystack| starting at |from|.
size_t Find(const uint8_t* hay, size_t n, size_t from, const char* needle) {
  size_t nlen = std::strlen(needle);
  if (nlen == 0 || nlen > n) return std::string::npos;
  for (size_t i = from; i + nlen <= n; ++i) {
    if (std::memcmp(hay + i, needle, nlen) == 0) return i;
  }
  return std::string::npos;
}

// Try to zlib-inflate |compressed|. Returns empty string on failure.
std::string TryInflate(const uint8_t* data, size_t len) {
  // Output buffer grows as needed. Start ~4x source.
  std::string out;
  out.resize(len * 4 + 256);
  z_stream zs{};
  zs.next_in = const_cast<Bytef*>(data);
  zs.avail_in = static_cast<uInt>(len);
  if (inflateInit(&zs) != Z_OK) return {};
  size_t total = 0;
  while (true) {
    if (total == out.size()) out.resize(out.size() * 2);
    zs.next_out = reinterpret_cast<Bytef*>(out.data() + total);
    zs.avail_out = static_cast<uInt>(out.size() - total);
    int r = inflate(&zs, Z_NO_FLUSH);
    total = out.size() - zs.avail_out;
    if (r == Z_STREAM_END) break;
    if (r != Z_OK) {
      inflateEnd(&zs);
      return {};
    }
    if (zs.avail_in == 0 && zs.avail_out > 0) break;
  }
  inflateEnd(&zs);
  out.resize(total);
  return out;
}

// Unescape a PDF literal string body (between '(' and ')').
// Handles \n \r \t \b \f \\ \( \) \ddd octal.
std::string UnescapePdfString(std::string_view in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    char c = in[i];
    if (c != '\\') {
      out += c;
      continue;
    }
    if (i + 1 >= in.size()) break;
    char e = in[++i];
    switch (e) {
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case '(': out += '('; break;
      case ')': out += ')'; break;
      case '\\': out += '\\'; break;
      case '\n': /* line continuation, skip */ break;
      case '\r':
        if (i + 1 < in.size() && in[i + 1] == '\n') ++i;
        break;
      default:
        if (e >= '0' && e <= '7') {
          // Octal up to 3 digits.
          int v = e - '0';
          for (int k = 0; k < 2; ++k) {
            if (i + 1 < in.size() && in[i + 1] >= '0' && in[i + 1] <= '7') {
              v = v * 8 + (in[++i] - '0');
            } else {
              break;
            }
          }
          if (v >= 0 && v < 256) out += static_cast<char>(v);
        } else {
          out += e;
        }
        break;
    }
  }
  return out;
}

// Scan |content| for PDF text-show operators and append decoded text
// into |out|. Stops once |out|.size() >= |max_chars|.
void ScanContentForText(std::string_view content,
                        std::string& out,
                        size_t max_chars) {
  // Walk left to right looking for '(' literals or '[' arrays followed
  // by Tj / TJ / ' / ".  We deliberately ignore Tf (font) and Tm
  // (matrix) ops — those don't carry user-visible glyphs.
  size_t i = 0;
  while (i < content.size() && out.size() < max_chars) {
    char c = content[i];
    if (c == '(') {
      // Find matching ')' allowing for nested escapes.
      size_t start = ++i;
      int depth = 1;
      while (i < content.size() && depth > 0) {
        char ch = content[i];
        if (ch == '\\' && i + 1 < content.size()) {
          i += 2;
          continue;
        }
        if (ch == '(') ++depth;
        else if (ch == ')') { if (--depth == 0) break; }
        ++i;
      }
      if (depth != 0) break;
      std::string_view literal(content.data() + start, i - start);
      ++i;  // past ')'
      // Look ahead a few bytes for the operator.
      size_t k = i;
      while (k < content.size() && (content[k] == ' ' || content[k] == '\n' ||
                                     content[k] == '\r' || content[k] == '\t')) {
        ++k;
      }
      bool is_text_op = false;
      if (k + 1 < content.size()) {
        char a = content[k], b = content[k + 1];
        if ((a == 'T' && (b == 'j' || b == 'J')) ||
            a == '\'' || a == '"') {
          is_text_op = true;
        }
      }
      if (is_text_op) {
        std::string decoded = UnescapePdfString(literal);
        // Heuristic: only append if it looks like printable text.
        // Many PDFs use 1-byte glyph indices that aren't ASCII; filter
        // those out to avoid garbage.
        int printable = 0, total = 0;
        for (unsigned char ch : decoded) {
          ++total;
          if (ch >= 32 && ch < 127) ++printable;
          else if (ch == '\n' || ch == '\r' || ch == '\t') ++printable;
        }
        if (total > 0 && printable * 4 >= total * 3) {  // >=75% printable
          out += decoded;
          // Add a space after each show op so adjacent glyph runs don't
          // merge into unreadable words.
          if (!decoded.empty() && decoded.back() != ' ' &&
              decoded.back() != '\n') {
            out += ' ';
          }
        }
      }
    } else {
      ++i;
    }
  }
}

}  // namespace

std::string ExtractText(const std::vector<uint8_t>& pdf_bytes,
                        size_t max_chars) {
  const uint8_t* p = pdf_bytes.data();
  const size_t n = pdf_bytes.size();
  if (n < 8) return {};
  // Quick sanity: PDF files start with "%PDF-" (often after a UTF-8 BOM).
  size_t header = 0;
  if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) header = 3;
  if (n - header < 5 || std::memcmp(p + header, "%PDF-", 5) != 0) return {};

  std::string out;
  out.reserve(std::min<size_t>(max_chars + 1024, 64 * 1024));

  // Walk stream blocks. Each `stream\n` ... `\nendstream` chunk is a
  // candidate content stream.
  size_t pos = 0;
  while (out.size() < max_chars) {
    size_t s = Find(p, n, pos, "stream");
    if (s == std::string::npos) break;
    // The keyword 'stream' may appear inside e.g. /Subtype /XObject
    // metadata; skip over xrefs. Real stream starts immediately followed
    // by EOL.
    size_t data_start = s + 6;
    if (data_start < n && p[data_start] == '\r') ++data_start;
    if (data_start < n && p[data_start] == '\n') ++data_start;

    size_t e = Find(p, n, data_start, "endstream");
    if (e == std::string::npos) break;

    // Trim trailing whitespace before endstream.
    size_t data_end = e;
    while (data_end > data_start &&
           (p[data_end - 1] == '\n' || p[data_end - 1] == '\r' ||
            p[data_end - 1] == ' ')) {
      --data_end;
    }
    if (data_end > data_start) {
      size_t blen = data_end - data_start;
      // Try inflate first (most PDFs use FlateDecode). Fall back to raw.
      std::string inflated = TryInflate(p + data_start, blen);
      std::string_view content =
          inflated.empty()
              ? std::string_view(reinterpret_cast<const char*>(p + data_start),
                                 blen)
              : std::string_view(inflated.data(), inflated.size());
      ScanContentForText(content, out, max_chars);
    }
    pos = e + 9;  // past "endstream"
  }
  if (out.size() > max_chars) out.resize(max_chars);
  // Trim runs of whitespace longer than 2 to keep the text readable in
  // chat. Cheap pass.
  std::string clean;
  clean.reserve(out.size());
  int ws = 0;
  for (char ch : out) {
    if (ch == ' ' || ch == '\t' || ch == '\r') {
      if (++ws <= 1) clean += ' ';
    } else if (ch == '\n') {
      if (clean.empty() || clean.back() != '\n') clean += '\n';
      ws = 0;
    } else {
      clean += ch;
      ws = 0;
    }
  }
  return clean;
}

}  // namespace pdf
}  // namespace molt_ai
