// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_repair.h"

#include <algorithm>
#include <set>
#include <utility>
#include <vector>

#include "chrome/browser/molt_ai/voice/phonetic_key.h"
#include "chrome/browser/molt_ai/voice/spoken_phrases.h"
#include "chrome/browser/molt_ai/voice/spoken_text.h"
#include "chrome/browser/molt_ai/voice/voice_command_phrases.h"

namespace molt_ai {
namespace voice {

namespace {

using K = VoiceCommandKind;

// How far, in the phonetic key's half-edits, a word may be from the
// grammar's word it is taken for: the same sounds, or one dropped or
// close sound ("groll"/"scroll", "pores"/"pause", "head"/"ad").
constexpr int kWordMargin = 2;
// How far a scrap may be from the wake phrase's name, or from the whole
// phrase ("halt", "him all", "Timolt" for "Hey Molt").
constexpr int kWakeScrapMargin = 3;
// A scrap of the wake phrase is at most this many words.
constexpr size_t kMaxWakeScrapWords = 2;

struct VocabularyWord {
  std::string word;
  std::string key;
};

bool IsLowerAsciiLetter(char c) {
  return c >= 'a' && c <= 'z';
}

// The words of English's patterns that have a phonetic key: the literal
// words, not the slots.
std::vector<VocabularyWord> BuildVocabulary() {
  std::set<std::string, std::less<>> seen;
  std::vector<VocabularyWord> words;
  for (const CommandPattern& pattern : CommandPatterns(Language::kEnglish)) {
    const std::string_view s = pattern.pattern;
    size_t i = 0;
    while (i < s.size()) {
      if (s[i] == '{') {
        const size_t close = s.find('}', i);
        if (close == std::string_view::npos) {
          break;
        }
        i = close + 1;
        continue;
      }
      if (!IsLowerAsciiLetter(s[i])) {
        ++i;
        continue;
      }
      size_t j = i;
      while (j < s.size() && IsLowerAsciiLetter(s[j])) {
        ++j;
      }
      const std::string word(s.substr(i, j - i));
      i = j;
      if (word.size() < 2 || !seen.insert(word).second) {
        continue;
      }
      std::string key = PhoneticKey(word);
      if (key.size() < 2) {
        continue;
      }
      words.push_back({word, std::move(key)});
    }
  }
  return words;
}

const std::vector<VocabularyWord>& Vocabulary() {
  static const std::vector<VocabularyWord>* const words =
      new std::vector<VocabularyWord>(BuildVocabulary());
  return *words;
}

// Words a recognizer gets right, that aren't swapped on their own (a
// command around them is what was misheard).
bool IsFunctionWord(std::string_view word) {
  static constexpr std::string_view kWords[] = {
      "a",    "an",    "and",    "at",    "by",   "for",  "from",
      "in",   "is",    "it",     "me",    "my",   "of",   "on",
      "the",  "this",  "that",   "to",    "with", "your", "please",
      // Answers: never swapped into, or out of, anything else.
      "yes",  "yeah",  "yep",    "no",    "nope", "nah",  "okay",
      "ok",   "sure",  "stop",   "cancel", "never", "mind", "thanks",
      "thank"};
  for (std::string_view w : kWords) {
    if (w == word) {
      return true;
    }
  }
  return false;
}

std::string Join(std::string_view a, std::string_view b) {
  if (a.empty()) {
    return std::string(b);
  }
  if (b.empty()) {
    return std::string(a);
  }
  return JoinPhoneticKeys(a, b);
}

// The keys of the wake phrase's name ("molt") and of the whole phrase
// ("hey molt").
std::pair<std::string, std::string> WakeKeys(const ParseContext& context,
                                             const LanguagePhrases& p) {
  const NormalizedText wake = NormalizeSpokenText(
      context.wake_phrase.empty() ? p.default_wake_phrase
                                  : context.wake_phrase,
      {});
  std::string name;
  std::string whole;
  for (size_t i = 0; i < wake.words.size(); ++i) {
    const std::string_view w = wake.Word(i);
    const std::string key = PhoneticKey(w);
    whole = Join(whole, key);
    const bool greeting =
        name.empty() && std::find(p.wake_greetings.begin(),
                                  p.wake_greetings.end(),
                                  w) != p.wake_greetings.end();
    if (!greeting) {
      name = Join(name, key);
    }
  }
  return {name, whole};
}

int Distance(std::string_view a, std::string_view b, int bound) {
  if (a.empty() || b.empty()) {
    return bound + 1;
  }
  return PhoneticDistance(a, b, bound);
}

bool IsSentenceBreak(char c) {
  return c == '.' || c == ',' || c == '!' || c == '?';
}

// Whether |command| carries words as heard: free text, a site, or a
// choice's description. A command with a swapped word must carry none, so
// that the swap made a command phrase ("close this tab"), never a verb in
// front of whatever followed ("the cat..." as "dictate ...").
bool HasFreeWords(const VoiceCommand& command) {
  return !command.text.empty() || !command.site_words.empty() ||
         !command.whole_query.empty() || !command.site.empty() ||
         !command.choice.words.empty() || !command.choice.by_words.empty() ||
         !command.choice.from_words.empty();
}

struct Candidate {
  std::string text;
  VoiceCommand command;
  int cost = 0;
};

class Repairer {
 public:
  explicit Repairer(const ParseContext& context) : context_(context) {}

