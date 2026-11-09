// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Spoken punctuation in dictation (docs/design/TOUCHLESS_BROWSING.md §5.2,
// layer 3): "type see you at noon comma thanks period" types "see you at
// noon, thanks.". The marks are said as words, in the window's language:
//
//   English: comma, period, full stop, question mark, exclamation mark (or
//     point), colon, semicolon, new line, new paragraph, open quote, close
//     quote (end quote, unquote), open and close parenthesis (bracket,
//     paren), hyphen, dash, apostrophe, ellipsis.
//   Spanish: coma, punto, punto y coma, dos puntos, punto y aparte, punto y
//     seguido, puntos suspensivos, abre and cierra interrogación (or
//     exclamación), signo de interrogación (or exclamación), nueva línea,
//     salto de línea, nuevo párrafo, abre and cierra comillas (or
//     paréntesis), guion, raya.
//
// "The word comma" ("la palabra coma") types the word itself. A mark sticks
// to the word before it (", . ? ! : ; )" and closing quotes), an opening one
// to the word after; after ". ? !" and a new line the next word starts with
// a capital letter (ASCII letters only: the rest are left as the
// recognizer wrote them). Recognizers that write punctuation themselves
// pass through unchanged. In English, "CU" (how the recognizer writes "see
// you") is typed as the words.
//
// Pure, std-only and linear; unit-tested in the voice harness and
// molt_ai_unittests.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_DICTATION_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_DICTATION_H_

#include <string>
#include <string_view>

#include "chrome/browser/molt_ai/voice/spoken_text.h"

namespace molt_ai {
namespace voice {

// |text| (dictated, as heard) with its spoken marks written as marks.
std::string ApplySpokenPunctuation(std::string_view text, Language language);

// The field's |value| with |typed| added after it: a space between them,
// except before a mark that sticks to the word before and after a new
// line or a space; |typed| starts with a capital letter after a sentence's
// end.
std::string JoinDictation(std::string_view value, std::string_view typed);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_DICTATION_H_
