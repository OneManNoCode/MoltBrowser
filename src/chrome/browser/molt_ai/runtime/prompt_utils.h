// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Std-only prompt helpers for BrowserAIRuntime (unit-tested standalone in
// prompt_utils_unittest.cc).

#ifndef CHROME_BROWSER_MOLT_AI_RUNTIME_PROMPT_UTILS_H_
#define CHROME_BROWSER_MOLT_AI_RUNTIME_PROMPT_UTILS_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace molt_ai {

// Number of leading tokens |a| and |b| share. KV-cache entries for that
// prefix can be reused verbatim across consecutive turns.
size_t CommonPrefixLength(const std::vector<int32_t>& a,
                          const std::vector<int32_t>& b);

// Keeps the first and last parts of |tokens| (1/3 head, 2/3 tail) with
// |marker| in between so the result is exactly |limit| tokens long (or
// |tokens| unchanged if it already fits). Used for raw agent prompts, whose
// task statement is at the start and output instructions at the end.
std::vector<int32_t> TruncateTokensMiddle(const std::vector<int32_t>& tokens,
                                          size_t limit,
                                          const std::vector<int32_t>& marker);

// The chat prompt is tokenized with parse_special=true so the template's
// own markers become control tokens. Untrusted text (page excerpts, PDF /
// attachment text, memory hits, earlier turns) must not be able to do the
// same: a page containing "<|im_end|><|im_start|>system" would otherwise
// inject a real system turn. Inserts a zero-width space after the first
// byte of every occurrence of a |special_texts| entry, which keeps the text
// readable but stops it from tokenizing to the control token.
std::string NeutralizeSpecialTokens(
    const std::string& text,
    const std::vector<std::string>& special_texts);

// True for vocab texts worth neutralizing: control-looking strings such as
// "<|im_start|>", "<start_of_turn>", "</s>", "[INST]", "<think>".
bool LooksLikeSpecialTokenText(const std::string& text);

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_RUNTIME_PROMPT_UTILS_H_
