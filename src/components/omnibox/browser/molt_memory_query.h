// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The pure logic of MoltBrowser's @memory omnibox shortcut
// (docs/design/SEMANTIC_SEARCH.md, "The @memory omnibox shortcut"):
// recognizing "@memory <words>", molt://memory/?q= in both directions, the
// profile and Memory-state gate, and turning memory hits into display-safe
// suggestion rows.
//
// Standard library only. The autocomplete controller's gate
// (autocomplete_controller.cc), the omnibox client's hooks
// (chrome_omnibox_client.cc), MoltMemoryProvider, molt://memory's handler and
// molt_memory_query_fuzzer all use it, and it builds without //base.

#ifndef COMPONENTS_OMNIBOX_BROWSER_MOLT_MEMORY_QUERY_H_
#define COMPONENTS_OMNIBOX_BROWSER_MOLT_MEMORY_QUERY_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace molt_memory {

// The shortcut, matched ASCII-case-insensitively.
inline constexpr char16_t kShortcutKeyword[] = u"@memory";

// Longest query, in characters (molt://memory's search box maxlength).
inline constexpr size_t kMaxQueryCharacters = 500;
// Longest title shown in a suggestion, in characters.
inline constexpr size_t kMaxTitleCharacters = 256;
// Longest URL a suggestion may navigate to, in bytes.
inline constexpr size_t kMaxUrlBytes = 4096;

// molt://memory as the browser loads it. Must equal
// chrome::kChromeUIMoltAIMemoryURL (memory_omnibox_provider_unittest.cc
// checks).
inline constexpr char kMemoryPageUrl[] = "chrome://molt-memory/";

// ---- The shortcut in the omnibox text ----

// True iff |text|, after leading whitespace, is "@memory" alone or followed
// by whitespace. Such input belongs to the shortcut: no other omnibox
// provider sees it, in any profile and whatever Memory's state.
bool IsShortcutInput(std::u16string_view text);

// True iff |text| (after leading whitespace) is a non-empty proper prefix of
// "@memory": "@", "@m" ... "@memor". The omnibox may hint at the shortcut.
bool IsShortcutPrefix(std::u16string_view text);

// The words after "@memory" in |text|, as NormalizeQuery() makes them; ""
// when |text| is not shortcut input or has no words after the keyword.
std::string ShortcutQuery(std::u16string_view text);

// ---- Queries and molt://memory/?q= ----

// |utf8| made safe to search for and to show: invalid UTF-8 becomes U+FFFD;
// control characters (C0, DEL, C1), bidirectional formatting characters and
// byte order marks are dropped; every run of whitespace (Unicode spaces,
// line and paragraph separators included) becomes one space; leading and
// trailing spaces are trimmed; the result is cut to |max_characters| on a
// character boundary. Idempotent.
std::string NormalizeText(std::string_view utf8, size_t max_characters);

// NormalizeText() with kMaxQueryCharacters.
std::string NormalizeQuery(std::string_view utf8);

// The "q" parameter of a URL's query component (|url_query|, without the
// '?'), decoded as application/x-www-form-urlencoded ('+' is a space, %XX a
// byte, a malformed escape stays as it is) and normalized. The first "q"
// wins, like URLSearchParams.get(). std::nullopt when there is no "q" or it
// is empty once normalized. Linear in |url_query|.
std::optional<std::string> QueryFromUrlQuery(std::string_view url_query);

// "chrome://molt-memory/?q=<NormalizeQuery(query)>", with every byte but
// A-Z a-z 0-9 - . _ ~ percent-encoded; kMemoryPageUrl when the normalized
// query is empty. QueryFromUrlQuery() of what follows "?" gives
// NormalizeQuery(query) back.
std::string MemorySearchUrl(std::string_view query);

// ---- Who gets the shortcut ----

// What the shortcut does in a profile.
enum class ShortcutState {
  // Nothing at all: no hint, no row, no search. "@memory ..." still reaches
  // no other provider (IsShortcutInput()).
  kHidden,
  // Memory is off: one row that opens molt://memory; nothing is searched.
  kOff,
  // Memory is paused: likewise.
  kPaused,
  // Searches memory.
  kReady,
};

// |off_the_record| and |guest_session| describe the omnibox's profile
// (Incognito, MoltNet and the Guest session are off the record);
// |has_memory| is whether that profile has an AI memory (only regular
// profiles do); |enabled| and |paused| are Memory's settings.
ShortcutState ShortcutStateFor(bool off_the_record,
                               bool guest_session,
                               bool has_memory,
                               bool enabled,
                               bool paused);

// ---- Suggestion rows ----

// A page memory returned, as stored: untrusted text.
struct MemoryHit {
  std::string url;
  std::string title;
};

// One suggestion: an http(s) URL to navigate to and a display-safe title.
struct SuggestionRow {
  std::string url;
  // NormalizeText(title, kMaxTitleCharacters), or the same of the URL when
  // that is empty.
  std::string title;
  int relevance = 0;
};

// Relevance of the "Search memory for ..." row (the default match) and of
// the first hit; each later hit gets kHitRelevanceStep less.
inline constexpr int kSearchRowRelevance = 1500;
inline constexpr int kFirstHitRelevance = 1400;
inline constexpr int kHitRelevanceStep = 10;

// True iff |url| may become a suggestion's destination: "http://" or
// "https://" (any case) followed by a host, at most kMaxUrlBytes, and no
// space, control or non-ASCII byte (stored URLs are canonical). The provider
// also requires a valid GURL with an http(s) scheme.
bool IsSuggestibleUrl(std::string_view url);

// At most |max_rows| rows for |hits|, best first: only suggestible URLs, each
// URL once (the first, best-ranked hit wins), titles normalized.
std::vector<SuggestionRow> SuggestionRows(const std::vector<MemoryHit>& hits,
                                          size_t max_rows);

}  // namespace molt_memory

#endif  // COMPONENTS_OMNIBOX_BROWSER_MOLT_MEMORY_QUERY_H_
