// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The snippet shown under a memory search hit: the part of the matched
// chunk that best overlaps the query's words, or its start. Std-only,
// linear scanning (no std::regex on page text), UTF-8 safe.

#ifndef CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_SNIPPET_H_
#define CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_SNIPPET_H_

#include <cstddef>
#include <string>

namespace molt_ai {
namespace memory {

// At most |max_bytes| bytes of |chunk| (plus "…" where it was cut), never
// splitting a UTF-8 character: the window containing the most distinct
// query words (ASCII case-insensitive, words of 2+ bytes), starting a
// little before the first of them; the chunk's start when no query word
// occurs (a match by meaning often shares no word with the query).
std::string BestSnippet(const std::string& chunk,
                        const std::string& query,
                        size_t max_bytes);

}  // namespace memory
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_SNIPPET_H_
