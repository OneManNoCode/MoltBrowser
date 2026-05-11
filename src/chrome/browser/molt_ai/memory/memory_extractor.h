// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MemoryExtractor — pure helpers that take a raw page text + URL and
// produce the cleaned-up text plus a chunk list. Both inputs are
// expected to already be plaintext (the WebContentsObserver calls
// document.body.innerText via JS, so HTML stripping isn't our problem
// here — just whitespace normalization + boundary-aware chunking).

#ifndef CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_EXTRACTOR_H_
#define CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_EXTRACTOR_H_

#include <string>
#include <vector>

namespace molt_ai {
namespace memory {

// Collapse whitespace runs, trim, drop pages that are too short to be
// worth indexing. Returns "" for skipped pages.
std::string NormalizePageText(const std::string& raw);

// Break |text| into overlapping chunks of about kChunkTargetChars
// characters. Tries to break at sentence boundaries (period, newline,
// '?', '!') within ~80 chars of the target so chunks read naturally.
std::vector<std::string> ChunkText(const std::string& text);

// Word count over an already-normalized text. Used for the "skip
// too-thin pages" heuristic and for the UI's stats panel.
int CountWords(const std::string& text);

}  // namespace memory
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_EXTRACTOR_H_
