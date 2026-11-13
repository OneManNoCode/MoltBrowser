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
// close sound ("groll"/"scroll", "flay"/"play", "head"/"ad").
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

// Whether |a| and |b| are the same command: the same kind and the same
// values ("girl to the bottom" and "scroll to the bottom" are).
bool SameCommand(const VoiceCommand& a, const VoiceCommand& b) {
  return a.kind == b.kind && a.text == b.text && a.site == b.site &&
         a.new_tab == b.new_tab && a.scope == b.scope && a.verb == b.verb &&
         a.seconds == b.seconds && a.level == b.level && a.rate == b.rate &&
         a.settings_page == b.settings_page && a.scroll == b.scroll &&
         a.count == b.count && a.verbosity == b.verbosity &&
         a.page_question == b.page_question &&
         a.choice.kind == b.choice.kind &&
         a.choice.ordinal == b.choice.ordinal &&
         a.choice.from_end == b.choice.from_end &&
         a.choice.number == b.choice.number &&
         a.choice.relative == b.choice.relative &&
         a.choice.words == b.choice.words;
}

struct Candidate {
  std::string text;
  VoiceCommand command;
  int cost = 0;
  // Only a scrap of the wake phrase dropped: the command's words as heard.
  bool as_said = false;
};

class Repairer {
 public:
  explicit Repairer(const ParseContext& context) : context_(context) {}

  // |text| as the grammar understands it, kept when that is a command a
  // repair may run: after a swap, one RepairableKind() allows that carries
  // no words as heard; as said (a scrap of the wake phrase dropped), one
  // RepairableAsSaidKind() allows. Returns the kind understood.
  VoiceCommandKind Try(std::string text, int cost, bool swapped) {
    VoiceCommand command = ParseVoiceCommand(text, context_);
    const VoiceCommandKind kind = command.kind;
    const bool allowed = swapped
                             ? RepairableKind(kind) && !HasFreeWords(command)
                             : RepairableAsSaidKind(kind);
    if (allowed) {
      candidates_.push_back(
          {std::move(text), std::move(command), cost, /*as_said=*/!swapped});
    }
    return kind;
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

  // The cheapest candidate, when every candidate is the same command: two
  // different commands, and none is a guess to act on.
  std::optional<Candidate> Best() const {
    if (candidates_.empty()) {
      return std::nullopt;
    }
    const Candidate* best = &candidates_.front();
    for (const Candidate& c : candidates_) {
      if (!SameCommand(c.command, best->command)) {
        return std::nullopt;
      }
      if (c.cost < best->cost) {
        best = &c;
      }
    }
    return *best;
  }

 private:
  // A copy: a few flags and a view of the wake phrase, which outlives
  // the Repairer.
  const ParseContext context_;
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

bool RepairableAsSaidKind(VoiceCommandKind kind) {
  if (RepairableKind(kind)) {
    return true;
  }
  switch (kind) {
    // What the user said, once "Hey Molt" heard as "Kalt," is dropped: a
    // press ("click empty the cart", which asks first when it buys, deletes
    // or sends, as said in full), a form's submission (likewise), editing,
    // a correction, a routine run or listed, a bookmark made or opened,
    // history, settings' page, windows opened or switched, the AI.
    case K::kChoose:
    case K::kSubmit:
    case K::kCancel:
    case K::kDeleteLastWord:
    case K::kClearField:
    case K::kCorrection:
    case K::kStartOver:
    case K::kRunRoutine:
    case K::kListRoutines:
    case K::kBookmarkPage:
    case K::kOpenBookmark:
    case K::kOpenFromHistory:
    case K::kOpenSettings:
    case K::kNewWindow:
    case K::kSwitchWindow:
    case K::kAskAI:
    case K::kOpenAIChat:
      return true;
    default:
      // An answer to a question nobody asked, what voice mode learned
      // (remembered or forgotten), voice mode itself and always listening,
      // a protected setting, a downloaded file opened, a window closed, a
      // bookmark removed: the scrap is a guess too, and these wait for the
      // wake phrase heard as it is.
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
  // What follows the scrap is a command as it was said, one this doesn't
  // run: no swap makes it another.
  bool understood_as_said = false;
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
    // "Hey, more, ...": a greeting may come first, with its own comma.
    // When the wake detector heard the wake phrase begin the utterance,
    // the words before the break are it, however the recognizer wrote them
    // ("cabled, set the volume to half").
    const bool scrap =
        context.wake_heard ||
        Distance(head_key, name_key, kWakeScrapMargin) <= kWakeScrapMargin ||
        Distance(head_key, whole_key, kWakeScrapMargin) <= kWakeScrapMargin;
    if (scrap && w.source_end + 1 <= heard.size()) {
      rest = heard.substr(w.source_end + 1);
      understood_as_said =
          repairer.Try(rest, 1, /*swapped=*/false) != K::kUnknown;
      // Heard by the detector, a greeting with its own comma may come
      // first ("Hey, Lord, skip the head"): the second word is tried too.
      if (understood_as_said || !context.wake_heard) {
        break;
      }
    }
  }
  // Or without the name's last sound heard as a word of its own, after a
  // wake phrase the grammar took ("him all to scroll to the top" for "Hey
  // Molt, scroll to the top": the "t" of "Molt", clipped, as "to"), in
  // |utterance| or before it (ParseContext::wake_said: the controller
  // repairs a command's |heard|): a first word of one consonant, the stop
  // the name ends on. Or a breath, or the "uh" before a command, written as
  // "a" ("Hey molt, a type Grace Hopper", the spoken suite on build 50).
  // Only when what follows is a command as said.
  if (rest.empty() && !understood_as_said && n.words.size() >= 2 &&
      n.words[0].ascii && !n.words[0].numeric && !name_key.empty() &&
      (context.wake_said || BeginsWithWakePhrase(utterance, context))) {
    const char last = name_key.back();
    std::string first;
    for (char c : PhoneticKey(n.Word(0))) {
      if (c != 'a') {
        first.push_back(c);
      }
    }
    const bool stop = (last == 'P' || last == 'T' || last == 'K') &&
                      first.size() == 1 && first[0] == last;
    if (stop || n.Word(0) == "a") {
      understood_as_said =
          repairer.Try(heard.substr(n.words[1].source_begin), 1,
                       /*swapped=*/false) != K::kUnknown;
    }
  }

  // 2. One word swapped for the grammar's: in what follows the scrap, or,
  // without one, in all of it. A command after the scrap as it was heard
  // is taken as it is.
  std::optional<Candidate> best = repairer.Best();
  if (!best && !understood_as_said) {
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
  repaired.as_said = best->as_said;
  return repaired;
}

}  // namespace voice
}  // namespace molt_ai
