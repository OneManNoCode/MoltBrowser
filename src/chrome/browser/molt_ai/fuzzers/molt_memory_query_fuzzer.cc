// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The @memory omnibox shortcut's parsers
// (components/omnibox/browser/molt_memory_query.h) on any input: the query
// component of a molt://memory/?q= URL (whoever typed or bookmarked it),
// omnibox text (the user, or a paste), and the URLs and titles of the pages
// memory returns (sites).
//
// Checks: no memory or UB error or timeout; every query or title that comes
// out is "display-safe": valid UTF-8, within its character cap, no control,
// bidirectional-formatting or BOM character, single spaces only between
// characters, and normalizing it again changes nothing; a parsed query is
// never empty; MemorySearchUrl() escapes everything but unreserved bytes
// and QueryFromUrlQuery() gives its query back; only "@memory" input has a
// shortcut query, and no input is both shortcut input and a keyword
// prefix; the controller's gate (molt_local_input.h) tells @memory and @ai
// apart consistently and never claims text starting with "?" (a web
// search); suggestion rows only have http(s) URLs that pass
// IsSuggestibleUrl(), each once, within the row cap, with positive,
// non-increasing relevance below the search row's.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fuzzer/FuzzedDataProvider.h>

#include "base/check.h"
#include "base/check_op.h"
#include "components/omnibox/browser/molt_local_input.h"
#include "components/omnibox/browser/molt_memory_query.h"

