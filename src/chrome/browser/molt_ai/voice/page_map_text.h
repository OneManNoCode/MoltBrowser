// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Page text made safe to speak and show, and the simple field types the
// page map gives a list entry's text (docs/design/TOUCHLESS_BROWSING.md
// §6.3): durations, counts and dates.
//
// Every string here comes from a web page, so it is untrusted: it may be
// invalid UTF-8, hold control, bidi or invisible characters, or be very
// long. The parsers are linear scanners (no std::regex on page content) and
// read a bounded prefix of their input.
//
// Fields are read in the page's language (FieldLanguageOf() of the
// document's lang, never the voice's): English's words, or Spanish's
// ("hace 2 años", "1,2 M de visualizaciones", "6 minutos y 7 segundos").
// English is the default, and reads exactly as it always has.
//
// Std-only; unit-tested in molt_ai_unittests (page_map_text_unittest.cc)
// and fuzzed with the page map (molt_page_map_fuzzer).

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_PAGE_MAP_TEXT_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_PAGE_MAP_TEXT_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace molt_ai {
namespace voice {

// SpeechSafeText() reads at most this many bytes of its input.
inline constexpr size_t kMaxSpeechScanBytes = 8192;

// |text| as something to speak or show:
//   - valid UTF-8 (an invalid byte reads as a space);
//   - no C0 or C1 control character, DEL, or any character
//     hidden_chars::HiddenCharAt() names (bidi controls, zero-width and
//     other invisible characters, tag characters, variation selectors), no
//     noncharacter, private-use or object-replacement character; tabs, line
//     breaks and Unicode spaces become spaces;
//   - runs of spaces collapsed to one, no space at either end;
//   - at most |max_chars| code points: a longer text is cut, at the last
//     space when one is within the last 16 kept code points.
// Only the first kMaxSpeechScanBytes bytes are read.
std::string SpeechSafeText(std::string_view text, size_t max_chars);

// The user's text (dictation) as it may be written into a page's field:
// valid UTF-8 (an invalid byte is dropped), with no control character,
// DEL or character hidden_chars::HiddenCharAt() names; a line break (CR,
// LF, CRLF, U+2028, U+2029) stays as "\n" in a |multiline| field and is a
// space otherwise; tabs are spaces. Nullopt if the result would be longer
// than |max_bytes| bytes.
std::optional<std::string> TypedText(std::string_view text,
                                     bool multiline,
                                     size_t max_bytes);

// The number of code points in |utf8| (each byte that doesn't continue a
// character counts once).
size_t CountCodePoints(std::string_view utf8);

// The words a page's fields are read with.
enum class FieldLanguage {
  kEnglish,
  kSpanish,
};
// The page's language tag ("es", "es-419", "ES_mx", any case) as field
// words: Spanish for Spanish, English for anything else (or none).
FieldLanguage FieldLanguageOf(std::string_view language_tag);

// A duration as pages show one: "6:07", "1:02:03", "6 minutes, 7
// seconds", "1 hr 2 min", "PT6M7S"; in Spanish "6 minutos y 7 segundos",
// "1 h 2 min". The whole of |text| must be the duration (spaces at either
// end aside). Seconds, at most 1000 hours.
std::optional<int> ParseDuration(
    std::string_view text,
    FieldLanguage language = FieldLanguage::kEnglish);

// A count: a number, an optional multiplier ("K", "M", "B", "thousand",
// "million", "billion") and a unit of one or two words that isn't a unit of
// time: "2.1B views", "1,234 subscribers", "12K likes", "No views". In
// Spanish: a decimal comma or point, "." or "," between groups of three,
// "k", "mil", "M", "mill.", "millones", "mil M" and "mil millones", "de"
// before the unit, and "Sin" for none: "1,2 M de visualizaciones", "15 mil
// suscriptores", "1.234 vistas", "Sin visualizaciones".
struct ParsedCount {
  int64_t value = 0;
  // The unit in lower case, with one space between two words ("views").
  std::string unit;
};
std::optional<ParsedCount> ParseCount(
    std::string_view text,
    FieldLanguage language = FieldLanguage::kEnglish);

// Whether |text| is a date as pages show one, and nothing else: "15 years
// ago", "Streamed 2 years ago", "yesterday", "Jan 5, 2024", "5 January
// 2024", "2024-01-05", "1/5/2024". In Spanish: "hace 2 años", "Emitido
// hace 3 días", "ayer", "la semana pasada", "5 ene 2024", "5 de enero de
// 2024", "05/01/2024".
bool LooksLikeDate(std::string_view text,
                   FieldLanguage language = FieldLanguage::kEnglish);

// Whether |text| reads as a web address ("https://...", "www....", or a
// breadcrumb such as "en.wikipedia.org › wiki"), which isn't worth
// speaking as a field.
bool LooksLikeAddress(std::string_view text);

// Whether |text| has no letter or digit ("•", "|", "-").
bool HasNoWords(std::string_view text);

// ASCII lower case of |text| with runs of spaces collapsed, for comparing
// texts.
std::string FoldForCompare(std::string_view text);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_PAGE_MAP_TEXT_H_
