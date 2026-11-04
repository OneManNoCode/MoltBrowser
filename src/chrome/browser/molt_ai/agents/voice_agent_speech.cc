// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/agents/voice_agent_speech.h"

#include <string>
#include <utility>
#include <vector>

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

// ---- The web agent's own questions ----------------------------------------

namespace {

// "https://shop.example" -> "shop.example" (as QuestionWhat() says it).
std::string WithoutScheme(std::string_view origin) {
  for (std::string_view scheme : {"https://", "http://"}) {
    if (origin.substr(0, scheme.size()) == scheme) {
      origin.remove_prefix(scheme.size());
      break;
    }
  }
  return Spoken(origin);
}

// The page's label of a click (QuotedLabel()'s "Pay now", between quotes),
// as one phrase: its punctuation said as spaces. "" for a target that
// isn't a quoted label (the model's selector: never said).
std::string ClickLabel(std::string_view what) {
  if (what.size() < 2 || what.front() != '"' || what.back() != '"') {
    return std::string();
  }
  std::string label(what.substr(1, what.size() - 2));
  for (char& c : label) {
    if (c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?' ||
        c == '"') {
      c = ' ';
    }
  }
  return Spoken(label);
}

// |effect|'s origin when it starts with |head|.
bool EffectOf(std::string_view effect,
              std::string_view head,
              std::string* origin) {
  if (effect.substr(0, head.size()) != head) {
    return false;
  }
  *origin = WithoutScheme(effect.substr(head.size()));
  return !origin->empty();
}

}  // namespace

std::string OpenQuestion(std::string_view origin) {
  return Spoken(Said(kEn, "open the site {0}", {WithoutScheme(origin)}));
}

std::string ContinueQuestion(std::string_view origin) {
  return Spoken(Said(kEn, "keep working on {0} (the page moved there)",
                     {WithoutScheme(origin)}));
}

std::string ClickQuestion(std::string_view label,
                          const std::vector<std::string>& effects) {
  std::string opens;
  std::string form;
  bool form_to = false;
  for (const std::string& effect : effects) {
    std::string origin;
    if (EffectOf(effect, "opens ", &origin) && opens.empty()) {
      opens = std::move(origin);
    } else if (EffectOf(effect, "submits a form on ", &origin) &&
               form.empty()) {
      form = std::move(origin);
    } else if (EffectOf(effect, "submits a form to ", &origin) &&
               form.empty()) {
      form = std::move(origin);
      form_to = true;
    } else {
      return std::string();
    }
  }
  if (opens.empty() && form.empty()) {
    return std::string();
  }
  const std::string name = ClickLabel(label);
  std::string said;
  if (!name.empty()) {
    if (!opens.empty() && !form.empty()) {
      said = form_to ? Said(kEn,
                            "click the one labelled {0}, it opens {1} and "
                            "submits a form to {2}",
                            {name, opens, form})
                     : Said(kEn,
                            "click the one labelled {0}, it opens {1} and "
                            "submits a form on {2}",
                            {name, opens, form});
    } else if (!opens.empty()) {
      said =
          Said(kEn, "click the one labelled {0}, it opens {1}", {name, opens});
    } else {
      said = form_to ? Said(kEn,
                            "click the one labelled {0}, it submits a form "
                            "to {1}",
                            {name, form})
                     : Said(kEn,
                            "click the one labelled {0}, it submits a form "
                            "on {1}",
                            {name, form});
    }
  } else if (!opens.empty() && !form.empty()) {
    said = form_to ? Said(kEn, "click, it opens {0} and submits a form to {1}",
                          {opens, form})
                   : Said(kEn, "click, it opens {0} and submits a form on {1}",
                          {opens, form});
  } else if (!opens.empty()) {
    said = Said(kEn, "click, it opens {0}", {opens});
  } else {
    said = form_to ? Said(kEn, "click, it submits a form to {0}", {form})
                   : Said(kEn, "click, it submits a form on {0}", {form});
  }
  return Spoken(said);
}

std::string FieldQuestion(bool fill,
                          std::string_view field,
                          std::string_view origin) {
  const std::string where = WithoutScheme(origin);
  std::string said;
  if (field == "password") {
    said = fill ? Said(kEn, "fill a password field on {0}", {where})
                : Said(kEn, "type into a password field on {0}", {where});
  } else if (field == "payment card") {
    said = fill ? Said(kEn, "fill a payment card field on {0}", {where})
                : Said(kEn, "type into a payment card field on {0}", {where});
  } else if (field == "one-time code") {
    said = fill ? Said(kEn, "fill a one-time code field on {0}", {where})
                : Said(kEn, "type into a one-time code field on {0}", {where});
  } else if (field == "form") {
    said = fill ? Said(kEn, "fill a form field on {0}", {where})
                : Said(kEn, "type into a form field on {0}", {where});
  }
  return said.empty() ? said : Spoken(said);
}

std::string ToolQuestion(std::string_view name,
                         bool other_site,
                         std::string_view origin) {
  const std::string tool = Spoken(name);
  return Spoken(
      other_site ? Said(kEn, "use another site's {0} tool, in a frame on {1}",
                        {tool, WithoutScheme(origin)})
                 : Said(kEn, "use the page's {0} tool", {tool}));
}

}  // namespace voice_agent
}  // namespace molt_ai
