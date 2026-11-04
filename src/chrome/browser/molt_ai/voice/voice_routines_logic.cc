// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_routines_logic.h"

#include <algorithm>
#include <array>
#include <span>
#include <utility>

#include "chrome/browser/molt_ai/voice/page_map_text.h"
#include "chrome/browser/molt_ai/voice/spoken_answers.h"
#include "chrome/browser/molt_ai/voice/spoken_numbers.h"
#include "chrome/browser/molt_ai/voice/spoken_phrases.h"
#include "chrome/browser/molt_ai/voice/spoken_text.h"
#include "chrome/browser/molt_ai/voice/voice_speech_text.h"

namespace molt_ai {
namespace voice {

namespace {

using K = VoiceCommandKind;

constexpr std::string_view kOrdinals[] = {
    "first", "second", "third",  "fourth", "fifth",
    "sixth", "seventh", "eighth", "ninth",  "tenth",
};
// Spanish's, masculine (what is chosen is "el primero", whatever it is).
constexpr std::string_view kOrdinalsEs[] = {
    "primero", "segundo", "tercero", "cuarto", "quinto",
    "sexto",   "séptimo", "octavo",  "noveno", "décimo",
};

// Words the user said, as part of a sentence the browser says.
std::string UserWords(std::string_view text) {
  return SpeechSafeText(text, kMaxCommandTextBytes);
}

std::string Joined(const std::vector<std::string>& words) {
  std::string out;
  for (const std::string& word : words) {
    if (!out.empty()) {
      out += ' ';
    }
    out += word;
  }
  return out;
}

std::string OrdinalText(int n, Language language) {
  const std::span<const std::string_view> ordinals =
      language == Language::kSpanish
          ? std::span<const std::string_view>(kOrdinalsEs)
          : std::span<const std::string_view>(kOrdinals);
  if (n >= 1 && n <= static_cast<int>(ordinals.size())) {
    return std::string(ordinals[static_cast<size_t>(n - 1)]);
  }
  return std::string();
}

// "the live one by Queen": the words of a description, in English's order.
std::string EnglishDescription(const ChoiceRef& ref) {
  std::string out = "the";
  if (ref.ordinal > 0) {
    const std::string ordinal = OrdinalText(ref.ordinal, Language::kEnglish);
    if (!ordinal.empty()) {
      out += " " + ordinal;
    }
  }
  if (!ref.words.empty()) {
    out += " " + Joined(ref.words);
  }
  out += " one";
  if (!ref.by_words.empty()) {
    out += " by " + Joined(ref.by_words);
  }
  if (!ref.from_words.empty()) {
    out += " from " + Joined(ref.from_words);
  }
  return out;
}

// "el primero en directo de Queen": Spanish's order (the user's own words
// after the ordinal, "de" before whose and where from).
std::string SpanishDescription(const ChoiceRef& ref) {
  std::string out = "el";
  if (ref.ordinal > 0) {
    const std::string ordinal = OrdinalText(ref.ordinal, Language::kSpanish);
    if (!ordinal.empty()) {
      out += " " + ordinal;
    }
  }
  if (!ref.words.empty()) {
    out += " " + Joined(ref.words);
  }
  if (ref.ordinal <= 0 && ref.words.empty()) {
    out += " uno";
  }
  if (!ref.by_words.empty()) {
    out += " de " + Joined(ref.by_words);
  }
  if (!ref.from_words.empty()) {
    out += " de " + Joined(ref.from_words);
  }
  return out;
}

std::string ChoiceText(const ChoiceRef& ref, Language language) {
  switch (ref.kind) {
    case ChoiceRef::Kind::kPosition: {
      if (ref.from_end) {
        if (ref.ordinal == 1) {
          return Said(language, "the last one", {});
        }
        const std::string ordinal = OrdinalText(ref.ordinal, language);
        return ordinal.empty()
                   ? Said(language, "number {0} from the end",
                          {std::to_string(ref.ordinal)})
                   : Said(language, "the {0} to last one", {ordinal});
      }
      const std::string ordinal = OrdinalText(ref.ordinal, language);
      return ordinal.empty()
                 ? Said(language, "number {0}", {std::to_string(ref.ordinal)})
                 : Said(language, "the {0} one", {ordinal});
    }
    case ChoiceRef::Kind::kDescription:
      return language == Language::kSpanish ? SpanishDescription(ref)
                                            : EnglishDescription(ref);
    case ChoiceRef::Kind::kNumber:
      return Said(language, "number {0}", {std::to_string(ref.number)});
    case ChoiceRef::Kind::kRelative:
    case ChoiceRef::Kind::kNone:
      break;
  }
  return Said(language, "one", {});
}

// The choice with what was done with it: "play the first one".
std::string ChoiceStepText(ChoiceVerb verb,
                           const std::string& choice,
                           Language language) {
  switch (verb) {
    case ChoiceVerb::kDefault:
      return Said(language, "choose {0}", {choice});
    case ChoiceVerb::kOpen:
      return Said(language, "open {0}", {choice});
    case ChoiceVerb::kPlay:
      return Said(language, "play {0}", {choice});
    case ChoiceVerb::kClick:
      return Said(language, "click {0}", {choice});
    case ChoiceVerb::kSelect:
      return Said(language, "select {0}", {choice});
    case ChoiceVerb::kRead:
      return Said(language, "read {0}", {choice});
    case ChoiceVerb::kDescribe:
      return Said(language, "describe {0}", {choice});
  }
  return Said(language, "choose {0}", {choice});
}

// The planner's step (IsAgentStep()), as the browser says it.
std::string DescribeAgentStep(const RoutineStep& step, Language language) {
  const PlannerStep& a = step.agent;
  // "the button" (the catalog has the kinds the page map says).
  const std::string element =
      a.label.empty() ? Localized(language, "the " + UserWords(a.kind))
                      : UserWords(a.label);
  switch (a.action) {
    case PlannerStep::Action::kNavigate:
      return step.host.empty()
                 ? Said(language, "open a page", {})
                 : Said(language, "open {0}", {UserWords(step.host)});
    case PlannerStep::Action::kSearch:
      return a.query.empty() ? Said(language, "search the web", {})
                             : Said(language, "search the web for {0}",
                                    {UserWords(a.query)});
    case PlannerStep::Action::kClick:
      return Said(language, "click {0}", {element});
    case PlannerStep::Action::kType:
      return step.variable.empty()
                 ? Said(language, "type {0} in {1}",
                        {UserWords(a.text), element})
                 : Said(language, "type what you tell me in {0}", {element});
    case PlannerStep::Action::kSelect:
      return Said(language, "choose {0} in {1}",
                  {UserWords(a.option), element});
    case PlannerStep::Action::kScroll:
      return a.direction == "up" ? Said(language, "scroll up", {})
                                 : Said(language, "scroll down", {});
    case PlannerStep::Action::kUnkept:
      return Said(language, "a step the AI took that I can't repeat", {});
    case PlannerStep::Action::kNone:
      break;
  }
  return Said(language, "a step the AI took", {});
}

enum class Way : uint8_t { kDown, kUp, kLeft, kRight };

std::string ScrollText(Way way, const VoiceCommand& c, Language language) {
  const bool pages = c.scroll == ScrollAmount::kPage && c.count > 1;
  const bool bit = !pages && c.scroll == ScrollAmount::kHalfPage;
  const std::string count = std::to_string(c.count);
  switch (way) {
    case Way::kDown:
      return pages ? Said(language, "scroll down {0} pages", {count})
             : bit ? Said(language, "scroll down a bit", {})
                   : Said(language, "scroll down", {});
    case Way::kUp:
      return pages ? Said(language, "scroll up {0} pages", {count})
             : bit ? Said(language, "scroll up a bit", {})
                   : Said(language, "scroll up", {});
    case Way::kLeft:
      return pages ? Said(language, "scroll left {0} pages", {count})
             : bit ? Said(language, "scroll left a bit", {})
                   : Said(language, "scroll left", {});
    case Way::kRight:
      return pages ? Said(language, "scroll right {0} pages", {count})
             : bit ? Said(language, "scroll right a bit", {})
                   : Said(language, "scroll right", {});
  }
  return Said(language, "scroll down", {});
}

}  // namespace

LearningEnvironment::LearningEnvironment() = default;
LearningEnvironment::~LearningEnvironment() = default;
LearningEnvironment::LearningEnvironment(const LearningEnvironment&) = default;
LearningEnvironment& LearningEnvironment::operator=(
    const LearningEnvironment&) = default;
LearningEnvironment::LearningEnvironment(LearningEnvironment&&) = default;
LearningEnvironment& LearningEnvironment::operator=(LearningEnvironment&&) =
    default;

RoutineStep::RoutineStep() = default;
RoutineStep::~RoutineStep() = default;
RoutineStep::RoutineStep(const RoutineStep&) = default;
RoutineStep& RoutineStep::operator=(const RoutineStep&) = default;
RoutineStep::RoutineStep(RoutineStep&&) = default;
RoutineStep& RoutineStep::operator=(RoutineStep&&) = default;

bool IsRoutineKind(VoiceCommandKind kind) {
  switch (kind) {
    case K::kOpenSite:
    case K::kSearch:
    case K::kFindInPage:
    case K::kBack:
    case K::kForward:
    case K::kReload:
    case K::kHome:
    case K::kNextPage:
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
    case K::kReadPage:
    case K::kReadHeadings:
    case K::kReadLinks:
    case K::kReadResults:
    case K::kReadTitle:
    case K::kDescribeScreen:
    case K::kChoose:
    case K::kType:
    case K::kNewLine:
    case K::kClearField:
    case K::kSubmit:
      return true;
    default:
      return false;
  }
}

bool IsTaskStart(const RoutineStep& step) {
  if (step.planned) {
    return false;
  }
  const VoiceCommand& c = step.command;
  return c.kind == K::kOpenSite || c.kind == K::kHome ||
         (c.kind == K::kSearch && (c.scope == SearchScope::kNamedSite ||
                                   c.scope == SearchScope::kDefaultEngine));
}

bool IsAgentStep(const RoutineStep& step) {
  return step.planned && step.agent.action != PlannerStep::Action::kNone;
}

bool IsUnkeptStep(const RoutineStep& step) {
  return step.planned && step.agent.action == PlannerStep::Action::kUnkept;
}

VoiceCommand RoutineCommand(const VoiceCommand& c) {
  VoiceCommand out;
  out.kind = c.kind;
  switch (c.kind) {
    case K::kOpenSite:
      out.site = c.site;
      out.site_is_address = c.site_is_address;
      out.site_words = c.site_words;
      out.new_tab = c.new_tab;
      break;
    case K::kSearch:
      out.text = c.text;
      out.scope = c.scope;
      out.site = c.site;
      out.site_is_address = c.site_is_address;
      out.site_words = c.site_words;
      out.whole_query = c.whole_query;
      break;
    case K::kFindInPage:
    case K::kType:
      out.text = c.text;
      break;
    case K::kChoose:
      out.verb = c.verb;
      out.choice = c.choice;
      break;
    case K::kScrollDown:
    case K::kScrollUp:
    case K::kScrollLeft:
    case K::kScrollRight:
      out.scroll = c.scroll;
      out.count = c.count;
      break;
    case K::kSkipForward:
    case K::kSkipBack:
      out.seconds = c.seconds;
      break;
    default:
      break;
  }
  return out;
}

std::optional<ChoiceRef> RoutineChoice(const ChoiceRef& said,
                                       int position,
                                       std::string_view label,
                                       bool answered_offer) {
  if (said.kind == ChoiceRef::Kind::kNone) {
    return std::nullopt;
  }
  const bool momentary = answered_offer ||
                         said.kind == ChoiceRef::Kind::kNumber ||
                         said.kind == ChoiceRef::Kind::kRelative;
  if (!momentary) {
    return said;
  }
  if (position > 0) {
    ChoiceRef ref;
    ref.kind = ChoiceRef::Kind::kPosition;
    ref.ordinal = position;
    return ref;
  }
  const std::string name = SpeechSafeText(label, kMaxChoiceNameChars);
  if (name.empty()) {
    return std::nullopt;
  }
  std::optional<ChoiceRef> by_name =
      ParseChoiceRef(name, Language::kEnglish, /*bare_numbers=*/false);
  if (!by_name || by_name->kind != ChoiceRef::Kind::kDescription) {
    return std::nullopt;
  }
  return by_name;
}

std::optional<RoutineStep> RoutineStepFor(const VoiceCommand& command,
                                          std::string_view host,
                                          int position,
                                          std::string_view label,
                                          bool answered_offer) {
  if (!IsRoutineKind(command.kind)) {
    return std::nullopt;
  }
  RoutineStep step;
  step.command = RoutineCommand(command);
  step.host = HintHost(host);
  switch (command.kind) {
    case K::kChoose: {
      std::optional<ChoiceRef> choice =
          RoutineChoice(command.choice, position, label, answered_offer);
      if (!choice) {
        return std::nullopt;
      }
      step.command.choice = std::move(*choice);
      break;
    }
    case K::kSearch:
    case K::kType:
    case K::kFindInPage:
      if (command.text.empty()) {
        return std::nullopt;
      }
      break;
    case K::kOpenSite:
      if (command.site.empty()) {
        return std::nullopt;
      }
      break;
    default:
      break;
  }
  return step;
}

VoiceCommand CommandForStep(const RoutineStep& step, std::string_view value) {
  VoiceCommand command = step.command;
  if (!step.variable.empty()) {
    command.text = std::string(value);
  }
  command.heard = DescribeRoutineStep(step);
  return command;
}

std::string DescribeRoutineStep(const RoutineStep& step) {
  return DescribeRoutineStep(step, Language::kEnglish);
}

std::string DescribeRoutineStep(const RoutineStep& step, Language language) {
  if (IsAgentStep(step)) {
    return DescribeAgentStep(step, language);
  }
  const VoiceCommand& c = step.command;
  const std::string text = step.variable.empty()
                               ? UserWords(c.text)
                               : Said(language, "what you tell me", {});
  const std::string site =
      UserWords(c.site_words.empty() ? c.site : c.site_words);
  switch (c.kind) {
    case K::kOpenSite:
      return c.new_tab ? Said(language, "open {0} in a new tab", {site})
                       : Said(language, "open {0}", {site});
    case K::kSearch:
      switch (c.scope) {
        case SearchScope::kNamedSite:
          return Said(language, "search {0} for {1}", {site, text});
        case SearchScope::kDefaultEngine:
          return Said(language, "search the web for {0}", {text});
        case SearchScope::kCurrentSite:
          return Said(language, "search this site for {0}", {text});
        case SearchScope::kAuto:
          return Said(language, "search for {0}", {text});
      }
      return Said(language, "search for {0}", {text});
    case K::kFindInPage:
      return Said(language, "find {0}", {text});
    case K::kBack:
      return Said(language, "go back", {});
    case K::kForward:
      return Said(language, "go forward", {});
    case K::kReload:
      return Said(language, "reload the page", {});
    case K::kHome:
      return Said(language, "go home", {});
    case K::kNextPage:
      return Said(language, "go to the next page", {});
    case K::kScrollDown:
      return ScrollText(Way::kDown, c, language);
    case K::kScrollUp:
      return ScrollText(Way::kUp, c, language);
    case K::kScrollLeft:
      return ScrollText(Way::kLeft, c, language);
    case K::kScrollRight:
      return ScrollText(Way::kRight, c, language);
    case K::kScrollToTop:
      return Said(language, "go to the top", {});
    case K::kScrollToBottom:
      return Said(language, "go to the bottom", {});
    case K::kZoomIn:
      return Said(language, "zoom in", {});
    case K::kZoomOut:
      return Said(language, "zoom out", {});
    case K::kZoomReset:
      return Said(language, "reset the zoom", {});
    case K::kPlay:
      return Said(language, "play", {});
    case K::kPause:
      return Said(language, "pause", {});
    case K::kResume:
      return Said(language, "resume", {});
    case K::kVolumeUp:
      return Said(language, "louder", {});
    case K::kVolumeDown:
      return Said(language, "quieter", {});
    case K::kMute:
      return Said(language, "mute", {});
    case K::kUnmute:
      return Said(language, "unmute", {});
    case K::kSkipForward:
      return c.seconds > 0 ? Said(language, "skip ahead {0}",
                                  {SpokenLength(c.seconds, language)})
                           : Said(language, "skip ahead", {});
    case K::kSkipBack:
      return c.seconds > 0 ? Said(language, "skip back {0}",
                                  {SpokenLength(c.seconds, language)})
                           : Said(language, "skip back", {});
    case K::kRestart:
      return Said(language, "play from the start", {});
    case K::kNextTrack:
      return Said(language, "next track", {});
    case K::kPreviousTrack:
      return Said(language, "previous track", {});
    case K::kFullScreen:
      return Said(language, "full screen", {});
    case K::kExitFullScreen:
      return Said(language, "leave full screen", {});
    case K::kCaptionsOn:
      return Said(language, "turn on captions", {});
    case K::kCaptionsOff:
      return Said(language, "turn off captions", {});
    case K::kReadPage:
      return Said(language, "read the page", {});
    case K::kReadHeadings:
      return Said(language, "read the headings", {});
    case K::kReadLinks:
      return Said(language, "read the links", {});
    case K::kReadResults:
      return Said(language, "read the results", {});
    case K::kReadTitle:
      return Said(language, "read the title", {});
    case K::kDescribeScreen:
      return Said(language, "say what's on the screen", {});
    case K::kChoose:
      return ChoiceStepText(c.verb, ChoiceText(c.choice, language), language);
    case K::kType:
      return Said(language, "type {0}", {text});
    case K::kNewLine:
      return Said(language, "start a new line", {});
    case K::kClearField:
      return Said(language, "clear the field", {});
    case K::kSubmit:
      return Said(language, "submit", {});
    default:
      break;
  }
  return std::string(VoiceCommandKindName(c.kind));
}

std::string JoinSpokenList(const std::vector<std::string>& items) {
  return JoinSpokenList(items, Language::kEnglish);
}

std::string JoinSpokenList(const std::vector<std::string>& items,
                           Language language) {
  const std::string last = Localized(language, " and ");
  std::string out;
  for (size_t i = 0; i < items.size(); ++i) {
    if (i > 0) {
      // Spanish's "y" is "e" before an "i" sound ("tomate e higos").
      const bool e =
          language == Language::kSpanish && i + 1 == items.size() &&
          (items[i].rfind("i", 0) == 0 || items[i].rfind("I", 0) == 0 ||
           items[i].rfind("hi", 0) == 0 || items[i].rfind("Hi", 0) == 0) &&
          items[i].rfind("hie", 0) != 0 && items[i].rfind("Hie", 0) != 0;
      out += i + 1 == items.size() ? (e ? std::string(" e ") : last) : ", ";
    }
    out += items[i];
  }
  return out;
}

std::string RoutineDisplayName(std::string_view said) {
  if (said.size() > kMaxUtteranceBytes || !IsValidUtf8(said)) {
    return std::string();
  }
  std::string name = CleanSpokenText(said, kMaxUtteranceBytes);
  if (name.empty() || name.size() > kMaxRoutineNameBytes) {
    return std::string();
  }
  NormalizeOptions options;
  options.max_words = kMaxRoutineNameWords + 1;
  const NormalizedText text = NormalizeSpokenText(name, options);
  if (text.words.empty() || text.words.size() > kMaxRoutineNameWords ||
      text.truncated) {
    return std::string();
  }
  bool letter = false;
  for (size_t i = 0; i < text.words.size() && !letter; ++i) {
    letter = !text.words[i].numeric;
  }
  if (!letter) {
    return std::string();
  }
  ParseContext answer;
  answer.expecting_yes_no = true;
  const VoiceCommandKind kind = ParseVoiceCommand(name, answer).kind;
  if (kind == K::kYes || kind == K::kNo) {
    return std::string();
  }
  return name;
}

std::string RoutineNameKey(std::string_view name, Language language) {
  const std::string folded = FoldForComparison(
      name.substr(0, std::min(name.size(), kMaxUtteranceBytes)));
  static constexpr std::string_view kEnglishArticles[] = {"my ", "the ", "a ",
                                                          "an "};
  static constexpr std::string_view kSpanishArticles[] = {
      "mi ", "mis ", "el ", "la ", "los ", "las ", "un ", "una "};
  const std::span<const std::string_view> articles =
      language == Language::kSpanish
          ? std::span<const std::string_view>(kSpanishArticles)
          : std::span<const std::string_view>(kEnglishArticles);
  size_t start = 0;
  while (true) {
    bool dropped = false;
    for (std::string_view article : articles) {
      if (folded.compare(start, article.size(), article) == 0 &&
          folded.size() > start + article.size()) {
        start += article.size();
        dropped = true;
        break;
      }
    }
    if (!dropped) {
      break;
    }
  }
  // Numbers as digits, whether written or said: "my news (2)" is said "my
  // news two", and a recognizer may write either.
  const std::string_view rest = std::string_view(folded).substr(start);
  WordList words;
  for (size_t at = 0; at < rest.size();) {
    size_t space = rest.find(' ', at);
    if (space == std::string_view::npos) {
      space = rest.size();
    }
    if (space > at) {
      words.push_back(rest.substr(at, space - at));
    }
    at = space + 1;
  }
  NumberOptions cardinals;
  cardinals.ordinals = false;
  std::string key;
  for (size_t i = 0; i < words.size();) {
    if (!key.empty()) {
      key.push_back(' ');
    }
    const auto number = ReadNumberIn(words, i, language, cardinals);
    if (number && number->words > 0) {
      key += std::to_string(number->value);
      i += number->words;
    } else {
      key.append(words[i]);
      ++i;
    }
  }
  return key;
}

RoutineNameMatch MatchRoutineName(const std::vector<std::string>& names,
                                  std::string_view said,
                                  Language language) {
  RoutineNameMatch match;
  const std::string key = RoutineNameKey(said, language);
  if (key.empty()) {
    return match;
  }
  const std::string* found = nullptr;
  for (const std::string& name : names) {
    if (RoutineNameKey(name, language) == key) {
      found = &name;
      ++match.count;
    }
  }
  if (match.count == 1) {
    match.name = found;
  }
  return match;
}

const std::string* FindRoutineName(const std::vector<std::string>& names,
                                   std::string_view said,
                                   Language language) {
  return MatchRoutineName(names, said, language).name;
}

RoutineNameMatch MatchRoutineNameInAnyLanguage(
    const std::vector<std::string>& names,
    std::string_view name) {
  // Every language voice mode can use.
  static constexpr std::array<Language, 2> kLanguages = {Language::kEnglish,
                                                         Language::kSpanish};
  std::array<std::string, kLanguages.size()> keys;
  for (size_t i = 0; i < kLanguages.size(); ++i) {
    keys[i] = RoutineNameKey(name, kLanguages[i]);
  }
  RoutineNameMatch match;
  const std::string* found = nullptr;
  for (const std::string& other : names) {
    bool clashes = false;
    for (size_t i = 0; i < kLanguages.size() && !clashes; ++i) {
      clashes =
          !keys[i].empty() && RoutineNameKey(other, kLanguages[i]) == keys[i];
    }
    if (clashes) {
      found = &other;
      ++match.count;
    }
  }
  if (match.count == 1) {
    match.name = found;
  }
  return match;
}

std::string UniqueRoutineName(const std::vector<std::string>& names,
                              std::string_view name) {
  if (!MatchRoutineNameInAnyLanguage(names, name).count) {
    return std::string(name);
  }
  // "my news (2)" brought in again is "my news (3)", not "my news (2) (2)".
  std::string_view base = name;
  if (base.size() > 3 && base.back() == ')') {
    const size_t open = base.rfind(" (");
    if (open != std::string_view::npos && open + 3 < base.size()) {
      const std::string_view digits =
          base.substr(open + 2, base.size() - open - 3);
      if (std::all_of(digits.begin(), digits.end(),
                      [](char c) { return c >= '0' && c <= '9'; })) {
        base = base.substr(0, open);
      }
    }
  }
  for (int n = 2; n <= kMaxRoutineNameCopies; ++n) {
    std::string candidate = std::string(base) + " (" + std::to_string(n) + ")";
    if (!MatchRoutineNameInAnyLanguage(names, candidate).count) {
      return candidate;
    }
  }
  return std::string();
}

RoutineRecorder::RoutineRecorder() = default;
RoutineRecorder::~RoutineRecorder() = default;

void RoutineRecorder::Add(RoutineStep step) {
  if (IsTaskStart(step)) {
    steps_.clear();
  }
  steps_.push_back(std::move(step));
  if (steps_.size() > kMaxThisSteps) {
    steps_.erase(steps_.begin());
  }
  ++version_;
}

void RoutineRecorder::DropLast() {
  if (!steps_.empty()) {
    steps_.pop_back();
    ++version_;
  }
}

void RoutineRecorder::Clear() {
  steps_.clear();
  ++version_;
}

bool RoutineRecorder::planned() const {
  return std::any_of(steps_.begin(), steps_.end(),
                     [](const RoutineStep& step) { return step.planned; });
}

std::string RoutineRecorder::refusal() const {
  for (const RoutineStep& step : steps_) {
    if (IsUnkeptStep(step)) {
      return step.agent.why.empty()
                 ? std::string(
                       SaidLater("one of the AI's steps can't be repeated."))
                 : step.agent.why;
    }
  }
  return std::string();
}

}  // namespace voice
}  // namespace molt_ai
