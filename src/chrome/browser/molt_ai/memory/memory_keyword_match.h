// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Keyword ranking for the @memory omnibox shortcut's first, fast answer
// (docs/design/SEMANTIC_SEARCH.md, "The @memory omnibox shortcut"): which
// pages contain every word the user typed, in their title, URL or text, and
// how well. No model; words as HashingEmbedder reads them (FoldedWords()),
// so case and scripts are handled the same way as in the keyword index.
// Standard library only (memory_core).

#ifndef CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_KEYWORD_MATCH_H_
#define CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_KEYWORD_MATCH_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace molt_ai {
namespace memory {

// Most distinct words of a query that count.
inline constexpr size_t kMaxKeywordQueryWords = 8;
// Most words read from a title, URL or passage.
inline constexpr size_t kMaxKeywordFieldWords = 512;

// What a field match is worth, per query word.
inline constexpr float kTitleMatchWeight = 1.0f;
inline constexpr float kUrlMatchWeight = 0.6f;
inline constexpr float kTextMatchWeight = 0.3f;

// The distinct folded words of a query, in order, at most
// kMaxKeywordQueryWords.
struct KeywordQuery {
  std::vector<std::string> words;
};
KeywordQuery ParseKeywordQuery(const std::string& query);

// True iff query word |q| matches field word |w|: |w| starts with |q|, or,
// for a |q| with non-ASCII characters (scripts written without spaces),
// contains it.
bool KeywordWordMatches(const std::string& q, const std::string& w);

// 0 when some query word is in none of |title|, |url| and |text|; else the
// mean, over the query words, of the best field each matches in
// (kTitleMatchWeight, kUrlMatchWeight, kTextMatchWeight): in (0, 1]. The URL's
// scheme is not read. Also 0 for a query without words.
float KeywordScore(const KeywordQuery& query,
                   const std::string& title,
                   const std::string& url,
                   const std::string& text);

// A page that matched.
struct KeywordCandidate {
  int64_t doc_id = 0;
  float score = 0.0f;
  int64_t visited_at_unix = 0;
};

// Best first: higher score, then more recently visited, then newer doc id.
// One entry per doc id (its best score) and at most |top_k| entries.
std::vector<KeywordCandidate> RankKeywordCandidates(
    std::vector<KeywordCandidate> candidates,
    size_t top_k);

}  // namespace memory
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_KEYWORD_MATCH_H_
