// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Shared safety rules for everything that drives a page on the user's
// behalf: the WebAgent ReAct loop, chat [[ACTION ...]] / slash-command
// actions, and automation replay (AutomationRunner). Keeping the rules in
// one place means a fix to, say, the navigation scheme allowlist lands for
// all three at once.
//
// Everything here is pure logic (no WebContents, no threads) so it is
// covered by agent_safety_unittest.cc in molt_ai_unittests.

#ifndef CHROME_BROWSER_MOLT_AI_SECURITY_AGENT_SAFETY_H_
#define CHROME_BROWSER_MOLT_AI_SECURITY_AGENT_SAFETY_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

class GURL;

namespace base {
class Value;
}  // namespace base

namespace molt_ai {
namespace agent_safety {

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

// True iff |url| is a valid http:// or https:// URL. Every navigation that an
// agent, a chat action or an automation replay starts must pass this, so
// javascript:, data:, file:, blob:, filesystem:, view-source:, chrome:,
// molt:, about: etc. are never reachable from model output or a .molt file.
bool IsAllowedNavigationURL(const GURL& url);

// The explicit http(s) URLs written in |text| (e.g. the user's agent goal),
// in order of appearance, with trailing sentence punctuation removed. This
// is a plain-text scan; callers still parse each with GURL and run it
// through IsAllowedNavigationURL.
std::vector<std::string> ExtractExplicitURLs(std::string_view text);

// ---------------------------------------------------------------------------
// Sensitive form fields
// ---------------------------------------------------------------------------

enum class FieldKind {
  kOrdinary,
  kPassword,     // <input type=password>, autocomplete current/new-password
  kPaymentCard,  // autocomplete cc-*
  kOneTimeCode,  // autocomplete one-time-code
};

// Classifies a form control from its (lower- or mixed-case) `type` property
// and `autocomplete` attribute. Either may be empty.
FieldKind ClassifyField(std::string_view input_type,
                        std::string_view autocomplete);

bool IsSensitive(FieldKind kind);

// Stable wire names used in .molt files and probe results: "" for
// kOrdinary, else "password" / "payment_card" / "one_time_code".
std::string_view FieldKindToString(FieldKind kind);
FieldKind FieldKindFromString(std::string_view name);

// Human-readable name for UI / step notes ("password", "payment card",
// "one-time code").
std::string_view FieldKindLabel(FieldKind kind);

// Heuristic for data recorded before field kinds were captured: the kind of
// field a CSS selector clearly names, e.g. `#password`, `[name="passwd"]`,
// `input[type="password"]` -> kPassword, `[autocomplete="cc-number"]`,
// `#cvv` -> kPaymentCard, `#otp` -> kOneTimeCode. Deliberately narrow (whole
// identifier tokens) so `#forgot-password-email` or `#passenger` do not
// match.
FieldKind ClassifySelector(std::string_view selector);
bool SelectorLooksSensitive(std::string_view selector);

// ---------------------------------------------------------------------------
// Element probe — what a click / type on a selector would actually touch.
// ---------------------------------------------------------------------------

struct ElementProbe {
  bool found = false;
  FieldKind field_kind = FieldKind::kOrdinary;
  // The element is (or sits inside) a submit control of a form.
  bool submits_form = false;
  // Absolute URL a click would load: the enclosing link's href, or the form
  // action for a submit control. Empty when the click does not navigate.
  // Raw page data — check it with IsAllowedNavigationURL before use.
  std::string navigates_to;
  // Short visible label (aria-label / text / value), for confirmation UI.
  std::string label;
};

// Builds a script (run it in an isolated world) that evaluates to a list
// with one {found, type, autocomplete, submit, nav, label} dict per selector
// in |selectors|, in order. Selectors are JSON-encoded, never spliced raw.
std::string BuildElementProbeScript(const std::vector<std::string>& selectors);

// Parses the result of BuildElementProbeScript. Always returns |expected|
// entries; anything missing or malformed reads as "not found".
std::vector<ElementProbe> ParseElementProbes(const base::Value& result,
                                             size_t expected);

// ---------------------------------------------------------------------------
// Untrusted page content inside LLM prompts
// ---------------------------------------------------------------------------

// Fence markers placed around page-derived text in agent prompts. The system
// prompt tells the model that anything between them is data, never
// instructions.
inline constexpr char kUntrustedBegin[] = "<<<UNTRUSTED_PAGE_CONTENT>>>";
inline constexpr char kUntrustedEnd[] = "<<<END_UNTRUSTED_PAGE_CONTENT>>>";

// Neutralises anything in |text| that could pass for prompt structure: runs
// of three or more '<' / '>' (so the fence markers can never be forged), and
// chat-template control tokens such as <|im_start|> / <|eot_id|>. Also drops
// ASCII control characters other than \n and \t.
std::string SanitizeUntrustedText(std::string_view text);

// kUntrustedBegin + "\n" + SanitizeUntrustedText(text) + "\n" + kUntrustedEnd.
std::string FenceUntrustedText(std::string_view text);

// ---------------------------------------------------------------------------
// RUN_JS
// ---------------------------------------------------------------------------

// Wraps an automation RUN_JS body so the run's variables reach it as data:
// |vars_json| (a JSON object produced by base::JSONWriter) is bound to a
// block-scoped `vars` constant ahead of the author's code, which follows
// verbatim. The block keeps the script's completion value (what the step
// stores), so expression-style and statement-style bodies both still work.
// Variables are never spliced into the code itself.
std::string BuildRunJsSource(std::string_view user_js,
                             std::string_view vars_json);

}  // namespace agent_safety
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_SECURITY_AGENT_SAFETY_H_
