// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/services/molt_ai/document_parser/docx_text_extractor.h"

#include <algorithm>
#include <string_view>

#include "base/files/file_path.h"
#include "base/strings/string_util.h"
#include "third_party/zlib/google/zip_reader.h"

namespace molt_ai::docx {

// Same hand-rolled-scanner altitude as pdf_text_scraper: no XML parser
// dependency needed.
std::string StripWordprocessingXml(const std::string& xml) {
  std::string out;
  bool in_text = false;
  size_t i = 0;
  while (i < xml.size()) {
    if (xml[i] != '<') {
      if (in_text) {
        out += xml[i];
      }
      ++i;
      continue;
    }
    size_t close = xml.find('>', i);
    if (close == std::string::npos) {
      break;
    }
    std::string_view tag(xml.data() + i + 1, close - i - 1);
    const bool self_closing = !tag.empty() && tag.back() == '/';
    if (self_closing) {
      tag.remove_suffix(1);
    }
    const size_t space = tag.find_first_of(" \t\r\n");
    const std::string_view tag_name =
        space == std::string_view::npos ? tag : tag.substr(0, space);
    if (tag_name == "w:t") {
      in_text = !self_closing;
    } else if (tag_name == "/w:t") {
      in_text = false;
    } else if (tag_name == "/w:p") {
      out += '\n';
    } else if (tag_name == "w:tab") {
      out += '\t';
    } else if (tag_name == "w:br" || tag_name == "w:cr") {
      out += '\n';
    }
    i = close + 1;
  }
  // Decode the predefined XML entities. &amp; must go last so an
  // escaped "&amp;lt;" decodes to a literal "&lt;", not a fake tag.
  base::ReplaceSubstringsAfterOffset(&out, 0, "&lt;", "<");
  base::ReplaceSubstringsAfterOffset(&out, 0, "&gt;", ">");
  base::ReplaceSubstringsAfterOffset(&out, 0, "&quot;", "\"");
  base::ReplaceSubstringsAfterOffset(&out, 0, "&apos;", "'");
  base::ReplaceSubstringsAfterOffset(&out, 0, "&amp;", "&");
  return out;
}

// In-memory: zlib inflate via zip::ZipReader::OpenFromString, no temp files.
std::string ExtractDocxText(const std::string& zip_bytes, std::string* error) {
  zip::ZipReader reader;
  if (!reader.OpenFromString(zip_bytes)) {
    *error = "not a valid .docx (could not open as a zip archive)";
    return std::string();
  }
  bool found = false;
  std::string xml;
  while (const zip::ZipReader::Entry* entry = reader.Next()) {
    if (entry->is_directory || entry->is_encrypted) {
      continue;
    }
    // Zip entry paths use '/', but base::FilePath may hold either
    // separator on Windows — compare a normalized UTF-8 form.
    std::string path = entry->path.AsUTF8Unsafe();
    std::replace(path.begin(), path.end(), '\\', '/');
    if (path != "word/document.xml") {
      continue;
    }
    found = true;
    const bool ok = reader.ExtractCurrentEntryToString(kMaxDocxXmlBytes, &xml);
    if (!ok && xml.size() < kMaxDocxXmlBytes) {
      // Real inflate error (a full-to-the-cap buffer just means the
      // XML hit the read cap; the char budget truncates below that
      // anyway, so we keep what we got).
      *error = "failed to read word/document.xml from the archive";
      return std::string();
    }
    break;
  }
  if (!found) {
    *error = "no word/document.xml entry — is this really a .docx file?";
    return std::string();
  }
  std::string text = StripWordprocessingXml(xml);
  if (text.find_first_not_of(" \t\r\n") == std::string::npos) {
    *error = "document contains no extractable text";
    return std::string();
  }
  if (!base::IsStringUTF8(text)) {
    *error = "document text is not valid UTF-8";
    return std::string();
  }
  return text;
}

}  // namespace molt_ai::docx
