// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "components/omnibox/browser/molt_memory_query.h"

#include <algorithm>
#include <cstdint>

namespace molt_memory {

namespace {

constexpr char32_t kReplacementCharacter = 0xFFFD;
constexpr size_t kNotFound = static_cast<size_t>(-1);

// The next code point of |s| at |*i|, advancing |*i|. A byte that doesn't
// start a well-formed, shortest-form, non-surrogate sequence is one U+FFFD.
char32_t NextFromUtf8(std::string_view s, size_t* i) {
  const unsigned char c0 = static_cast<unsigned char>(s[*i]);
  if (c0 < 0x80) {
    ++*i;
    return c0;
  }
  size_t n;
  char32_t cp;
  char32_t min;
  if ((c0 & 0xE0) == 0xC0) {
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
    ++*i;
    return kReplacementCharacter;
  }
  if (n > s.size() - *i) {
    ++*i;
    return kReplacementCharacter;
  }
  for (size_t k = 1; k < n; ++k) {
    const unsigned char ck = static_cast<unsigned char>(s[*i + k]);
    if ((ck & 0xC0) != 0x80) {
      ++*i;
      return kReplacementCharacter;
    }
    cp = (cp << 6) | (ck & 0x3F);
  }
  if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
    ++*i;
    return kReplacementCharacter;
  }
  *i += n;
  return cp;
}

// The next code point of |s| at |*i|; an unpaired surrogate is U+FFFD.
char32_t NextFromUtf16(std::u16string_view s, size_t* i) {
  const char16_t c = s[(*i)++];
  if (c < 0xD800 || c > 0xDFFF) {
    return c;
  }
  if (c <= 0xDBFF && *i < s.size()) {
    const char16_t d = s[*i];
    if (d >= 0xDC00 && d <= 0xDFFF) {
      ++*i;
      return 0x10000 + ((static_cast<char32_t>(c) - 0xD800) << 10) +
             (static_cast<char32_t>(d) - 0xDC00);
    }
  }
  return kReplacementCharacter;
}

// |cp| is a Unicode scalar value.
void AppendUtf8(char32_t cp, std::string* out) {
  if (cp < 0x80) {
    out->push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

// Whitespace: ASCII, NEL, NBSP, Ogham, the U+2000 spaces, line and
// paragraph separators, narrow NBSP, medium math space, ideographic space.
bool IsSpace(char32_t c) {
  return c == ' ' || (c >= 0x09 && c <= 0x0D) || c == 0x85 || c == 0xA0 ||
         c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 ||
         c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

// Never shown or searched for: the other C0 controls, DEL and C1 controls,
// bidirectional formatting (marks, embeddings, overrides, isolates: they can
// reorder what the omnibox shows) and the byte order mark.
bool IsDropped(char32_t c) {
  return c < 0x20 || (c >= 0x7F && c <= 0x9F) || c == 0x061C || c == 0x200E ||
         c == 0x200F || (c >= 0x202A && c <= 0x202E) ||
         (c >= 0x2066 && c <= 0x2069) || c == 0xFEFF;
}

// The normalization of NormalizeText() over code points from |next| (which
// returns false at the end). Stops reading once the result is full, so the
// work is bounded by |max_characters| plus the dropped characters and
// spaces in between.
template <typename Next>
std::string Normalize(Next next, size_t max_characters) {
  std::string out;
  size_t count = 0;
  bool pending_space = false;
  char32_t c = 0;
  while (count < max_characters && next(&c)) {
    if (IsSpace(c)) {
      pending_space = true;
      continue;
    }
    if (IsDropped(c)) {
      continue;
    }
    if (pending_space && !out.empty()) {
      // A space only when a character can follow it.
      if (count + 2 > max_characters) {
        break;
      }
      out.push_back(' ');
      ++count;
    }
    pending_space = false;
    AppendUtf8(c, &out);
    ++count;
  }
  return out;
}

char16_t AsciiLower(char16_t c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char16_t>(c + ('a' - 'A')) : c;
}

size_t SkipSpaces(std::u16string_view text) {
  size_t i = 0;
  while (i < text.size() && IsSpace(text[i])) {
    ++i;
  }
  return i;
}

// Where the words start when |text| is shortcut input (just after
// "@memory"), else kNotFound.
size_t KeywordEnd(std::u16string_view text) {
  constexpr std::u16string_view kKeyword = kShortcutKeyword;
  size_t i = SkipSpaces(text);
  if (text.size() - i < kKeyword.size()) {
    return kNotFound;
  }
  for (size_t k = 0; k < kKeyword.size(); ++k) {
    if (AsciiLower(text[i + k]) != kKeyword[k]) {
      return kNotFound;
    }
  }
  i += kKeyword.size();
  if (i < text.size() && !IsSpace(text[i])) {
    return kNotFound;  // "@memoryfoo"
  }
  return i;
}

bool IsHexDigit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  return c - 'A' + 10;
}

// application/x-www-form-urlencoded decoding of one name or value.
std::string FormDecode(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '+') {
      out.push_back(' ');
    } else if (c == '%' && s.size() - i > 2 && IsHexDigit(s[i + 1]) &&
               IsHexDigit(s[i + 2])) {
      out.push_back(
          static_cast<char>(HexValue(s[i + 1]) * 16 + HexValue(s[i + 2])));
      i += 2;
    } else {
      out.push_back(c);
    }
  }
  return out;
}

bool IsUnreserved(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
         c == '~';
}

bool StartsWithAsciiIgnoreCase(std::string_view s, std::string_view prefix) {
  if (s.size() < prefix.size()) {
    return false;
  }
  for (size_t i = 0; i < prefix.size(); ++i) {
    char c = s[i];
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c + ('a' - 'A'));
    }
    if (c != prefix[i]) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool IsShortcutInput(std::u16string_view text) {
  return KeywordEnd(text) != kNotFound;
}

bool IsShortcutPrefix(std::u16string_view text) {
  constexpr std::u16string_view kKeyword = kShortcutKeyword;
  const std::u16string_view rest = text.substr(SkipSpaces(text));
  if (rest.empty() || rest.size() >= kKeyword.size()) {
    return false;
  }
  for (size_t k = 0; k < rest.size(); ++k) {
    if (AsciiLower(rest[k]) != kKeyword[k]) {
      return false;
    }
  }
  return true;
}

std::string ShortcutQuery(std::u16string_view text) {
  const size_t start = KeywordEnd(text);
  if (start == kNotFound) {
    return std::string();
  }
  const std::u16string_view words = text.substr(start);
  size_t i = 0;
  return Normalize(
      [&](char32_t* c) {
        if (i >= words.size()) {
          return false;
        }
        *c = NextFromUtf16(words, &i);
        return true;
      },
      kMaxQueryCharacters);
}

std::string NormalizeText(std::string_view utf8, size_t max_characters) {
  size_t i = 0;
  return Normalize(
      [&](char32_t* c) {
        if (i >= utf8.size()) {
          return false;
        }
        *c = NextFromUtf8(utf8, &i);
        return true;
      },
      max_characters);
}

std::string NormalizeQuery(std::string_view utf8) {
  return NormalizeText(utf8, kMaxQueryCharacters);
}

std::optional<std::string> QueryFromUrlQuery(std::string_view url_query) {
  size_t pos = 0;
  while (pos <= url_query.size()) {
    size_t end = url_query.find('&', pos);
    if (end == std::string_view::npos) {
      end = url_query.size();
    }
    const std::string_view param = url_query.substr(pos, end - pos);
    const size_t eq = param.find('=');
    const std::string_view name = param.substr(0, eq);
    // Only "q" itself, "%71" or "%51"-like spellings can decode to "q".
    if (!name.empty() && name.size() <= 3 && FormDecode(name) == "q") {
      const std::string_view value = eq == std::string_view::npos
                                         ? std::string_view()
                                         : param.substr(eq + 1);
      std::string query = NormalizeQuery(FormDecode(value));
      if (query.empty()) {
        return std::nullopt;
      }
      return query;
    }
    pos = end + 1;
  }
  return std::nullopt;
}

std::string MemorySearchUrl(std::string_view query) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  const std::string normalized = NormalizeQuery(query);
  std::string url = kMemoryPageUrl;
  if (normalized.empty()) {
    return url;
  }
  url += "?q=";
  for (char ch : normalized) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (IsUnreserved(c)) {
      url.push_back(ch);
    } else {
      url.push_back('%');
      url.push_back(kHex[c >> 4]);
      url.push_back(kHex[c & 0x0F]);
    }
  }
  return url;
}

