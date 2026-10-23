// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The AI chat's page actions (docs/design/AGENT_ACTIONS.md §4).
//
// The chat streams a free-text answer as before. When the user's typed
// message asks to act on the page (LooksLikePageActionRequest), the
// browser then asks the on-device model, in a separate request, for at
// most one action in the web agent's format (agent_actions.h) restricted
// to kChatActions: the model writes one JSON object under a grammar and
// picks an element id from an observation of the page, never a selector.
// The browser parses the reply again (agent_actions::ParseAction),
// describes the action in its own words (DescribeProposal), probes the
// elements it touches and decides whether it runs, needs the user's OK or
// is refused (CheckProposal). The chat window, MoltNet's boundary and the
// document the action was proposed for are checked by the handler
// (molt_ai_chat_handler_actions.cc).
//
// Nothing in a model reply is acted on any other way. The old
// "[[ACTION verb:args]]" text the chat page used to scan replies for is
// gone; such text, from a page, a model or an old saved conversation, is
// shown as text.
//
// When the page offers site tools (WebMCP; agent_tools.h, the main
// document's and its same-origin frames'), the set gains CALL_TOOL
// (kChatToolActions): the tools offered, their schemas and the arguments'
// checks are the web agent's (AddChatSiteTools(), agent_actions' schema and
// parser). A tool proposal always asks, read-only or not, and runs only on
// the user's click (MayRunProposal()).
//
// Pure logic (no content/), unit-tested in molt_ai_unittests.

#ifndef CHROME_BROWSER_MOLT_AI_AGENTS_CHAT_ACTIONS_H_
#define CHROME_BROWSER_MOLT_AI_AGENTS_CHAT_ACTIONS_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/agents/agent_actions.h"
#include "chrome/browser/molt_ai/security/agent_safety.h"
#include "url/origin.h"

namespace base {
class Value;
}  // namespace base

namespace molt_ai {
namespace chat_actions {

// What one chat proposal may do: a single step on the page in front of the
// user. No OBSERVE (there is no next step) and no ERROR: DONE means "no
// page action" (a question, nothing on the page fits, or not possible).
inline constexpr agent_actions::ActionSet kChatActions =
    agent_actions::ActionBit(agent_actions::ActionType::kNavigate) |
    agent_actions::ActionBit(agent_actions::ActionType::kSearch) |
    agent_actions::ActionBit(agent_actions::ActionType::kClick) |
    agent_actions::ActionBit(agent_actions::ActionType::kType) |
    agent_actions::ActionBit(agent_actions::ActionType::kFillForm) |
    agent_actions::ActionBit(agent_actions::ActionType::kScroll) |
    agent_actions::ActionBit(agent_actions::ActionType::kDone);
// ... and on a page that offers site tools: CALL_TOOL too.
inline constexpr agent_actions::ActionSet kChatToolActions =
    kChatActions | agent_actions::ActionBit(agent_actions::ActionType::kCallTool);

// The chat's set for |observation|: kChatToolActions when it has tools,
// else kChatActions (so without tools nothing differs).
agent_actions::ActionSet ChatActionsFor(
    const agent_actions::Observation& observation);

// Adds the page's tools to |observation| as the web agent does
// (agent_actions::AddSiteToolsFor()), each kept only if the chat's schema
// with it converts.
void AddChatSiteTools(std::vector<agent_tools::SiteTool> tools,
                  agent_actions::Observation* observation);

// A message of more words than this is not taken as a page-action request.
inline constexpr size_t kMaxRequestWords = 40;
// The request is quoted in the decision prompt up to this many characters.
inline constexpr size_t kMaxRequestChars = 1000;

// ---------------------------------------------------------------------------
// When to ask
// ---------------------------------------------------------------------------

// Whether a message the user typed reads like a request to act on the page
// in front of them: at most kMaxRequestWords words, starting (after
// politeness such as "please", "can you", "go ahead and") with one of a
// fixed set of verbs: open, click, tap, press, type, enter, fill, select,
// choose, scroll, search, submit, follow, visit, navigate, "go to". A
// question that merely contains such a word ("what type of ...") is not.
// Each request costs an on-device inference, so the test is deliberately
// narrow; the model can still answer DONE.
bool LooksLikePageActionRequest(std::string_view message);

// ---------------------------------------------------------------------------
// The decision request
// ---------------------------------------------------------------------------

// The parts of a molt_ai::ChatPrompt (browser_ai_runtime.h): the runtime
// sends system_prompt + context as the system message, never trims the
// system prompt and treats context and user_message as untrusted.
struct DecisionPrompt {
  std::string system_prompt;  // the format, the rules, the security rules
  std::string context;        // the page, fenced as untrusted
  std::string user_message;   // REQUEST: ..., and the retry note if any
};

// |request| is the user's message (cut to kMaxRequestChars characters,
// sanitized like page text so it can't forge the fence). |page_url|,
// |page_title|, the element labels and the TOOLS list (when the
// observation has tools: then the rules gain CALL_TOOL) are
// page-controlled and go inside the untrusted-content fence.
// |retry_note| is InterpretDecision()'s message after an unusable reply,
// else "".
DecisionPrompt BuildDecisionPrompt(
    std::string_view request,
    std::string_view page_url,
    std::string_view page_title,
    const agent_actions::Observation& observation,
    std::string_view retry_note);

// agent_actions::BuildActionSchema(observation, ChatActionsFor(observation)).
std::string BuildDecisionSchema(const agent_actions::Observation& observation);

// agent_actions::InterpretReply() for ChatActionsFor(), with the chat's own
// messages when it stops. |bad_replies| counts the unusable replies so far
// for this request; the third one stops (kMaxAttemptsPerStep). kAct with
// a DONE action means: no page action.
agent_actions::ReplyOutcome InterpretDecision(
    const agent_actions::ModelReply& reply,
    const agent_actions::Observation& observation,
    int bad_replies);

// ---------------------------------------------------------------------------
// The proposal
// ---------------------------------------------------------------------------

// The browser's own one-line description of |action| for the user, e.g.
// `Click button "Log in"`, `Type "cats" into input[search] "Search"`,
// `Open https://example.com/`. Labels are page text (cleaned by the
// snapshot parser) and typed text is the model's; the page shows it as
// text. Long values are shortened with "…", except a site tool's
// arguments: `Run the site's tool "add_to_cart" in the frame
// https://shop.example/w on https://shop.example with id = "A1"; qty = 2`
// shows the tool, the frame when it isn't the main document, and every
// argument whole, in agent_tools' escaped form.
std::string DescribeProposal(const agent_actions::AgentAction& action,
                             const agent_actions::Observation& observation);

// The selectors of the elements |action| acts on, from |observation| (the
// browser's own snapshot): one for CLICK and TYPE, one per field for
// FILL_FORM, none otherwise. Probe them with
// agent_safety::BuildElementProbeScript() and pass the result to
// CheckProposal().
std::vector<std::string> TargetSelectors(
    const agent_actions::AgentAction& action,
    const agent_actions::Observation& observation);

// The URL a NAVIGATE or SEARCH loads ("" for other actions). Check it
// with agent_safety::IsAllowedNavigationURL() (CheckProposal does).
std::string NavigationURL(const agent_actions::AgentAction& action);

// Origins a proposal may reach without asking: the page's own, the
// built-in search engine's, and those of http(s) URLs the user wrote in
// the request (agent_safety::ExtractExplicitURLs, as the web agent does
// for its goal). Only http(s) origins are ever in it.
class Scope {
 public:
  Scope();
  ~Scope();
  Scope(const Scope&);
  Scope& operator=(const Scope&);
  Scope(Scope&&);
  Scope& operator=(Scope&&);

