// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// What voice mode learned that isn't a routine (docs/design/
// TOUCHLESS_BROWSING.md §6.8): the personal vocabulary (voice_vocabulary.h)
// and the site hints (voice_hints.h), as one small JSON file in the profile
// (voice_learning.h names it), never synced:
//
//   {"version": 1, "learn": true,
//    "words": [{"phrase": "Queen", "uses": 3, "last": 1800000000}],
//    "corrections": [{"heard": "green", "meant": "Queen", "uses": 1,
//                     "last": 1800000000}],
//    "hints": [{"host": "www.youtube.com", "search": "Search",
//               "list": "", "player": "Video player", "last": 1800000000}],
//    "declined_routines": ["voice_1790000000_1"]}
//
// "declined_routines" (round 43, profile-routines): the ids of routines
// from before routines belonged to a profile that the user said in this
// profile aren't theirs (voice_routine_store.h), so they aren't offered
// here again. An older browser ignores the key.
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
#include <vector>

#include "chrome/browser/molt_ai/voice/voice_hints.h"
#include "chrome/browser/molt_ai/voice/voice_vocabulary.h"

namespace molt_ai {
namespace voice {

inline constexpr size_t kMaxLearningFileBytes = 256 * 1024;
inline constexpr int kLearningFormatVersion = 1;
// Routines from before profiles declined, at most (the oldest go first).
inline constexpr size_t kMaxDeclinedRoutines = 200;

struct LearnedData {
  LearnedData();
  ~LearnedData();
  LearnedData(const LearnedData&);
  LearnedData& operator=(const LearnedData&);
  LearnedData(LearnedData&&);
  LearnedData& operator=(LearnedData&&);

  PersonalVocabulary vocabulary;
  SiteHints hints;
  // "Learn from what I say" (voice_learning.h); on unless turned off.
  bool learn = true;
  // Routines from before profiles that aren't this profile's (their ids:
  // a script id, as AutomationStorage takes it), oldest first, without
  // repeats, at most kMaxDeclinedRoutines.
  std::vector<std::string> declined_routines;

  bool empty() const { return vocabulary.empty() && hints.empty(); }
};

// The file's contents for |data|.
std::string SerializeLearning(const LearnedData& data);

// |ids| added to |declined| (LearnedData::declined_routines): each valid
// id not there yet, at the end, the oldest dropped past the cap. Returns
// whether it changed.
bool AddDeclinedRoutines(std::vector<std::string>& declined,
                         const std::vector<std::string>& ids);

// |json| read as untrusted, then aged as of |now_unix|.
LearnedData ParseLearning(std::string_view json, int64_t now_unix);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_LEARNING_STORE_H_
