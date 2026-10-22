// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/memory/memory_snippet.h"

#include <algorithm>
#include <vector>

namespace molt_ai {
namespace memory {

namespace {

constexpr size_t kMaxQueryWords = 16;
constexpr size_t kMaxWordBytes = 64;
constexpr size_t kMaxHits = 2048;
// Context kept before the first matching word.
constexpr size_t kMaxLeadBytes = 40;
// A cut at a space this close to the limit reads better than mid-word.
constexpr size_t kWordCutSlack = 24;

constexpr char kEllipsis[] = "\xE2\x80\xA6";  // …

bool IsWordByte(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c >= 0x80;
}

bool IsContinuationByte(unsigned char c) {
  return (c & 0xC0) == 0x80;
}

std::string LowerAscii(const std::string& s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

std::vector<std::string> QueryWords(const std::string& query) {
  std::vector<std::string> words;
  const std::string q = LowerAscii(query);
  size_t i = 0;
  while (i < q.size() && words.size() < kMaxQueryWords) {
    while (i < q.size() && !IsWordByte(static_cast<unsigned char>(q[i]))) ++i;
    const size_t begin = i;
    while (i < q.size() && IsWordByte(static_cast<unsigned char>(q[i]))) ++i;
    const size_t len = i - begin;
    if (len < 2 || len > kMaxWordBytes) continue;
    std::string w = q.substr(begin, len);
    if (std::find(words.begin(), words.end(), w) == words.end()) {
      words.push_back(std::move(w));
    }
  }
  return words;
}

struct Hit {
  size_t pos;
  size_t word;
};

}  // namespace

std::string BestSnippet(const std::string& chunk,
                        const std::string& query,
                        size_t max_bytes) {
  if (max_bytes == 0) return std::string();
  if (chunk.size() <= max_bytes) return chunk;

  // Where the query's words start a word of the chunk (a prefix match, so
  // "recipe" finds "recipes").
  const std::vector<std::string> words = QueryWords(query);
  const std::string text = LowerAscii(chunk);
  std::vector<Hit> hits;
  for (size_t w = 0; w < words.size() && hits.size() < kMaxHits; ++w) {
    size_t from = 0;
    while (hits.size() < kMaxHits) {
      const size_t pos = text.find(words[w], from);
      if (pos == std::string::npos) break;
      if (pos == 0 || !IsWordByte(static_cast<unsigned char>(text[pos - 1]))) {
        hits.push_back({pos, w});
      }
      from = pos + 1;
    }
  }

  const size_t lead = std::min(kMaxLeadBytes, max_bytes / 4);
  size_t start = 0;
  if (!hits.empty()) {
    std::sort(hits.begin(), hits.end(),
              [](const Hit& a, const Hit& b) { return a.pos < b.pos; });
    // The window (after the lead) holding the most distinct words; the
    // earliest on a tie. Two pointers over the sorted hits.
    const size_t span = max_bytes - lead;
    std::vector<size_t> counts(words.size(), 0);
    size_t distinct = 0;
    size_t best = 0;
    size_t best_distinct = 0;
    size_t j = 0;
    for (size_t i = 0; i < hits.size(); ++i) {
      // |counts| holds the hits in [i, j); none when hit i didn't fit.
      if (j < i) j = i;
      const size_t limit = hits[i].pos + span;
      while (j < hits.size() &&
             hits[j].pos + words[hits[j].word].size() <= limit) {
        if (counts[hits[j].word]++ == 0) ++distinct;
        ++j;
      }
      if (distinct > best_distinct) {
        best_distinct = distinct;
        best = hits[i].pos;
      }
      if (j > i && --counts[hits[i].word] == 0) --distinct;
    }
    start = best > lead ? best - lead : 0;
    // Begin at a word if one starts within the lead.
    if (start > 0) {
      for (size_t k = start; k < best; ++k) {
        if (chunk[k] == ' ') {
          start = k + 1;
          break;
        }
      }
    }
  }
  while (start < chunk.size() &&
         IsContinuationByte(static_cast<unsigned char>(chunk[start]))) {
    ++start;
  }
  // Keep the cut window within the chunk.
  if (chunk.size() - start < max_bytes) {
    start = chunk.size() - max_bytes;
    while (start < chunk.size() &&
           IsContinuationByte(static_cast<unsigned char>(chunk[start]))) {
      ++start;
    }
  }

  size_t end = std::min(chunk.size(), start + max_bytes);
  if (end < chunk.size()) {
    while (end > start &&
           IsContinuationByte(static_cast<unsigned char>(chunk[end]))) {
      --end;
    }
    for (size_t k = end; k > start && end - k < kWordCutSlack; --k) {
      if (chunk[k - 1] == ' ') {
        end = k - 1;
        break;
      }
    }
  }

  std::string out;
  if (start > 0) out += kEllipsis;
  out.append(chunk, start, end - start);
  if (end < chunk.size()) out += kEllipsis;
  return out;
}

}  // namespace memory
}  // namespace molt_ai