  static Scope ForRequest(const url::Origin& page_origin,
                          std::string_view request);

  bool Contains(const url::Origin& origin) const;

 private:
  void Add(const url::Origin& origin);

  std::vector<url::Origin> origins_;
};

enum class Verdict {
  kAct,     // run it
  kAsk,     // run it only once the user has approved |message|
  kRefuse,  // never run it; |message| says why
};

struct Check {
  Verdict verdict = Verdict::kRefuse;
  // For kAsk the reason shown to the user ("Opens a different site:
  // https://x.example"); for kRefuse why not. Fixed text plus origins and
  // element names, never page script output.
  std::string message;
};

// Whether |action| may run on the page now. |probes| are
// agent_safety::ParseElementProbes() of TargetSelectors(), in order
// (empty for NAVIGATE, SEARCH, SCROLL, DONE). |page_origin| is the
// origin of the document it was proposed for.
//  - NAVIGATE / SEARCH: only http(s) (else refused); a site outside
//    |scope| asks.
//  - CLICK: a link to a site outside |scope| or a form submission asks.
//  - TYPE / FILL_FORM: a password field is refused, confirmed or not;
//    payment-card and one-time-code fields ask.
//  - CALL_TOOL: a tool the observation doesn't offer, or a page outside
//    |scope|, is refused; anything else asks, read-only or not: the choice
//    is model output written from page text. The reason names the tool,
//    the site, the frame when it isn't the main document, and what the
//    page says the tool does.
//  - A target that is gone, or probes that don't match the targets, is
//    refused. SCROLL and DONE act.
Check CheckProposal(const agent_actions::AgentAction& action,
                    const agent_actions::Observation& observation,
                    const Scope& scope,
                    const url::Origin& page_origin,
                    const std::vector<agent_safety::ElementProbe>& probes);

// Whether a proposal with |check| runs now. A kAsk check runs only when
// the user |confirmed| and the reason they approved (|approved_reason|,
// the last one shown for this proposal) is still the reason now: a page
// that changes what the action would do is asked about again.
bool MayRun(const Check& check, bool confirmed, std::string_view approved_reason);

// MayRun(), and a site tool only on the user's own click as well:
// |user_activation| is whether the chat page had transient user
// activation when it asked to run |action| (the Allow click), which the
// page's script can't fake.
bool MayRunProposal(const agent_actions::AgentAction& action,
                    const Check& check,
                    bool confirmed,
                    std::string_view approved_reason,
                    bool user_activation);

// What a CLICK / TYPE / FILL_FORM / SCROLL script (agent_actions::Build*
// Script) evaluated to, for the chat. Fixed text: an 'error:...' result
// carries page exception text and is not repeated.
struct ScriptOutcome {
  bool success = false;
  std::string message;
};
ScriptOutcome InterpretScriptResult(const agent_actions::AgentAction& action,
                                    const base::Value& result);

// The answer of a site tool's call for the chat: its text, marked as the
// site's, on one line with hidden characters shown and quotes escaped, cut
// at kMaxShownToolAnswerChars characters; "The tool loaded another page."
// when |loaded_another_page| (its document went away after it started);
// else agent_tools::CallFailureNote().
inline constexpr size_t kMaxShownToolAnswerChars = 300;
ScriptOutcome InterpretToolCall(const agent_tools::CallOutcome& outcome,
                                bool loaded_another_page);

}  // namespace chat_actions
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AGENTS_CHAT_ACTIONS_H_
