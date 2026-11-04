// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// What voice mode says while the web agent works on an open-ended request
// (docs/design/TOUCHLESS_BROWSING.md §6.2, the planner fallback; the glue is
// voice_agent_planner.h): each planned step before it runs ("I'll search
// the site for trains to Lyon."), each action when there is no plan, the
// agent's questions as the voice loop asks them ("Say yes to open
// evil.example, or no to cancel."), the result, and what it can do instead
// when there's no model.
//
// Plan steps, results and element names are model output or page text:
// everything returned here went through voice::SpeechSafeText() (no
// control, bidi or invisible characters, one line, capped), and the voice
// loop cleans what it says once more.
//
// Std-only; unit-tested in molt_ai_unittests.

#ifndef CHROME_BROWSER_MOLT_AI_AGENTS_VOICE_AGENT_SPEECH_H_
#define CHROME_BROWSER_MOLT_AI_AGENTS_VOICE_AGENT_SPEECH_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace molt_ai {
namespace voice_agent {

// One spoken text, in code points.
inline constexpr size_t kMaxSpokenChars = 300;
// Typed text and options as they are read back, in code points.
inline constexpr size_t kMaxReadBackChars = 60;

// Said as the run starts (the model may take a while to load and plan).
std::string StartSpeech();

// Plan step |index| (1-based) of |count|, before it runs: "I'll <goal>.",
// "Next, I'll <goal>." after the first.
std::string PlanStepSpeech(std::string_view goal, int index, int count);
// A rewritten plan's first step.
std::string ReplanSpeech(std::string_view goal);

// An action, before it runs, when there is no plan to speak: from the
// browser's description of it (WebAgent::Step: the wire name, the target
// as DescribeElement() or the URL / query writes it, the typed text or
// option). Empty for what changes nothing on the page (OBSERVE, REPLAN)
// and for the run's end (DONE, ERROR), which ResultSpeech() says.
//   NAVIGATE "https://x.test/a"   -> "Opening x.test."
//   SEARCH "trains to Lyon"       -> "Searching the web for trains to Lyon."
//   CLICK `button "Search"`       -> "Pressing Search."
//   TYPE `input "To" (text field)`, "Lyon" -> "Typing Lyon in To."
//   SELECT `select "Class" (...)`, "First" -> "Choosing First in Class."
//   SCROLL "down"                 -> "Scrolling down."
//   FILL_FORM                     -> "Filling in the form."
//   CALL_TOOL `tool "search"`     -> "Using the page's search tool."
std::string ActionSpeech(std::string_view action,
                         std::string_view target,
                         std::string_view value);

// What the agent asks about (WebAgent::ConfirmRequest::message: "Open
// https://evil.example", "Click \"Pay\" — it submits a form on
// https://shop.example"), as the voice loop's question puts it after "Say
// yes to ": "open evil.example", "click Pay, it submits a form on
// shop.example".
std::string QuestionWhat(std::string_view message);

// The run's end: "Done. <result>" or "I stopped: <result> Say what can I
// say to hear the commands I know."
std::string ResultSpeech(bool success, std::string_view result);

// Why the planner can't run, said plainly with what the user can do
// instead.
enum class NoPlanner {
  kNoModel,      // no on-device model is installed
  kUnsupported,  // this computer can't run on-device AI (|detail| says why)
  kLoadFailed,   // the model couldn't be loaded (|detail|: the reason)
  kNoPage,       // no tab to work in
};
std::string NoPlannerSpeech(NoPlanner reason, std::string_view detail);

// ---- The web agent's own questions (WebAgent::ConfirmRequest::spoken) ----
// What voice mode asks before an action of the web agent's, after "Say yes
// to ", built where the agent composes its question (web_agent.cc): each a
// whole sentence of the catalog in English (voice/spoken_answers.h), which
// the voice loop says in its language (VoiceController::PlannerAsks(),
// voice::Relocalized()), "abrir el sitio evil.example" in Spanish. In
// English each is QuestionWhat() of the agent's message, but that a site
// opened is "the site" and a click with no label says none (never the
// model's selector). Origins are said without their scheme; a page's label
// is one phrase (its punctuation said as spaces).
//   OpenQuestion("https://evil.example")    -> "open the site evil.example"
//   ContinueQuestion("http://x.test")            -> "keep working on x.test
//                                                    (the page moved there)"
//   ClickQuestion("Pay now", {"submits a form on https://shop.example"})
//       -> "click the one labelled Pay now, it submits a form on
//           shop.example"
//   FieldQuestion(false, "password", "https://x.test")
//       -> "type into a password field on x.test"
// A click's effects are agent_page_map::CheckClick()'s ("opens <origin>",
// "submits a form on|to <origin>"); ClickQuestion() returns "" for any
// other, and the caller asks QuestionWhat() of the message instead.
std::string OpenQuestion(std::string_view origin);
std::string ContinueQuestion(std::string_view origin);
std::string ClickQuestion(std::string_view label,
                          const std::vector<std::string>& effects);
// A sensitive field typed into (|fill| false) or filled in a form fill
// (|fill| true): |field| is agent_safety::FieldKindLabel()'s ("password",
// "payment card", "one-time code", "form"); "" for another.
std::string FieldQuestion(bool fill,
                          std::string_view field,
                          std::string_view origin);
// A site's tool the page offers, called (its name is page text): of the
// page, or of another site's frame on |origin|.
std::string ToolQuestion(std::string_view name,
                         bool other_site,
                         std::string_view origin);

}  // namespace voice_agent
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AGENTS_VOICE_AGENT_SPEECH_H_
