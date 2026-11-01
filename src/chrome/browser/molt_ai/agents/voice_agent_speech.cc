// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/agents/voice_agent_speech.h"

#include "chrome/browser/molt_ai/voice/page_map_text.h"
#include "chrome/browser/molt_ai/voice/spoken_answers.h"

namespace molt_ai {
namespace voice_agent {

namespace {

using voice::Said;
using voice::SaidLater;

// Said in English, with whole sentences of the catalog
// (voice/spoken_answers.h): the voice loop says them in its language
// (VoiceController::PlannerSays(), voice::Relocalized()). A failure is
// followed by what the user can do instead ("Say what can I say ...").
constexpr voice::Language kEn = voice::Language::kEnglish;

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
  return std::string(SaidLater("Working on it."));
}

std::string PlanStepSpeech(std::string_view goal, int index, int count) {
  const std::string step = WithoutFinalStop(LowerFirst(Spoken(goal)));
  if (step.empty()) {
    return std::string();
  }
  return Spoken(index > 1 ? Said(kEn, "Next, I'll {0}.", {step})
                          : Said(kEn, "I'll {0}.", {step}));
}

std::string ReplanSpeech(std::string_view goal) {
  const std::string step = WithoutFinalStop(LowerFirst(Spoken(goal)));
  if (step.empty()) {
    return std::string(
        SaidLater("That didn't work, so I'm trying another way."));
  }
  return Spoken(Said(kEn, "That didn't work, so I'll {0} instead.", {step}));
}

std::string ActionSpeech(std::string_view action,
                         std::string_view target,
                         std::string_view value) {
  std::string said;
  if (action == "NAVIGATE") {
    const std::string host = HostOf(target);
    said = host.empty() ? std::string(SaidLater("Opening the page."))
                        : Said(kEn, "Opening {0}.", {host});
  } else if (action == "SEARCH") {
    said = Said(kEn, "Searching the web for {0}.", {ReadBack(target)});
  } else if (action == "CLICK") {
    const std::string label = ReadBack(LabelOf(target));
    said = label.empty() ? std::string(SaidLater("Pressing it."))
                         : Said(kEn, "Pressing {0}.", {label});
  } else if (action == "TYPE") {
    const std::string label = ReadBack(LabelOf(target));
    const std::string text = ReadBack(value);
    if (text.empty()) {
      said = label.empty() ? std::string(SaidLater("Clearing the field."))
                           : Said(kEn, "Clearing the field in {0}.", {label});
    } else {
      said = label.empty() ? Said(kEn, "Typing {0}.", {text})
                           : Said(kEn, "Typing {0} in {1}.", {text, label});
    }
  } else if (action == "SELECT") {
    const std::string label = ReadBack(LabelOf(target));
    said = label.empty()
               ? Said(kEn, "Choosing {0}.", {ReadBack(value)})
               : Said(kEn, "Choosing {0} in {1}.", {ReadBack(value), label});
  } else if (action == "FILL_FORM") {
    said = SaidLater("Filling in the form.");
  } else if (action == "SCROLL") {
    const bool up = value == "up" || target == "up";
    said = up ? SaidLater("Scrolling up.") : SaidLater("Scrolling down.");
  } else if (action == "CALL_TOOL") {
    const std::string name = ReadBack(LabelOf(target));
    said = name.empty() ? std::string(SaidLater("Using a tool of the page."))
                        : Said(kEn, "Using the page's {0} tool.", {name});
  }
  return Spoken(said);
}

std::string QuestionWhat(std::string_view message) {
  std::string what = voice::SpeechSafeText(message, kMaxSpokenChars);
  // A click's question names the page's label between quotes
  // (agent_page_map::QuotedLabel()), which aren't heard: it is said as "the
  // one labelled ...", as one phrase, so its own punctuation can't make a
  // clause that sounds like the browser's ("Cancel, or no to cancel").
  constexpr std::string_view kClickQuote = "Click \"";
  if (what.rfind(kClickQuote, 0) == 0) {
    const size_t close = what.find('"', kClickQuote.size());
    if (close != std::string::npos) {
      std::string label =
          what.substr(kClickQuote.size(), close - kClickQuote.size());
      for (char& c : label) {
        if (c == '.' || c == ',' || c == ';' || c == ':' || c == '!' ||
            c == '?') {
          c = ' ';
        }
      }
      label = voice::SpeechSafeText(label, kMaxSpokenChars);
      what = (label.empty() ? std::string("Click")
                            : "Click the one labelled " + label) +
             what.substr(close + 1);
    }
  }
  ReplaceAll(&what, "https://", "");
  ReplaceAll(&what, "http://", "");
  ReplaceAll(&what, " \xE2\x80\x94 ", ", ");
  ReplaceAll(&what, "\"", "");
  return Spoken(WithoutFinalStop(LowerFirst(std::move(what))));
}

std::string ResultSpeech(bool success, std::string_view result) {
  const std::string text = WithoutFinalStop(Spoken(result));
  if (success) {
    return Spoken(text.empty() ? std::string(SaidLater("Done."))
                               : Said(kEn, "Done. {0}.", {text}));
  }
  return Spoken(
      text.empty()
          ? Said(kEn,
                 "I stopped. Say what can I say to hear the commands I know.",
                 {})
          : Said(kEn,
                 "I stopped: {0}. Say what can I say to hear the commands I "
                 "know.",
                 {text}));
}

std::string NoPlannerSpeech(NoPlanner reason, std::string_view detail) {
  const std::string why = WithoutFinalStop(Spoken(detail));
  switch (reason) {
    case NoPlanner::kNoModel:
      return Spoken(Said(kEn,
                         "I can't do that by myself yet: no on-device AI "
                         "model is installed. You can add one in the AI "
                         "settings. Say what can I say to hear the commands "
                         "I know.",
                         {}));
    case NoPlanner::kUnsupported:
      return Spoken(why.empty()
                        ? Said(kEn,
                               "The on-device AI can't run on this computer. "
                               "Say what can I say to hear the commands I "
                               "know.",
                               {})
                        : Said(kEn,
                               "The on-device AI can't run on this computer: "
                               "{0}. Say what can I say to hear the commands "
                               "I know.",
                               {why}));
    case NoPlanner::kLoadFailed:
      return Spoken(why.empty()
                        ? Said(kEn,
                               "I couldn't start the on-device AI. Say what "
                               "can I say to hear the commands I know.",
                               {})
                        : Said(kEn,
                               "I couldn't start the on-device AI: {0}. Say "
                               "what can I say to hear the commands I know.",
                               {why}));
    case NoPlanner::kNoPage:
      return Spoken(Said(kEn,
                         "There's no tab for me to work in. Say open and a "
                         "site's name first.",
                         {}));
  }
  return Spoken(SaidLater("Say what can I say to hear the commands I know."));
}

}  // namespace voice_agent
}  // namespace molt_ai
