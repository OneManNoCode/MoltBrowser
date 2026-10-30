// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// What voice mode learned that isn't a routine (docs/design/
// TOUCHLESS_BROWSING.md §6.8): the personal vocabulary (voice_vocabulary.h)
// and the site hints (voice_hints.h), as one small JSON file in the profile
// (voice_learning.h names it), never synced:
//
//   {"version": 1,
//    "words": [{"phrase": "Queen", "uses": 3, "last": 1800000000}],
//    "corrections": [{"heard": "green", "meant": "Queen", "uses": 1,
//                     "last": 1800000000}],
//    "hints": [{"host": "www.youtube.com", "search": "Search",
//               "list": "", "player": "Video player", "last": 1800000000}]}
//
// The file is read as untrusted (another program, or an old version, may
// have written it): over kMaxLearningFileBytes, not JSON, or another
// version is read as nothing; each entry goes through the same checks as
// what is learned (Restore()) and the caps; the rest is dropped. Then the
// whole is aged. Fuzzed by molt_voice_learning_fuzzer.
//
// base::Value only: unit-tested in molt_ai_unittests.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_LEARNING_STORE_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_LEARNING_STORE_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "chrome/browser/molt_ai/voice/voice_hints.h"
#include "chrome/browser/molt_ai/voice/voice_vocabulary.h"

namespace molt_ai {
namespace voice {

inline constexpr size_t kMaxLearningFileBytes = 256 * 1024;
inline constexpr int kLearningFormatVersion = 1;

struct LearnedData {
  LearnedData();
  ~LearnedData();
  LearnedData(const LearnedData&);
  LearnedData& operator=(const LearnedData&);
  LearnedData(LearnedData&&);
  LearnedData& operator=(LearnedData&&);

  PersonalVocabulary vocabulary;
  SiteHints hints;

  bool empty() const { return vocabulary.empty() && hints.empty(); }
};

// The file's contents for |data|.
std::string SerializeLearning(const LearnedData& data);

// |json| read as untrusted, then aged as of |now_unix|.
LearnedData ParseLearning(std::string_view json, int64_t now_unix);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_LEARNING_STORE_H_
