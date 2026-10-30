// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_routines_logic.h"

#include <algorithm>
#include <utility>

#include "chrome/browser/molt_ai/voice/page_map_text.h"
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

// Words the user said, as part of a sentence the browser says.
std::string Said(std::string_view text) {
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

std::string OrdinalText(int n) {
  if (n >= 1 && n <= static_cast<int>(std::size(kOrdinals))) {
    return std::string(kOrdinals[n - 1]);
  }
  return std::string();
}

std::string ChoiceText(const ChoiceRef& ref) {
  switch (ref.kind) {
    case ChoiceRef::Kind::kPosition: {
      if (ref.from_end) {
        if (ref.ordinal == 1) {
          return "the last one";
        }
        const std::string ordinal = OrdinalText(ref.ordinal);
        return ordinal.empty()
                   ? "number " + std::to_string(ref.ordinal) + " from the end"
                   : "the " + ordinal + " to last one";
      }
      const std::string ordinal = OrdinalText(ref.ordinal);
      return ordinal.empty() ? "number " + std::to_string(ref.ordinal)
                             : "the " + ordinal + " one";
    }
    case ChoiceRef::Kind::kDescription: {
      std::string out = "the";
      if (ref.ordinal > 0) {
        const std::string ordinal = OrdinalText(ref.ordinal);
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
    case ChoiceRef::Kind::kNumber:
      return "number " + std::to_string(ref.number);
    case ChoiceRef::Kind::kRelative:
    case ChoiceRef::Kind::kNone:
      break;
  }
  return "one";
}

std::string_view VerbText(ChoiceVerb verb) {
  switch (verb) {
    case ChoiceVerb::kDefault:
      return "choose";
    case ChoiceVerb::kOpen:
      return "open";
    case ChoiceVerb::kPlay:
      return "play";
    case ChoiceVerb::kClick:
      return "click";
    case ChoiceVerb::kSelect:
      return "select";
    case ChoiceVerb::kRead:
      return "read";
    case ChoiceVerb::kDescribe:
      return "describe";
  }
  return "choose";
}

std::string ScrollText(std::string_view way, const VoiceCommand& c) {
  std::string out = "scroll " + std::string(way);
  if (c.scroll == ScrollAmount::kPage && c.count > 1) {
    out += " " + std::to_string(c.count) + " pages";
  } else if (c.scroll == ScrollAmount::kHalfPage) {
    out += " a bit";
  }
  return out;
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
  const VoiceCommand& c = step.command;
  const std::string text =
      step.variable.empty() ? Said(c.text) : std::string("what you tell me");
  const std::string site =
      Said(c.site_words.empty() ? c.site : c.site_words);
  switch (c.kind) {
    case K::kOpenSite:
      return "open " + site + (c.new_tab ? " in a new tab" : "");
    case K::kSearch:
      switch (c.scope) {
        case SearchScope::kNamedSite:
          return "search " + site + " for " + text;
        case SearchScope::kDefaultEngine:
          return "search the web for " + text;
        case SearchScope::kCurrentSite:
          return "search this site for " + text;
        case SearchScope::kAuto:
          return "search for " + text;
      }
      return "search for " + text;
    case K::kFindInPage:
      return "find " + text;
    case K::kBack:
      return "go back";
    case K::kForward:
      return "go forward";
    case K::kReload:
      return "reload the page";
    case K::kHome:
      return "go home";
    case K::kNextPage:
      return "go to the next page";
    case K::kScrollDown:
      return ScrollText("down", c);
    case K::kScrollUp:
      return ScrollText("up", c);
    case K::kScrollLeft:
      return ScrollText("left", c);
    case K::kScrollRight:
      return ScrollText("right", c);
    case K::kScrollToTop:
      return "go to the top";
    case K::kScrollToBottom:
      return "go to the bottom";
    case K::kZoomIn:
      return "zoom in";
    case K::kZoomOut:
      return "zoom out";
    case K::kZoomReset:
      return "reset the zoom";
    case K::kPlay:
      return "play";
    case K::kPause:
      return "pause";
    case K::kResume:
      return "resume";
    case K::kVolumeUp:
      return "louder";
    case K::kVolumeDown:
      return "quieter";
    case K::kMute:
      return "mute";
    case K::kUnmute:
      return "unmute";
    case K::kSkipForward:
      return c.seconds > 0 ? "skip ahead " + SpokenLength(c.seconds)
                           : std::string("skip ahead");
    case K::kSkipBack:
      return c.seconds > 0 ? "skip back " + SpokenLength(c.seconds)
                           : std::string("skip back");
    case K::kRestart:
      return "play from the start";
    case K::kNextTrack:
      return "next track";
    case K::kPreviousTrack:
      return "previous track";
    case K::kFullScreen:
      return "full screen";
    case K::kExitFullScreen:
      return "leave full screen";
    case K::kCaptionsOn:
      return "turn on captions";
    case K::kCaptionsOff:
      return "turn off captions";
    case K::kReadPage:
      return "read the page";
    case K::kReadHeadings:
      return "read the headings";
    case K::kReadLinks:
      return "read the links";
    case K::kReadResults:
      return "read the results";
    case K::kReadTitle:
      return "read the title";
    case K::kDescribeScreen:
      return "say what's on the screen";
    case K::kChoose:
      return std::string(VerbText(c.verb)) + " " + ChoiceText(c.choice);
    case K::kType:
      return "type " + text;
    case K::kNewLine:
      return "start a new line";
    case K::kClearField:
      return "clear the field";
    case K::kSubmit:
      return "submit";
    default:
      break;
  }
  return std::string(VoiceCommandKindName(c.kind));
}

std::string JoinSpokenList(const std::vector<std::string>& items) {
  std::string out;
  for (size_t i = 0; i < items.size(); ++i) {
    if (i > 0) {
      out += i + 1 == items.size() ? " and " : ", ";
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

std::string RoutineNameKey(std::string_view name) {
  const std::string folded = FoldForComparison(
      name.substr(0, std::min(name.size(), kMaxUtteranceBytes)));
  size_t start = 0;
  while (true) {
    bool dropped = false;
    for (std::string_view article : {"my ", "the ", "a ", "an "}) {
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
  return folded.substr(start);
}

const std::string* FindRoutineName(const std::vector<std::string>& names,
                                   std::string_view said) {
  const std::string key = RoutineNameKey(said);
  if (key.empty()) {
    return nullptr;
  }
  for (const std::string& name : names) {
    if (RoutineNameKey(name) == key) {
      return &name;
    }
  }
  return nullptr;
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

}  // namespace voice
}  // namespace molt_ai
