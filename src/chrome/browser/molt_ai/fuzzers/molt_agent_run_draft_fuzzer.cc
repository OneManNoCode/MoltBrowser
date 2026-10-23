// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// A web agent run saved as a draft workflow (automation/agent_run_draft.h)
// on everything outside the browser: the goal (the user's words, which may
// hold secrets), what the model typed, the URLs and queries it chose, and
// the page's element labels and selectors (any bytes), in any order of
// steps that worked or failed.
//
// Checks:
//  * a run is refused exactly when it should be: not ended with DONE;
//    MoltNet or saved workflows off; a site tool that ran; and then always
//    with the refusal's fixed text and no script;
//  * a draft is CASUAL with an empty whitelist, its sites unconfirmed,
//    the default approvals, manual, no stats, no id; its effective policy
//    allows nothing; once ConfirmDraftSites() gets exactly its sites, it
//    may open its first page and nothing new;
//  * it opens a page first; every page it opens is http(s), without "{{"
//    and not UrlLooksSecret(); every string in it is valid UTF-8;
//  * descriptions, the name and text anchors are one clean line: no
//    control, bidi, zero-width or BOM character, no "{{", and descriptions
//    no ASCII quote (the runner would read it as an anchor);
//  * a typed value is kept only if neither it nor its field looks secret
//    and the goal didn't give it; otherwise it is a {{variable}} with an
//    empty default, or a secret step (never a password one) without value;
//    no value the run typed that looks secret is anywhere in the file;
//  * the draft saves and loads back the same, and building it twice gives
//    the same file;
//  * the pieces: CleanDisplayText() is clean, capped and idempotent,
//    VariableNameFor() plain, unique and capped, GoalSecretTokens() gives
//    pieces of the goal.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <fuzzer/FuzzedDataProvider.h>

#include "base/check.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/automation/agent_run_draft.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/automation/automation_security.h"
#include "chrome/browser/molt_ai/security/agent_safety.h"
#include "url/gurl.h"

