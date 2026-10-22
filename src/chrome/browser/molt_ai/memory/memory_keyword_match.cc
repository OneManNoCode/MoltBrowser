// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/memory/memory_keyword_match.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "chrome/browser/molt_ai/memory/memory_embedder.h"

namespace molt_ai {
namespace memory {

namespace {

bool AnyMatches(const std::string& q, const std::vector<std::string>& words) {
  return std::any_of(words.begin(), words.end(), [&](const std::string& w) {
    return KeywordWordMatches(q, w);
  });
}

// |url| without "<scheme>://": "https" or "www" alone shouldn't match every
// page.
std::string WithoutScheme(const std::string& url) {
  const size_t sep = url.find("://");
  if (sep != std::string::npos && sep <= 16) {
    return url.substr(sep + 3);
  }
  return url;
}

}  // namespace

KeywordQuery ParseKeywordQuery(const std::string& query) {
  KeywordQuery q;
  for (std::string& w : FoldedWords(query, kMaxKeywordFieldWords)) {
    if (q.words.size() >= kMaxKeywordQueryWords) {
      break;
    }
    if (std::find(q.words.begin(), q.words.end(), w) == q.words.end()) {
      q.words.push_back(std::move(w));
    }
  }
  return q;
}

bool KeywordWordMatches(const std::string& q, const std::string& w) {
  if (q.empty()) {
    return false;
  }
  if (w.size() >= q.size() && w.compare(0, q.size(), q) == 0) {
    return true;
  }
  const bool non_ascii = std::any_of(q.begin(), q.end(), [](char c) {
    return static_cast<unsigned char>(c) >= 0x80;
  });
  return non_ascii && w.find(q) != std::string::npos;
}

float KeywordScore(const KeywordQuery& query,
                   const std::string& title,
                   const std::string& url,
                   const std::string& text) {
  if (query.words.empty()) {
    return 0.0f;
  }
  const std::vector<std::string> title_words =
      FoldedWords(title, kMaxKeywordFieldWords);
  const std::vector<std::string> url_words =
      FoldedWords(WithoutScheme(url), kMaxKeywordFieldWords);
  std::vector<std::string> text_words;
  bool text_read = false;
  float total = 0.0f;
  for (const std::string& q : query.words) {
    float best = 0.0f;
    if (AnyMatches(q, title_words)) {
      best = kTitleMatchWeight;
    } else if (AnyMatches(q, url_words)) {
      best = kUrlMatchWeight;
    } else {
      if (!text_read) {
        text_words = FoldedWords(text, kMaxKeywordFieldWords);
        text_read = true;
      }
      if (AnyMatches(q, text_words)) {
        best = kTextMatchWeight;
      }
    }
    if (best == 0.0f) {
      return 0.0f;  // every word must be there
    }
    total += best;
  }
  return total / static_cast<float>(query.words.size());
}

std::vector<KeywordCandidate> RankKeywordCandidates(
    std::vector<KeywordCandidate> candidates,
    size_t top_k) {
  for (KeywordCandidate& c : candidates) {
    if (!std::isfinite(c.score)) {
      c.score = 0.0f;
    }
  }
  // One entry per page: its best score.
  std::sort(candidates.begin(), candidates.end(),
            [](const KeywordCandidate& a, const KeywordCandidate& b) {
              if (a.doc_id != b.doc_id) {
                return a.doc_id < b.doc_id;
              }
              return a.score > b.score;
            });
  candidates.erase(
      std::unique(candidates.begin(), candidates.end(),
                  [](const KeywordCandidate& a, const KeywordCandidate& b) {
                    return a.doc_id == b.doc_id;
                  }),
      candidates.end());
  std::sort(candidates.begin(), candidates.end(),
            [](const KeywordCandidate& a, const KeywordCandidate& b) {
              if (a.score != b.score) {
                return a.score > b.score;
              }
              if (a.visited_at_unix != b.visited_at_unix) {
                return a.visited_at_unix > b.visited_at_unix;
              }
              return a.doc_id > b.doc_id;
            });
  if (candidates.size() > top_k) {
    candidates.resize(top_k);
  }
  return candidates;
}

}  // namespace memory
}  // namespace molt_ai
