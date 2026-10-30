// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// What the browser's own voice says for a text, as a recognizer that heard
// it would write it: the words of molt-voice's text front end
// (voice_helper/voice_text.h, the same code the helper runs), so
// "example.com" is "example dot com", "2nd" is "second", "&" is "and" and
// "U.S." is "u s". Words written together in camel case are said apart
// ("MoltBrowser" is "molt browser"), as the helper's phonemizer says them,
// and short words in capitals are given both ways ("GPU" is "gpu g p u").
//
// voice_session_logic.h compares what the recognizer heard while the
// browser spoke with what the browser said: an echo the canceller missed
// is the spoken words, not the written ones. The platform's voices read
// numbers, addresses and symbols much the same way.
//
// Std-only: molt_ai_unittests, a standalone harness and the voice listener
// fuzzer build the same code.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_SAID_ALOUD_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_SAID_ALOUD_H_

#include <stddef.h>

#include <string>
#include <string_view>

namespace molt_ai {
namespace voice {

// The words said for |text| (any bytes; only what the voice reads of it),
// lower case ASCII, one space between them, at most |max_bytes| bytes (cut
// between words).
std::string SaidAloudWords(std::string_view text, size_t max_bytes);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_SAID_ALOUD_H_
