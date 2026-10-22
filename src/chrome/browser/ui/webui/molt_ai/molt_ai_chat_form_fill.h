// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// /fill ai and /paste in the AI chat (docs/design/AGENT_ACTIONS.md §4.7):
// form fills proposed and checked like the chat's page actions
// (molt_ai/agents/chat_actions.h), with the user's data instead of a
// request.
//
// The browser lists the page's elements with the agent's snapshot
// (agent_actions::BuildElementSnapshotScript), asks the on-device model,
// under a grammar, for one FILL_FORM of observed text fields (never a
// password field: the snapshot never marks one as a text field) or DONE,
// with the user's saved profile (/fill ai) or the text they pasted (/paste)
// as DATA, reads the reply with agent_actions::ParseAction, and describes
// it in its own words. Nothing is filled before the user's Allow, in Ask
// first and Auto mode alike (RequireConfirmation). The proposal then runs
// through the chat's proposal machinery (molt_ai_chat_handler_actions.cc):
// the element probe, chat_actions::CheckProposal, the pinned document and
// the chat's window, and agent_actions' fill script.
//
// Before, the page streamed the model's free text, cut it from the first
// '{' to the last '}', and sent the model's selector-to-value map back to
// be written.
//
// Pure logic (no content/), unit-tested in molt_ai_unittests.

#ifndef CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_CHAT_FORM_FILL_H_
#define CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_CHAT_FORM_FILL_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/agents/agent_actions.h"
#include "chrome/browser/molt_ai/agents/chat_actions.h"

namespace base {
class DictValue;
}  // namespace base

namespace url {
class Origin;
}  // namespace url

namespace molt_ai {
namespace chat_form_fill {

// A form fill writes text fields or does nothing.
inline constexpr agent_actions::ActionSet kFormFillActions =
    agent_actions::ActionBit(agent_actions::ActionType::kFillForm) |
    agent_actions::ActionBit(agent_actions::ActionType::kDone);

// Where the values come from.
enum class Source {
  kProfile,  // /fill ai: the saved profile (molt_ai/profile)
  kPaste,    // /paste: the text on the clipboard
};

// "profile" / "paste" (what the page sends); anything else is nullopt.
std::optional<Source> SourceFromName(std::string_view name);

// Caps. DATA and the user's words are counted in characters.
inline constexpr size_t kMaxDataChars = 4000;
inline constexpr size_t kMaxHintChars = 300;
// The pasted text the page may send; more is refused, not cut.
inline constexpr size_t kMaxPastedBytes = 64 * 1024;
// Profile entries: at most this many, keys of at most kMaxProfileKeyChars
// ASCII letters, digits and '_', values cut to kMaxProfileValueChars.
inline constexpr size_t kMaxProfileEntries = 40;
inline constexpr size_t kMaxProfileKeyChars = 40;
inline constexpr size_t kMaxProfileValueChars = 300;

// The saved profile (a flat dictionary of strings) as DATA: one
// "key: value" line per entry with a non-blank string value, in key order.
// Values are put on one line (control characters become spaces); other
// keys and values are left out. "" when nothing is left.
std::string FormatProfile(const base::DictValue& profile);

// The pasted text as DATA: '\r' dropped, other control characters except
// '\n' and '\t' made spaces, cut to kMaxDataChars characters. "" when
// blank.
std::string FormatPasted(std::string_view pasted);

// The decision request. |data| is FormatProfile() or FormatPasted(); |hint|
// the user's words after the command ("" if none). The page's URL, title
// and elements, and DATA, go inside the untrusted-content fence; the hint
// is cut to kMaxHintChars and sanitized like page text. |retry_note| is
// InterpretFormFill()'s message after an unusable reply, else "".
chat_actions::DecisionPrompt BuildFormFillPrompt(
    Source source,
    std::string_view data,
    std::string_view hint,
    std::string_view page_url,
    std::string_view page_title,
    const agent_actions::Observation& observation,
    std::string_view retry_note);

// agent_actions::BuildActionSchema(observation, kFormFillActions): one
// FILL_FORM of 1 to 8 of the observed text fields, or DONE.
std::string BuildFormFillSchema(const agent_actions::Observation& observation);

// agent_actions::InterpretReply() for kFormFillActions (a reply is parsed
// with ParseAction against |observation|), with the form filler's own
// words when it stops. |bad_replies| counts the unusable replies so far;
// the third one stops.
agent_actions::ReplyOutcome InterpretFormFill(
    const agent_actions::ModelReply& reply,
    const agent_actions::Observation& observation,
    int bad_replies);

// The browser's description of a FILL_FORM, which the user approves:
// every field and its whole value, e.g.
//   Fill 2 fields: input[email] "Email" = "ada@example.com";
//   textarea "Address" = "1 Main St"
// Values and labels on one line (line breaks and tabs shown as spaces);
// characters that reorder or hide text (bidi controls, zero-width
// characters) shown as U+FFFD, so what the user reads is what is typed.
// "" for anything but a FILL_FORM.
std::string DescribeFormFill(const agent_actions::AgentAction& action,
                             const agent_actions::Observation& observation);

// The reason every form fill asks first, e.g. "Fills 2 fields on
// https://example.com with your saved profile".
std::string ConfirmationReason(Source source,
                               const agent_actions::AgentAction& action,
                               const url::Origin& page_origin);

// A form fill never runs without the user's Allow: a kAct check becomes
// kAsk with |reason|. kAsk (a payment-card or one-time-code field) and
// kRefuse (a password field, a field gone) stay as they are.
chat_actions::Check RequireConfirmation(chat_actions::Check check,
                                        std::string_view reason);

// What the chat says when the model found nothing to fill (DONE). Fixed
// text: the model's own words are not shown.
std::string_view NothingToFillMessage(Source source);

// The fields a form fill wrote on this document (|filled_selectors|) are
// left out of the next observation of the same document, so running the
// command again continues with the other fields (a proposal fills at most
// agent_actions::kMaxFormFields). The other elements keep their ids.
void LeaveOutFilled(agent_actions::Observation* observation,
                    const std::vector<std::string>& filled_selectors);

// After a fill that used a whole proposal's fields while other text fields
// were observed: how to fill the rest ("" otherwise).
std::string MoreFieldsNote(Source source,
                           const agent_actions::AgentAction& action,
                           const agent_actions::Observation& observation);

}  // namespace chat_form_fill
}  // namespace molt_ai

#endif  // CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_CHAT_FORM_FILL_H_