namespace molt_ai::automation::agent_draft {
namespace {

size_t CountChars(std::string_view s) {
  size_t n = 0;
  for (char c : s) {
    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
  }
  return n;
}

// Strict UTF-8 (what base::Value requires of its strings).
bool ValidUtf8(std::string_view s) {
  for (size_t i = 0; i < s.size();) {
    const unsigned char b0 = static_cast<unsigned char>(s[i]);
    size_t len = b0 < 0x80 ? 1 : (b0 & 0xE0) == 0xC0 ? 2
                              : (b0 & 0xF0) == 0xE0 ? 3
                              : (b0 & 0xF8) == 0xF0 ? 4 : 0;
    if (!len || len > s.size() - i) return false;
    uint32_t cp = len == 1 ? b0 : len == 2 ? (b0 & 0x1F)
                                : len == 3 ? (b0 & 0x0F) : (b0 & 0x07);
    for (size_t k = 1; k < len; ++k) {
      const unsigned char b = static_cast<unsigned char>(s[i + k]);
      if ((b & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (b & 0x3F);
    }
    const uint32_t min = len == 1 ? 0 : len == 2 ? 0x80 : len == 3 ? 0x800
                                                                   : 0x10000;
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
      return false;
    i += len;
  }
  return true;
}

// One clean line: no C0 control or DEL, and none of the characters that
// reorder, hide or break text (agent_plan.cc's list, as UTF-8).
bool CleanLine(std::string_view s) {
  for (char c : s) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7F) return false;
  }
  for (std::string_view hidden :
       {"\xC2\x80", "\xC2\x85", "\xC2\x9F", "\xD8\x9C", "\xE1\xA0\x8E",
        "\xE2\x80\x8B", "\xE2\x80\x8C", "\xE2\x80\x8D", "\xE2\x80\x8E",
        "\xE2\x80\x8F", "\xE2\x80\xA8", "\xE2\x80\xA9", "\xE2\x80\xAA",
        "\xE2\x80\xAB", "\xE2\x80\xAC", "\xE2\x80\xAD", "\xE2\x80\xAE",
        "\xE2\x81\xA0", "\xE2\x81\xA4", "\xE2\x81\xA6", "\xE2\x81\xA9",
        "\xEF\xBB\xBF"}) {
    if (s.find(hidden) != std::string_view::npos) return false;
  }
  return ValidUtf8(s) && s.find("{{") == std::string_view::npos;
}

std::string PickUrl(FuzzedDataProvider& in) {
  static const char* const kUrls[] = {
      "https://shop.example/",
      "https://shop.example/search?q=socks&page=2",
      "http://www.example.com/a/b#frag",
      "https://search.brave.com/search?q=cheap+flights",
      "https://user:pw@shop.example/",
      "https://shop.example/cb?token=abc",
      "https://shop.example/reset-password/abcdefghij0123456789abcd",
      "https://shop.example/pay?n=4111111111111111",
      "javascript:alert(1)",
      "file:///etc/passwd",
      "chrome://settings",
      "https://{{host}}/",
      "",
  };
  if (in.ConsumeBool())
    return kUrls[in.ConsumeIntegralInRange<size_t>(0, std::size(kUrls) - 1)];
  return in.ConsumeRandomLengthString(300);
}

std::string PickText(FuzzedDataProvider& in) {
  static const char* const kTexts[] = {
      "blue socks", "Tr0ub4dor&3", "4111 1111 1111 1111", "123456",
      "123-45-6789", "hunter2", "SAVE20", "alice@example.com", "2026-12-01",
      "line\nbreak", "{{password}}", "sk-live-0123456789abcdef", "",
      "New York", "flights to Tokyo", "Kx9mQ2vLp7Rz"};
  if (in.ConsumeIntegralInRange<int>(0, 3) != 0)
    return kTexts[in.ConsumeIntegralInRange<size_t>(0, std::size(kTexts) - 1)];
  return in.ConsumeRandomLengthString(600);
}

std::string PickLabel(FuzzedDataProvider& in) {
  static const char* const kLabels[] = {
      "Search", "Password", "PIN", "Card number", "Card\nnumber",
      "Pass\xE2\x80\x8Bword", "Pay\xE2\x80\xAE" "evil", "Send {{x}}", ""};
  if (in.ConsumeBool())
    return kLabels[in.ConsumeIntegralInRange<size_t>(0, std::size(kLabels) - 1)];
  return in.ConsumeRandomLengthString(120);
}

// One step. Mostly what the agent reports (the action's own fields, one
// element for CLICK / TYPE, a few for FILL_FORM, a page of the run), so
// whole runs get through and the draft is checked; sometimes anything.
RunStep PickStep(FuzzedDataProvider& in) {
  static const char* const kActions[] = {
      "NAVIGATE", "SEARCH",  "CLICK", "TYPE",   "FILL_FORM", "SCROLL",
      "OBSERVE",  "DONE",    "CLICK", "TYPE",   "FILL_FORM", "ERROR",
      "REPLAN",   "CALL_TOOL", "HOVER"};
  static const char* const kPages[] = {
      "https://shop.example/", "https://shop.example/cart?step=2",
      "https://www.airline.example/book#seats", "http://news.example/a/b"};
  static const char* const kKinds[] = {"input",  "textarea", "button",
                                       "link",   "select",   "checkbox",
                                       "radio",  "editable"};
  const bool plausible = in.ConsumeIntegralInRange<int>(0, 7) != 0;
  RunStep s;
  s.action = kActions[in.ConsumeIntegralInRange<size_t>(
      0, std::size(kActions) - (plausible ? 4 : 1))];
  s.success = in.ConsumeIntegralInRange<int>(0, 7) != 0;
  if (plausible) {
    s.url = kPages[in.ConsumeIntegralInRange<size_t>(0, std::size(kPages) - 1)];
    s.query = in.ConsumeBool() ? "cheap flights" : PickText(in);
    if (s.action == "SEARCH")
      s.url = "https://search.brave.com/search?q=x";
    s.direction = in.ConsumeBool() ? "down" : "up";
    s.origin = in.ConsumeBool() ? "https://shop.example"
                                : "https://www.airline.example";
  } else {
    s.url = PickUrl(in);
    s.query = PickText(in);
    s.direction = in.ConsumeRandomLengthString(8);
    s.origin = PickUrl(in);
  }
  size_t n = 0;
  if (!plausible) {
    n = in.ConsumeIntegralInRange<size_t>(0, 9);
  } else if (s.action == "CLICK" || s.action == "TYPE") {
    n = 1;
  } else if (s.action == "FILL_FORM") {
    n = in.ConsumeIntegralInRange<size_t>(1, 4);
  }
  for (size_t i = 0; i < n; ++i) {
    RunElement e;
    e.selector = plausible || in.ConsumeBool()
                     ? "#f" + base::NumberToString(i)
                     : in.ConsumeRandomLengthString(80);
    e.kind = plausible ? kKinds[in.ConsumeIntegralInRange<size_t>(
                             0, std::size(kKinds) - 1)]
                       : in.ConsumeRandomLengthString(10);
    e.input_type =
        plausible ? (in.ConsumeBool() ? "text" : "") : in.ConsumeRandomLengthString(10);
    e.label = PickLabel(in);
    const int kind = in.ConsumeIntegralInRange<int>(0, plausible ? 7 : 3);
    e.field_kind = static_cast<agent_safety::FieldKind>(kind <= 3 ? kind : 0);
    e.text = PickText(in);
    s.elements.push_back(std::move(e));
  }
  return s;
}

void CheckPieces(FuzzedDataProvider& in) {
  const std::string text = in.ConsumeRandomLengthString(400);
  const size_t max = in.ConsumeIntegralInRange<size_t>(0, 80);
  const std::string clean = CleanDisplayText(text, max);
  CHECK(ValidUtf8(clean));
  CHECK(CountChars(clean) <= max);
  for (char c : clean) {
    const unsigned char u = static_cast<unsigned char>(c);
    CHECK(u >= 0x20 && u != 0x7F);
  }
  CHECK(clean.empty() || (clean.front() != ' ' && clean.back() != ' '));
  CHECK(clean.find("  ") == std::string::npos);
  CHECK(CleanDisplayText(clean, max) == clean) << "not idempotent";

  std::set<std::string> taken;
  for (int i = 0; i < 4; ++i) {
    const std::string name = VariableNameFor(text, taken);
    CHECK(!name.empty() && name.size() <= kMaxVariableNameChars);
    CHECK(name[0] >= 'a' && name[0] <= 'z');
    for (char c : name) {
      CHECK((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_');
    }
    CHECK(!taken.count(name));
    taken.insert(name);
  }
  for (const std::string& t : GoalSecretTokens(text)) {
    CHECK(t.size() >= 3);
    CHECK(text.find(t) != std::string::npos);
  }
  UrlLooksSecret(text);
  TextLooksSecret(text);
  LabelSuggestsSecret(text);
}

void CheckDraft(const Run& run, bool allowed, const Draft& d) {
  // When to refuse, and how.
  bool tool = false;
  for (const RunStep& s : run.steps) tool |= s.success && s.action == "CALL_TOOL";
  if (!run.succeeded) {
    CHECK(d.refusal == Refusal::kNotSucceeded);
  } else if (run.in_moltnet || !allowed) {
    CHECK(d.refusal == Refusal::kMoltNet);
  } else if (run.steps.size() <= kMaxDraftSteps && tool) {
    CHECK(d.refusal == Refusal::kUsedSiteTool);
  }
  if (d.refusal != Refusal::kNone) {
    CHECK(!d.script);
    CHECK(d.message == RefusalText(d.refusal));
    return;
  }
  CHECK(d.script);
  CHECK(run.succeeded && !run.in_moltnet && allowed && !tool);
  const Script& s = *d.script;

  // An unconfirmed, untrusted, manual draft.
  CHECK(s.id.empty());
  CHECK(s.security.trust == TrustLevel::CASUAL);
  CHECK(s.security.sites_unconfirmed);
  CHECK(s.security.domain_whitelist.empty());
  CHECK(s.security.require_approval_for ==
        (std::vector<std::string>{"form_submit", "payment", "login"}));
  CHECK(s.trigger.type == TriggerType::MANUAL);
  CHECK(s.stats.runs == 0 && s.stats.run_history.empty());
  CHECK(!s.steps.empty() && s.steps.size() <= kMaxDraftSteps);
  CHECK(s.steps.front().type == StepType::NAVIGATE);
  CHECK(CleanLine(s.name) && !s.name.empty());

  AutomationSecurity security;
  const SecurityPolicy unconfirmed = AutomationSecurity::EffectivePolicy(s);
  CHECK(unconfirmed.trust == TrustLevel::CASUAL);
  CHECK(unconfirmed.domain_whitelist.empty());

  // The goal's secrets and the values that look secret, as the run typed
  // them: none may be stored as a value.
  const std::vector<std::string> goal_secrets = GoalSecretTokens(run.goal);
  std::vector<std::string> secret_texts;
  for (const RunStep& rs : run.steps) {
    for (const RunElement& e : rs.elements) {
      if (!e.text.empty() && TextLooksSecret(e.text))
        secret_texts.push_back(e.text);
    }
  }

  int variables = 0;
  int secret_steps = 0;
  for (const Step& step : s.steps) {
    CHECK(CleanLine(step.description)) << step.description;
    CHECK(step.description.find('"') == std::string::npos);
    CHECK(ValidUtf8(step.target) && ValidUtf8(step.value));
    if (step.extra.is_dict()) {
      for (const auto kv : step.extra.GetDict()) {
        CHECK(ValidUtf8(kv.first));
        if (kv.second.is_string()) CHECK(ValidUtf8(kv.second.GetString()));
      }
      if (const std::string* t = step.extra.GetDict().FindString("text"))
        CHECK(CleanLine(*t) && !t->empty() && CountChars(*t) <= kMaxLabelChars);
      if (const std::string* h = step.extra.GetDict().FindString("host"))
        CHECK(!h->empty() && CleanLine(*h));
    }
    switch (step.type) {
      case StepType::NAVIGATE: {
        const GURL g(step.target);
        CHECK(g.is_valid() && agent_safety::IsAllowedNavigationURL(g));
        CHECK(step.target.find("{{") == std::string::npos);
        CHECK(!UrlLooksSecret(step.target));
        CHECK(!security.AllowNavigation(unconfirmed, step.target));
        break;
      }
      case StepType::CLICK:
        CHECK(!step.target.empty());
        break;
      case StepType::SCROLL:
        CHECK(step.value == "down" || step.value == "up");
        break;
      case StepType::TYPE: {
        CHECK(step.target.find("{{") == std::string::npos);
        const agent_safety::FieldKind kind =
            AutomationSecurity::SecretStepKind(step);
        if (kind != agent_safety::FieldKind::kOrdinary) {
          CHECK(kind != agent_safety::FieldKind::kPassword);
          CHECK(step.value.empty());
          ++secret_steps;
          break;
        }
        if (step.value.size() >= 4 && step.value.rfind("{{", 0) == 0) {
          const std::string name = step.value.substr(2, step.value.size() - 4);
          CHECK(step.value == "{{" + name + "}}");
          CHECK(s.default_variables.count(name) == 1);
          CHECK(s.default_variables.at(name).empty());
          ++variables;
          break;
        }
        CHECK(step.value.find("{{") == std::string::npos);
        CHECK(!TextLooksSecret(step.value)) << "a secret-looking value was kept";
        for (const std::string& t : secret_texts)
          CHECK(step.value != t) << "a secret the run typed was kept";
        for (const std::string& t : goal_secrets)
          CHECK(step.value.find(t) == std::string::npos) << "a goal secret was kept";
        if (step.extra.is_dict()) {
          if (const std::string* t = step.extra.GetDict().FindString("text"))
            CHECK(!LabelSuggestsSecret(*t)) << "a secret field's value was kept";
        }
        break;
      }
      default:
        CHECK(false) << "unexpected step type";
    }
  }
  CHECK(variables == d.withheld);
  CHECK(secret_steps == d.secret_steps);
  CHECK(static_cast<size_t>(variables) == s.default_variables.size());

  // It saves (with the id the chat gives it) and loads back the same.
  base::DictValue json = s.ToJSON();
  json.Set("id", "agent_1");
  std::optional<Script> copy = Script::FromJSON(json);
  CHECK(copy);
  const std::string saved = copy->ToJSONString();
  std::optional<Script> loaded = Script::FromJSONString(saved);
  CHECK(loaded && loaded->ToJSONString() == saved) << "save/load changed it";
  CHECK(loaded->security.sites_unconfirmed);

  // Confirming exactly its sites makes it an ordinary CASUAL workflow on
  // them; anything else confirms nothing.
  const std::vector<std::string> sites =
      AutomationSecurity::HostsReferencedBy(copy->steps);
  CHECK(!sites.empty());
  std::vector<std::string> wrong = sites;
  wrong.push_back("evil.example.invalid");
  CHECK(!AutomationSecurity::ConfirmDraftSites(*copy, wrong));
  CHECK(!AutomationSecurity::ConfirmDraftSites(*copy, {}));
  CHECK(copy->security.sites_unconfirmed);
  CHECK(AutomationSecurity::ConfirmDraftSites(*copy, sites));
  CHECK(!copy->security.sites_unconfirmed);
  CHECK(copy->security.trust == TrustLevel::CASUAL);
  CHECK(copy->security.domain_whitelist == sites);
  const SecurityPolicy confirmed = AutomationSecurity::EffectivePolicy(*copy);
  CHECK(confirmed.domain_whitelist == sites);
  CHECK(security.AllowNavigation(confirmed, copy->steps.front().target));
  CHECK(!security.AllowSubmit(confirmed));
  CHECK(!AutomationSecurity::ConfirmDraftSites(*copy, sites)) << "twice";
}

}  // namespace
}  // namespace molt_ai::automation::agent_draft

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace molt_ai::automation::agent_draft;
  FuzzedDataProvider in(data, size);
  CheckPieces(in);