ShortcutState ShortcutStateFor(bool off_the_record,
                               bool guest_session,
                               bool has_memory,
                               bool enabled,
                               bool paused) {
  // Checked even though such profiles have no memory: nothing about memory
  // may show in them.
  if (off_the_record || guest_session || !has_memory) {
    return ShortcutState::kHidden;
  }
  if (!enabled) {
    return ShortcutState::kOff;
  }
  if (paused) {
    return ShortcutState::kPaused;
  }
  return ShortcutState::kReady;
}

bool IsSuggestibleUrl(std::string_view url) {
  if (url.size() > kMaxUrlBytes) {
    return false;
  }
  size_t host_start;
  if (StartsWithAsciiIgnoreCase(url, "https://")) {
    host_start = 8;
  } else if (StartsWithAsciiIgnoreCase(url, "http://")) {
    host_start = 7;
  } else {
    return false;
  }
  if (url.size() == host_start) {
    return false;
  }
  const char first = url[host_start];
  if (first == '/' || first == '\\' || first == '?' || first == '#') {
    return false;
  }
  for (char ch : url) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (c <= 0x20 || c >= 0x7F) {
      return false;
    }
  }
  return true;
}

std::vector<SuggestionRow> SuggestionRows(const std::vector<MemoryHit>& hits,
                                          size_t max_rows) {
  std::vector<SuggestionRow> rows;
  for (const MemoryHit& hit : hits) {
    if (rows.size() >= max_rows) {
      break;
    }
    if (!IsSuggestibleUrl(hit.url)) {
      continue;
    }
    if (std::any_of(rows.begin(), rows.end(),
                    [&](const SuggestionRow& r) { return r.url == hit.url; })) {
      continue;
    }
    SuggestionRow row;
    row.url = hit.url;
    row.title = NormalizeText(hit.title, kMaxTitleCharacters);
    if (row.title.empty()) {
      row.title = NormalizeText(hit.url, kMaxTitleCharacters);
    }
    const int step = static_cast<int>(
        std::min<size_t>(rows.size(), kFirstHitRelevance / kHitRelevanceStep));
    row.relevance = std::max(1, kFirstHitRelevance - kHitRelevanceStep * step);
    rows.push_back(std::move(row));
  }
  return rows;
}

}  // namespace molt_memory