namespace {

using molt_memory::kMaxQueryCharacters;
using molt_memory::kMaxTitleCharacters;

// Strict UTF-8 decoding (shortest form, no surrogates); false if invalid.
bool DecodeUtf8(std::string_view s, std::vector<char32_t>* out) {
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c0 = static_cast<unsigned char>(s[i]);
    size_t n;
    char32_t cp;
    char32_t min;
    if (c0 < 0x80) {
      out->push_back(c0);
      ++i;
      continue;
    } else if ((c0 & 0xE0) == 0xC0) {
      n = 2;
      cp = c0 & 0x1F;
      min = 0x80;
    } else if ((c0 & 0xF0) == 0xE0) {
      n = 3;
      cp = c0 & 0x0F;
      min = 0x800;
    } else if ((c0 & 0xF8) == 0xF0) {
      n = 4;
      cp = c0 & 0x07;
      min = 0x10000;
    } else {
      return false;
    }
    if (n > s.size() - i) return false;
    for (size_t k = 1; k < n; ++k) {
      const unsigned char ck = static_cast<unsigned char>(s[i + k]);
      if ((ck & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (ck & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
      return false;
    }
    out->push_back(cp);
    i += n;
  }
  return true;
}

bool IsUnsafe(char32_t c) {
  return c < 0x20 || (c >= 0x7F && c <= 0x9F) || c == 0x061C ||
         c == 0x200E || c == 0x200F || (c >= 0x202A && c <= 0x202E) ||
         (c >= 0x2066 && c <= 0x2069) || c == 0xFEFF;
}

bool IsOtherSpace(char32_t c) {
  return c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
         c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F ||
         c == 0x3000;
}

void CheckDisplaySafe(const std::string& s, size_t max_characters) {
  std::vector<char32_t> cps;
  CHECK(DecodeUtf8(s, &cps)) << "invalid UTF-8";
  CHECK_LE(cps.size(), max_characters);
  for (size_t i = 0; i < cps.size(); ++i) {
    CHECK(!IsUnsafe(cps[i])) << "unsafe character U+" << std::hex
                             << static_cast<uint32_t>(cps[i]);
    CHECK(!IsOtherSpace(cps[i])) << "space other than U+0020";
    if (cps[i] == ' ') {
      CHECK(i > 0 && i + 1 < cps.size()) << "leading or trailing space";
      CHECK(cps[i + 1] != ' ') << "two spaces";
    }
  }
  CHECK(molt_memory::NormalizeText(s, max_characters) == s)
      << "normalizing again changed it";
}

bool IsUnreserved(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
         c == '~';
}

bool IsUpperHex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
}

// The query component of a URL MemorySearchUrl() made, checked.
std::string CheckSearchUrl(const std::string& url) {
  const std::string_view page = molt_memory::kMemoryPageUrl;
  CHECK(url.compare(0, page.size(), page) == 0) << "not molt://memory";
  if (url.size() == page.size()) return std::string();
  CHECK(url.compare(page.size(), 3, "?q=") == 0);
  const std::string encoded = url.substr(page.size() + 3);
  CHECK(!encoded.empty());
  for (size_t i = 0; i < encoded.size(); ++i) {
    if (encoded[i] == '%') {
      CHECK(i + 2 < encoded.size() && IsUpperHex(encoded[i + 1]) &&
            IsUpperHex(encoded[i + 2]))
          << "bad escape";
      i += 2;
    } else {
      CHECK(IsUnreserved(encoded[i])) << "unescaped byte";
    }
  }
  return url.substr(page.size() + 1);
}

char16_t Lower(char16_t c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char16_t>(c + 32) : c;
}

// The gate's decision for |text| is consistent: memory and AI prompts are
// told apart, the prompt starts inside the text right after "@ai" and one
// character, and text starting with "?" is never local.
void CheckLocalInput(const std::u16string& text) {
  const molt_omnibox::LocalInput kind = molt_omnibox::ClassifyLocalInput(text);
  const bool memory = molt_memory::IsShortcutInput(text);
  const std::optional<size_t> ai = molt_omnibox::AiPromptStart(text);
  CHECK(!(memory && ai)) << "both @memory and @ai";
  CHECK(molt_omnibox::IsLocalOnlyInput(text) == (memory || ai.has_value()));
  if (memory) {
    CHECK(kind == molt_omnibox::LocalInput::kMemoryShortcut);
  } else if (ai) {
    CHECK(kind == molt_omnibox::LocalInput::kAiPrompt);
    CHECK(*ai >= 4 && *ai <= text.size());
    const size_t at = *ai - 4;
    CHECK(text[at] == '@' && Lower(text[at + 1]) == 'a' &&
          Lower(text[at + 2]) == 'i');
  } else {
    CHECK(kind == molt_omnibox::LocalInput::kNone);
  }
  if (!text.empty() && text[0] == '?') {
    CHECK(kind == molt_omnibox::LocalInput::kNone) << "\"?\" is a web search";
  }
}

std::u16string ToUtf16Units(const std::string& bytes) {
  std::u16string out;
  for (size_t i = 0; i + 1 < bytes.size(); i += 2) {
    out.push_back(static_cast<char16_t>(
        static_cast<unsigned char>(bytes[i]) |
        (static_cast<unsigned char>(bytes[i + 1]) << 8)));
  }
  return out;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  FuzzedDataProvider p(data, size);

  // 1. molt://memory/?<query component>.
  const std::string url_query = p.ConsumeRandomLengthString(2048);
  if (std::optional<std::string> q =
          molt_memory::QueryFromUrlQuery(url_query)) {
    CHECK(!q->empty());
    CheckDisplaySafe(*q, kMaxQueryCharacters);
    CHECK(molt_memory::NormalizeQuery(*q) == *q);
    // Building the URL from it and parsing that gives it back.
    const std::string back = CheckSearchUrl(molt_memory::MemorySearchUrl(*q));
    CHECK(molt_memory::QueryFromUrlQuery(back) == q) << "round trip";
  }

  // 2. Any text, normalized and as a search URL.
  const std::string text = p.ConsumeRandomLengthString(1024);
  const std::string normalized = molt_memory::NormalizeQuery(text);
  CheckDisplaySafe(normalized, kMaxQueryCharacters);
  const std::string query_part =
      CheckSearchUrl(molt_memory::MemorySearchUrl(text));
  if (normalized.empty()) {
    CHECK(query_part.empty());
  } else {
    CHECK(molt_memory::QueryFromUrlQuery(query_part) == normalized);
  }

  // 3. Omnibox text (UTF-16 code units, unpaired surrogates included).
  const std::u16string omnibox =
      ToUtf16Units(p.ConsumeRandomLengthString(1024));
  const bool shortcut = molt_memory::IsShortcutInput(omnibox);
  const bool prefix = molt_memory::IsShortcutPrefix(omnibox);
  CHECK(!(shortcut && prefix));
  const std::string words = molt_memory::ShortcutQuery(omnibox);
  if (!shortcut) {
    CHECK(words.empty());
  }
  CheckDisplaySafe(words, kMaxQueryCharacters);
  const std::u16string with_keyword = u"@memory " + omnibox;
  CHECK(molt_memory::IsShortcutInput(with_keyword));
  CheckDisplaySafe(molt_memory::ShortcutQuery(with_keyword),
                   kMaxQueryCharacters);

  // Local-only input (molt_local_input.h): the controller's gate.
  CheckLocalInput(omnibox);
  CheckLocalInput(u"@ai " + omnibox);
  CheckLocalInput(u"?" + omnibox);
  CHECK(molt_omnibox::ClassifyLocalInput(u"@ai " + omnibox) ==
        molt_omnibox::LocalInput::kAiPrompt);

  // 4. Pages memory returned: URLs and titles from sites.
  std::vector<molt_memory::MemoryHit> hits;
  const size_t count = p.ConsumeIntegralInRange<size_t>(0, 12);
  for (size_t i = 0; i < count; ++i) {
    molt_memory::MemoryHit hit;
    hit.url = p.ConsumeBool()
                  ? "https://a.example/" + p.ConsumeRandomLengthString(64)
                  : p.ConsumeRandomLengthString(128);
    hit.title = p.ConsumeRandomLengthString(512);
    hits.push_back(std::move(hit));
  }
  const size_t max_rows = p.ConsumeIntegralInRange<size_t>(0, 10);
  const std::vector<molt_memory::SuggestionRow> rows =
      molt_memory::SuggestionRows(hits, max_rows);
  CHECK_LE(rows.size(), max_rows);
  for (size_t i = 0; i < rows.size(); ++i) {
    CHECK(molt_memory::IsSuggestibleUrl(rows[i].url));
    CHECK(!rows[i].title.empty());
    CheckDisplaySafe(rows[i].title, kMaxTitleCharacters);
    CHECK_GE(rows[i].relevance, 1);
    CHECK_LT(rows[i].relevance, molt_memory::kSearchRowRelevance);
    for (size_t j = 0; j < i; ++j) {
      CHECK(rows[j].url != rows[i].url) << "duplicate URL";
      CHECK_GE(rows[j].relevance, rows[i].relevance);
    }
  }
  return 0;
}
