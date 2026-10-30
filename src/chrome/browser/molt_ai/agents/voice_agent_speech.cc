// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/agents/voice_agent_speech.h"

#include "chrome/browser/molt_ai/voice/page_map_text.h"

namespace molt_ai {
namespace voice_agent {

namespace {

// What follows a failure: what the user can do instead.
constexpr char kWhatICanDo[] =
    "Say what can I say to hear the commands I know.";

std::string Spoken(std::string_view text) {
  return voice::SpeechSafeText(text, kMaxSpokenChars);
}

bool IsUpper(char c) {
  return c >= 'A' && c <= 'Z';
}

// "Search the site" -> "search the site"; "URL", "I" and "iPhone" stay.
std::string LowerFirst(std::string text) {
  if (text.size() >= 2 && IsUpper(text[0]) && !IsUpper(text[1]) &&
      text[1] != ' ' && text[1] != '\'') {
    text[0] = static_cast<char>(text[0] - 'A' + 'a');
  }
  return text;
}

std::string WithoutFinalStop(std::string text) {
  while (!text.empty() && (text.back() == '.' || text.back() == ' ')) {
    text.pop_back();
  }
  return text;
}

void ReplaceAll(std::string* text, std::string_view from, std::string_view to) {
  if (from.empty()) {
    return;
  }
  for (size_t at = text->find(from); at != std::string::npos;
       at = text->find(from, at + to.size())) {
    text->replace(at, from.size(), to);
  }
}

// "https://www.x.test/a?b" -> "x.test".
std::string HostOf(std::string_view url) {
  for (std::string_view scheme : {"https://", "http://"}) {
    if (url.substr(0, scheme.size()) == scheme) {
      url.remove_prefix(scheme.size());
      break;
    }
  }
  const size_t end = url.find_first_of("/?#");
  if (end != std::string_view::npos) {
    url = url.substr(0, end);
  }
  if (url.substr(0, 4) == "www.") {
    url.remove_prefix(4);
  }
  return std::string(url);
}

// The name inside the first pair of double quotes of a description
// (`button "Search" (text field)`); the description itself without one.
std::string LabelOf(std::string_view target) {
  const size_t open = target.find('"');
  if (open == std::string_view::npos) {
    return std::string(target);
  }
  const size_t close = target.find('"', open + 1);
  if (close == std::string_view::npos) {
    return std::string(target.substr(open + 1));
  }
  return std::string(target.substr(open + 1, close - open - 1));
}

std::string ReadBack(std::string_view text) {
  return voice::SpeechSafeText(text, kMaxReadBackChars);
}

}  // namespace

std::string StartSpeech() {
  return "Working on it.";
}

std::string PlanStepSpeech(std::string_view goal, int index, int count) {
  const std::string step = WithoutFinalStop(LowerFirst(Spoken(goal)));
  if (step.empty()) {
    return std::string();
  }
  return Spoken((index > 1 ? "Next, I'll " : "I'll ") + step + ".");
}

std::string ReplanSpeech(std::string_view goal) {
  const std::string step = WithoutFinalStop(LowerFirst(Spoken(goal)));
  if (step.empty()) {
    return "That didn't work, so I'm trying another way.";
  }
  return Spoken("That didn't work, so I'll " + step + " instead.");
}

std::string ActionSpeech(std::string_view action,
                         std::string_view target,
                         std::string_view value) {
  std::string said;
  if (action == "NAVIGATE") {
    const std::string host = HostOf(target);
    said = host.empty() ? "Opening the page." : "Opening " + host + ".";
  } else if (action == "SEARCH") {
    said = "Searching the web for " + ReadBack(target) + ".";
  } else if (action == "CLICK") {
    const std::string label = ReadBack(LabelOf(target));
    said = label.empty() ? "Pressing it." : "Pressing " + label + ".";
  } else if (action == "TYPE") {
    const std::string label = ReadBack(LabelOf(target));
    const std::string in = label.empty() ? std::string() : " in " + label;
    const std::string text = ReadBack(value);
    said = text.empty() ? "Clearing the field" + in + "."
                        : "Typing " + text + in + ".";
  } else if (action == "SELECT") {
    const std::string label = ReadBack(LabelOf(target));
    said = "Choosing " + ReadBack(value) +
           (label.empty() ? std::string() : " in " + label) + ".";
  } else if (action == "FILL_FORM") {
    said = "Filling in the form.";
  } else if (action == "SCROLL") {
    said =
        value == "up" || target == "up" ? "Scrolling up." : "Scrolling down.";
  } else if (action == "CALL_TOOL") {
    const std::string name = ReadBack(LabelOf(target));
    said = name.empty() ? "Using a tool of the page."
                        : "Using the page's " + name + " tool.";
  }
  return Spoken(said);
}

std::string QuestionWhat(std::string_view message) {
  std::string what = voice::SpeechSafeText(message, kMaxSpokenChars);
  ReplaceAll(&what, "https://", "");
  ReplaceAll(&what, "http://", "");
  ReplaceAll(&what, " \xE2\x80\x94 ", ", ");
  ReplaceAll(&what, "\"", "");
  return Spoken(WithoutFinalStop(LowerFirst(std::move(what))));
}

std::string ResultSpeech(bool success, std::string_view result) {
  const std::string text = WithoutFinalStop(Spoken(result));
  if (success) {
    return Spoken(text.empty() ? std::string("Done.") : "Done. " + text + ".");
  }
  return Spoken(
      (text.empty() ? std::string("I stopped.") : "I stopped: " + text + ".") +
      " " + kWhatICanDo);
}

std::string NoPlannerSpeech(NoPlanner reason, std::string_view detail) {
  const std::string why = WithoutFinalStop(Spoken(detail));
  switch (reason) {
    case NoPlanner::kNoModel:
      return Spoken(
          std::string("I can't do that by myself yet: no on-device AI model "
                      "is installed. You can add one in the AI settings. ") +
          kWhatICanDo);
    case NoPlanner::kUnsupported:
      return Spoken("The on-device AI can't run on this computer" +
                    (why.empty() ? std::string() : ": " + why) + ". " +
                    kWhatICanDo);
    case NoPlanner::kLoadFailed:
      return Spoken("I couldn't start the on-device AI" +
                    (why.empty() ? std::string() : ": " + why) + ". " +
                    kWhatICanDo);
    case NoPlanner::kNoPage:
      return Spoken(
          std::string("There's no tab for me to work in. Say open "
                      "and a site's name first."));
  }
  return Spoken(kWhatICanDo);
}

}  // namespace voice_agent
}  // namespace molt_ai