  // |text| as the grammar understands it, when that is a command a repair
  // may run; after a swap, only a command that carries no words as heard.
  void Try(std::string text, int cost, bool swapped) {
    VoiceCommand command = ParseVoiceCommand(text, context_);
    if (!RepairableKind(command.kind) ||
        (swapped && HasFreeWords(command))) {
      return;
    }
    candidates_.push_back({std::move(text), std::move(command), cost});
  }

  // One word of |text|, or two run together, swapped for a grammar word
  // that sounds like it.
  void TrySwaps(std::string_view text, int base_cost) {
    const NormalizedText n = NormalizeSpokenText(text, {});
    if (n.words.empty() || n.words.size() > kMaxRepairWords) {
      return;
    }
    for (size_t i = 0; i < n.words.size(); ++i) {
      for (size_t span = 1; span <= 2 && i + span <= n.words.size(); ++span) {
        std::string key;
        bool usable = true;
        for (size_t k = i; k < i + span; ++k) {
          const SpokenWord& w = n.words[k];
          // Digits and other scripts match only as written.
          if (!w.ascii || w.numeric ||
              (span == 1 && IsFunctionWord(n.Word(k)))) {
            usable = false;
            break;
          }
          key = Join(key, PhoneticKey(n.Word(k)));
        }
        if (!usable || key.size() < 2) {
          continue;
        }
        const size_t begin = n.words[i].source_begin;
        const size_t end = n.words[i + span - 1].source_end;
        for (const VocabularyWord& known : Vocabulary()) {
          const int d = Distance(key, known.key, kWordMargin);
          // Two words run together only when they sound exactly like one
          // ("on mute" for "unmute").
          if (d > (span == 2 ? 0 : kWordMargin) ||
              (span == 1 && known.word == n.Word(i))) {
            continue;
          }
          std::string repaired(text.substr(0, begin));
          repaired += known.word;
          repaired += text.substr(end);
          Try(std::move(repaired), base_cost + d + (span == 2 ? 1 : 0),
              /*swapped=*/true);
        }
      }
    }
  }

  // The best candidate, when they all mean the same kind of command and
  // one repair costs least.
  std::optional<Candidate> Best() const {
    if (candidates_.empty()) {
      return std::nullopt;
    }
    const K kind = candidates_.front().command.kind;
    const Candidate* best = nullptr;
    bool tie = false;
    for (const Candidate& c : candidates_) {
      if (c.command.kind != kind) {
        return std::nullopt;  // two different commands: not a guess to act on
      }
      if (!best || c.cost < best->cost) {
        best = &c;
        tie = false;
      } else if (c.cost == best->cost && c.text != best->text) {
        tie = true;
      }
    }
    if (tie) {
      return std::nullopt;
    }
    return *best;
  }

