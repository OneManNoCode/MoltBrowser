// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Characters that reorder, hide or break text, for the text a user approves
// or reads from the agents: the web agent's site-tool approvals and step
// trace (agent_tools), its plan (agent_plan), and the AI chat's proposals
// and form fills (chat_actions, molt_ai_chat_form_fill). Each shows them the
// same way (as U+FFFD, one character for one), so what the user reads is
// what runs.
//
// Std-only; unit-tested in molt_ai_unittests (hidden_chars_unittest.cc).

#ifndef CHROME_BROWSER_MOLT_AI_AGENTS_HIDDEN_CHARS_H_
#define CHROME_BROWSER_MOLT_AI_AGENTS_HIDDEN_CHARS_H_

#include <cstddef>
#include <string>
#include <string_view>

namespace molt_ai {
namespace hidden_chars {

// The length in bytes of such a character at |s|[i] (UTF-8), or 0:
//   C1 controls            U+0080-U+009F
//   soft hyphen            U+00AD
//   Arabic letter mark     U+061C
//   Hangul fillers         U+115F, U+1160, U+3164, U+FFA0
//   Mongolian vowel sep.   U+180E
//   zero-width, LRM, RLM   U+200B-U+200F
//   line and paragraph     U+2028, U+2029
//   separators
//   bidi embeddings and    U+202A-U+202E
//   overrides
//   word joiner, invisible U+2060-U+2064
//   operators
//   bidi isolates          U+2066-U+2069
//   variation selectors    U+FE00-U+FE0F, U+E0100-U+E01EF
//   byte-order mark        U+FEFF
//   tag characters         U+E0000-U+E007F (they spell text nobody sees)
// A sequence cut short by the end of |s| is not one of them.
size_t HiddenCharAt(std::string_view s, size_t i);

// |s| with each of them replaced by U+FFFD; everything else unchanged.
std::string ShowHidden(std::string_view s);

}  // namespace hidden_chars
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AGENTS_HIDDEN_CHARS_H_