  Run run;
  switch (in.ConsumeIntegralInRange<int>(0, 3)) {
    case 0:
      run.goal = "my password is hunter2, code SAVE20";
      break;
    case 1:
      run.goal = "Buy blue socks";
      break;
    default:
      run.goal = in.ConsumeRandomLengthString(300);
  }
  run.start_url = in.ConsumeBool() ? "https://shop.example/home" : PickUrl(in);
  run.succeeded = in.ConsumeIntegralInRange<int>(0, 7) != 0;
  run.in_moltnet = in.ConsumeIntegralInRange<int>(0, 15) == 0;
  const bool allowed = in.ConsumeIntegralInRange<int>(0, 15) != 0;
  const size_t steps = in.ConsumeIntegralInRange<size_t>(0, 12);
  for (size_t i = 0; i < steps && in.remaining_bytes() > 0; ++i)
    run.steps.push_back(PickStep(in));

  const Draft draft = BuildDraft(run, allowed, 1790000000);
  CheckDraft(run, allowed, draft);

  // Deterministic.
  const Draft again = BuildDraft(run, allowed, 1790000000);
  CHECK(again.refusal == draft.refusal && again.message == draft.message);
  if (draft.script)
    CHECK(again.script->ToJSONString() == draft.script->ToJSONString());
  return 0;
}