 private:
  const ParseContext& context_;
  std::vector<Candidate> candidates_;
};

}  // namespace

RepairedCommand::RepairedCommand() = default;
RepairedCommand::~RepairedCommand() = default;
RepairedCommand::RepairedCommand(const RepairedCommand&) = default;
RepairedCommand& RepairedCommand::operator=(const RepairedCommand&) = default;
RepairedCommand::RepairedCommand(RepairedCommand&&) = default;
RepairedCommand& RepairedCommand::operator=(RepairedCommand&&) = default;

bool RepairableKind(VoiceCommandKind kind) {
  switch (kind) {
    case K::kOpenSite:
    case K::kSearch:
    case K::kFindInPage:
    case K::kBack:
    case K::kForward:
    case K::kReload:
    case K::kStopLoading:
    case K::kHome:
    case K::kNextPage:
    case K::kNewTab:
    case K::kCloseTab:  // "reopen the closed tab" and "undo" bring it back
    case K::kSwitchTab:
    case K::kScrollDown:
    case K::kScrollUp:
    case K::kScrollLeft:
    case K::kScrollRight:
    case K::kScrollToTop:
    case K::kScrollToBottom:
    case K::kZoomIn:
    case K::kZoomOut:
    case K::kZoomReset:
    case K::kPlay:
    case K::kPause:
    case K::kResume:
    case K::kVolumeUp:
    case K::kVolumeDown:
    case K::kSetVolume:
    case K::kMute:
    case K::kUnmute:
    case K::kSkipForward:
    case K::kSkipBack:
    case K::kRestart:
    case K::kNextTrack:
    case K::kPreviousTrack:
    case K::kFullScreen:
    case K::kExitFullScreen:
    case K::kCaptionsOn:
    case K::kCaptionsOff:
    case K::kSpeedUp:
    case K::kSlowDown:
    case K::kNormalSpeed:
    case K::kSkipAd:
    case K::kReadPage:
    case K::kReadFromHere:
    case K::kReadHeadings:
    case K::kReadLinks:
    case K::kReadResults:
    case K::kReadTitle:
    case K::kReadSelection:
    case K::kDescribeScreen:
    case K::kNextHeading:
    case K::kPreviousHeading:
    case K::kNextLink:
    case K::kPreviousLink:
    case K::kNextItem:
    case K::kPreviousItem:
    case K::kNextParagraph:
    case K::kPreviousParagraph:
    case K::kReadSection:
    case K::kAskPage:
    case K::kShowNumbers:
    case K::kHideNumbers:
    case K::kType:  // "undo" takes it back
    case K::kNewLine:
    case K::kReadBack:
    case K::kStop:
    case K::kStopSpeaking:
    case K::kUndo:
    case K::kRepeat:
    case K::kHelp:
    case K::kWhereAmI:
    case K::kSpeakFaster:
    case K::kSpeakSlower:
    case K::kVerbosity:
    case K::kSetSpeed:
    case K::kNextMatch:
    case K::kPreviousMatch:
    case K::kListBookmarks:
    case K::kListHistory:
    case K::kReopenTab:
    case K::kListDownloads:
    case K::kShowBrowserButtons:
    case K::kOpenMenu:
    case K::kCloseMenu:
      return true;
    default:
      // An answer, a choice, a form's submission, a correction, a routine,
      // what voice mode learned, voice mode itself, a setting, a download,
      // a bookmark's removal, a window, a field cleared, the AI: never on
      // a guess.
      return false;
  }
}

std::optional<RepairedCommand> RepairMisheardCommand(
    std::string_view utterance,
    const ParseContext& context) {
  if (context.expecting_yes_no || context.language != Language::kEnglish) {
    return std::nullopt;
  }
  const VoiceCommand original = ParseVoiceCommand(utterance, context);
  if (original.kind != K::kUnknown) {
    return std::nullopt;
  }
  // What was said after the wake phrase (and fillers), as written: the wake
  // phrase itself is never swapped for a command.
  const std::string& heard = original.heard;
  const NormalizedText n = NormalizeSpokenText(heard, {});
  if (n.words.empty() || n.words.size() > kMaxRepairWords) {
    return std::nullopt;
  }
  Repairer repairer(context);

  // 1. Without a scrap of the wake phrase.
  std::string rest;
  const auto [name_key, whole_key] =
      WakeKeys(context, PhrasesFor(context.language));
  std::string head_key;
  for (size_t k = 0; k < kMaxWakeScrapWords && k + 1 < n.words.size(); ++k) {
    const SpokenWord& w = n.words[k];
    if (!w.ascii || w.numeric) {
      break;
    }
    head_key = Join(head_key, PhoneticKey(n.Word(k)));
    if (!IsSentenceBreak(w.trailing)) {
      continue;
    }
    const bool scrap =
        Distance(head_key, name_key, kWakeScrapMargin) <= kWakeScrapMargin ||
        Distance(head_key, whole_key, kWakeScrapMargin) <= kWakeScrapMargin;
    if (scrap && w.source_end + 1 <= heard.size()) {
      rest = heard.substr(w.source_end + 1);
      repairer.Try(rest, 1, /*swapped=*/false);
    }
    break;
  }

  // 2. One word swapped for the grammar's: in what follows the scrap, or,
  // without one, in all of it. A command after the scrap as it was heard
  // is taken as it is.
  std::optional<Candidate> best = repairer.Best();
  if (!best) {
    if (!rest.empty()) {
      repairer.TrySwaps(rest, 1);
    } else {
      repairer.TrySwaps(heard, 0);
    }
    best = repairer.Best();
  }
  if (!best) {
    return std::nullopt;
  }
  RepairedCommand repaired;
  repaired.command = std::move(best->command);
  repaired.command.heard = original.heard;
  repaired.text = CleanSpokenText(best->text, kMaxUtteranceBytes);
  return repaired;
}

}  // namespace voice
}  // namespace molt_ai
