// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// What voice mode says (docs/design/TOUCHLESS_BROWSING.md §2, §5.4, §5.6):
// a list read aloud ("20 results. 1: Bohemian Rhapsody, Queen Official, 6
// minutes. 2: ... Say a number or a name."), the close choices offered back
// after an unclear one, durations, counts and the browser's own sentences.
// English for now; another language adds its own sentences here as it
// adds its phrase tables (voice_command_phrases_<lang>.cc).
//
// Page text reaches these functions only from the page map, where it
// already passed SpeechSafeText(); it is cleaned and capped again here
// (SpeechSafeText() is idempotent), and is only ever read out: nothing a
// page shows is taken as a command, whatever it says.
//
// Std-only (no //base): unit-tested in molt_ai_unittests.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_SPEECH_TEXT_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_SPEECH_TEXT_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/voice/spoken_choice.h"
#include "chrome/browser/molt_ai/voice/voice_candidates.h"
#include "chrome/browser/molt_ai/voice/voice_command.h"

namespace molt_ai {
namespace voice {

// One sentence group the browser speaks at once, in code points. A long
// reading is split into several (spoken one after the other), so "stop"
// and a new command cut it short between them as well as inside them.
inline constexpr size_t kMaxSpeechChunkChars = 1200;
// One page text read in a sentence (a title, a label), in code points.
inline constexpr size_t kMaxSpokenPageTextChars = 160;
// Entries read at normal verbosity: results, and headings or links.
inline constexpr size_t kNormalResultsRead = 5;
inline constexpr size_t kNormalOutlineRead = 10;
// Entries read at full verbosity, at most.
inline constexpr size_t kFullListRead = 200;

// "45 seconds", "1 minute", "6 minutes", "1 hour 2 minutes": rounded to
// the minute from a minute on (the design's "6 minutes" for 6:07).
std::string SpokenDuration(int seconds);

// An exact length for a skip: "10 seconds", "1 minute 30 seconds".
std::string SpokenLength(int seconds);

// The same in |language| ("6 minutos", "1 minuto 30 segundos").
std::string SpokenDuration(int seconds, Language language);
std::string SpokenLength(int seconds, Language language);

// A count as the page shows it ("2.1B views") said in words ("2.1 billion
// views"); the text as it is when it isn't a count.
std::string SpokenCount(std::string_view text);

// "1 result" / "20 results".
std::string CountOf(size_t n, std::string_view one, std::string_view many);

// The number a candidate is read with: its on-screen number when it has
// one, else its position.
int SpokenNumber(const ChoiceCandidate& candidate);

// A candidate as read in a list, without its number: "Queen - Bohemian
// Rhapsody (Live Aid 1985), Queen Official, 3 minutes". Brief and normal:
// the title, the author or site, and the duration; full: also the date,
// counts and other details.
std::string SpokenCandidate(const ChoiceCandidate& candidate,
                            Verbosity verbosity);

// A list read aloud, in chunks of at most kMaxSpeechChunkChars: "20
// results." then, at normal verbosity, the first |normal_count| as "1:
// ..., 2: ..." and at full verbosity all of them (at most kFullListRead),
// then "Say a number or a name." |one| and |many| name the entries
// ("result", "results").
std::vector<std::string> ListSpeech(const CandidateSet& set,
                                    std::string_view one,
                                    std::string_view many,
                                    Verbosity verbosity,
                                    size_t normal_count);
// The same in |language| (its hint; |one| and |many| are the caller's).
std::vector<std::string> ListSpeech(const CandidateSet& set,
                                    std::string_view one,
                                    std::string_view many,
                                    Verbosity verbosity,
                                    size_t normal_count,
                                    Language language);

// The close choices after an unclear choice: "Did you mean 1: ..., 2:
// ..., or 3: ...? Say a number or a name."
std::vector<std::string> OfferSpeech(const CandidateSet& subset);
std::vector<std::string> OfferSpeech(const CandidateSet& subset,
                                     Language language);

// "Say a number or a name."
std::string_view ChooseHint();
std::string_view ChooseHint(Language language);

// |text| as page text inside a sentence: cleaned and capped at
// kMaxSpokenPageTextChars.
std::string PageText(std::string_view text);

// Chunks |sentences| (each one whole) into groups of at most
// kMaxSpeechChunkChars; a sentence longer than that is cut.
std::vector<std::string> ChunkSentences(
    const std::vector<std::string>& sentences);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_SPEECH_TEXT_H_
