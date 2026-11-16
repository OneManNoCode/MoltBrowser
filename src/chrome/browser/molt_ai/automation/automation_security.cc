// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/automation_security.h"

#include <algorithm>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "chrome/browser/molt_ai/agents/hidden_chars.h"
#include "chrome/browser/molt_ai/automation/automation_voice_steps.h"
#include "crypto/hash.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "url/gurl.h"

namespace molt_ai {
namespace automation {

namespace {

// Heuristics for sensitive form fields by selector or value content.
bool LooksLikePasswordSelector(const std::string& selector) {
  std::string lower = base::ToLowerASCII(selector);
  return lower.find("password") != std::string::npos ||
         lower.find("type=\"password\"") != std::string::npos ||
         lower.find("type='password'") != std::string::npos;
}

bool LooksLikePaymentCardValue(const std::string& value) {
  // Strip spaces, then check for 13–19 digits (covers most card formats).
  std::string digits;
  for (char c : value) {
    if (c >= '0' && c <= '9')
      digits.push_back(c);
  }
  if (digits.size() < 13 || digits.size() > 19) return false;
  // Cheap Luhn check.
  int sum = 0;
  bool alt = false;
  for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
    int n = *it - '0';
    if (alt) {
      n *= 2;
      if (n > 9) n -= 9;
    }
    sum += n;
    alt = !alt;
  }
  return (sum % 10) == 0;
}

// The approval categories a CASUAL workflow always asks for (the only ones
// a check reads: AllowSubmit(), AllowSensitiveField(); APPROVED lifts them
// there, TRUSTED needs none). A stored list adds to them, never removes one
// (EffectivePolicy()).
constexpr const char* kDefaultApprovals[] = {"form_submit", "payment",
                                             "login"};

// Whether |target| is an address a value gives: one leading {{name}}, and
// after it nothing, or only a path, query or fragment ('/', '?' or '#', never
// "//"). A scheme, a host or another value right after it could be a fixed
// site once the value is empty.
bool AddressIsAValue(std::string_view target) {
  if (!target.starts_with("{{")) {
    return false;
  }
  const size_t close = target.find("}}");
  if (close == std::string_view::npos ||
      target.substr(2, close - 2).find('{') != std::string_view::npos) {
    return false;
  }
  const std::string_view rest = target.substr(close + 2);
  if (rest.empty()) {
    return true;
  }
  return !rest.starts_with("//") &&
         (rest.front() == '/' || rest.front() == '?' || rest.front() == '#');
}

// |value| when it is positive and at most |limit|, else |limit|.
template <typename T>
T WithinLimit(T value, T limit) {
  return value > 0 && value <= limit ? value : limit;
}

bool Contains(const std::vector<std::string>& list, std::string_view item) {
  return std::find(list.begin(), list.end(), item) != list.end();
}

void AddUnique(std::vector<std::string>& list, std::string item) {
  if (!item.empty() && !Contains(list, item))
    list.push_back(std::move(item));
}

// The old recorder masked the description of a type=password step with
// this (while still storing the real value).
constexpr char kLegacyPasswordMask[] = "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2";  // "•••"

// What a scrubbed step / run-history entry is described as.
std::string SecretStepDescription(agent_safety::FieldKind kind) {
  return kind == agent_safety::FieldKind::kPassword
             ? "Fill your saved password"
             : "Enter the " + std::string(agent_safety::FieldKindLabel(kind));
}

// ---- What the Agent studio's review shows (ClampUnvouched()) ----

// Which fields of a step of each type the studio's review shows, as the
// runner uses them (molt_ai_agent_ui.cc's buildEditorStep()); nullopt for
// a type it doesn't show in full.
struct ShownFields {
  bool target = false;
  bool value = false;
  bool store_as = false;
};

std::optional<ShownFields> ShownFieldsOf(StepType type) {
  switch (type) {
    case StepType::NAVIGATE:   // "Web address"
    case StepType::CLICK:      // "What to click"
    case StepType::WAIT_FOR:   // "Wait until this appears"
    case StepType::CALL_TOOL:  // the tool's name (its title and its box)
      return ShownFields{.target = true};
    case StepType::TYPE:    // "Text to type", "On element"
    case StepType::ASSERT:  // "Must be true", "On element"
      return ShownFields{.target = true, .value = true};
    case StepType::SCROLL:    // "How far"
    case StepType::KEYPRESS:  // "Key"
    case StepType::WAIT:      // "Seconds to wait" (its timeout isn't kept)
    case StepType::NOTIFY:    // "Message"
      return ShownFields{.value = true};
    case StepType::EXTRACT:  // "What to grab", "Save answer as"
      return ShownFields{.target = true, .store_as = true};
    // Control flow (which steps run, how often), a page snapshot, code, the
    // on-device model's prompt and schema, another workflow: not shown in
    // full.
    case StepType::IF:
    case StepType::ELSE:
    case StepType::END_IF:
    case StepType::LOOP:
    case StepType::END_LOOP:
    case StepType::SCREENSHOT:
    case StepType::RUN_JS:
    case StepType::RUN_SCRIPT:
    case StepType::AI_DECIDE:
    case StepType::AI_EXTRACT:
    case StepType::UNKNOWN:
      return std::nullopt;
  }
  return std::nullopt;
}

// Whether |text|, shown in a one-line field, would read otherwise than it
// runs: a control character (an <input> drops a line break, a tab looks
// like spaces), DEL, or a character that reorders, hides or breaks text
// (agents/hidden_chars.h).
bool HasUnshownChars(std::string_view text) {
  for (size_t i = 0; i < text.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c < 0x20 || c == 0x7F || hidden_chars::HiddenCharAt(text, i) > 0) {
      return true;
    }
  }
  return false;
}

// Whether |text| holds a control character (a line break, a tab) or DEL.
bool HasControlChars(std::string_view text) {
  return std::any_of(text.begin(), text.end(), [](char ch) {
    const unsigned char c = static_cast<unsigned char>(ch);
    return c < 0x20 || c == 0x7F;
  });
}

// Whether a site tool's values (extra["tool"]["args"]) would read otherwise
// than they run: a name, or a string given (in a list too), that holds a
// control character. The review shows each value on its own, but a line
// break or a tab lays it out as lines and space, which could push the
// values after it out of view; one that would is dropped, as a one-line
// field's is (HasUnshownChars()).
bool ToolArgsHaveControlChars(const base::DictValue& args) {
  const auto has = [](const base::Value& value) {
    return value.is_string() && HasControlChars(value.GetString());
  };
  for (const auto [name, value] : args) {
    if (HasControlChars(name) || has(value)) {
      return true;
    }
    if (value.is_list() &&
        std::any_of(value.GetList().begin(), value.GetList().end(), has)) {
      return true;
    }
  }
  return false;
}

// A page host as the browser records one (kStepHostKey): printable ASCII,
// so the site the review lists is the site a run is allowed on.
bool IsShownHost(std::string_view host) {
  constexpr size_t kMaxHostBytes = 253;
  if (host.empty() || host.size() > kMaxHostBytes) {
    return false;
  }
  return std::all_of(host.begin(), host.end(),
                     [](char c) { return c > 0x20 && c < 0x7F; });
}

// A site tool's step's extra key (automation_tool_steps.h's kToolKey).
constexpr char kToolStepKey[] = "tool";

// A site tool's extra["tool"] with only what the review shows of it
// (automation_tool_steps.h's ParseToolStep() reads these keys): the page's
// origin, the frame's origin and path, the values given and the names of
// those not saved, and that it is a form tool.
base::DictValue ShownTool(const base::DictValue& tool) {
  base::DictValue out;
  if (const std::string* origin = tool.FindString("origin")) {
    out.Set("origin", *origin);
  }
  if (const base::DictValue* frame = tool.FindDict("frame")) {
    base::DictValue shown;
    if (const std::string* origin = frame->FindString("origin")) {
      shown.Set("origin", *origin);
    }
    if (const std::string* path = frame->FindString("path")) {
      shown.Set("path", *path);
    }
    out.Set("frame", std::move(shown));
  }
  if (const base::DictValue* args = tool.FindDict("args")) {
    out.Set("args", args->Clone());
  }
  if (const base::ListValue* secret = tool.FindList("secret_args")) {
    out.Set("secret_args", secret->Clone());
  }
  if (tool.FindBool("form").value_or(false)) {
    out.Set("form", true);
  }
  return out;
}

// |step| with only what the review shows of it, or nullopt when the review
// doesn't show it in full (ClampUnvouched()).
std::optional<Step> ShownStep(const Step& step) {
  const base::DictValue* extra = step.extra.GetIfDict();
  base::DictValue shown_extra;
  if (const std::string* host =
          extra ? extra->FindString(kStepHostKey) : nullptr;
      host && IsShownHost(*host)) {
    shown_extra.Set(kStepHostKey, *host);
  }
  Step out;
  out.type = step.type;
  // A routine taught by voice: what voice mode runs is its extra["voice"],
  // which a claim keeps only as the review describes it
  // (voice::DescribeRoutineStepsAsRun()); nothing else, its secret keys
  // included (voice mode never reads them).
  if (const base::DictValue* voice =
          extra ? extra->FindDict(kVoiceStepKey) : nullptr) {
    out.target = step.target;
    out.value = step.value;
    shown_extra.Set(kVoiceStepKey, voice->Clone());
    out.extra = base::Value(std::move(shown_extra));
    return out;
  }
  std::optional<ShownFields> shown = ShownFieldsOf(step.type);
  if (!shown) {
    return std::nullopt;
  }
  // A saved secret, as the runner reads it (its kind), which titles the
  // step; its value is never typed, nor kept.
  const bool secret =
      step.type == StepType::TYPE && extra && extra->FindString(kSecretKey);
  if (secret) {
    shown->value = false;
    shown_extra.Set(kSecretKey, kSecretVault);
    shown_extra.Set(kSecretKindKey,
                    agent_safety::FieldKindToString(
                        AutomationSecurity::SecretStepKind(step)));
  }
  if ((shown->target && HasUnshownChars(step.target)) ||
      (shown->value && HasUnshownChars(step.value)) ||
      (shown->store_as && HasUnshownChars(step.store_as))) {
    return std::nullopt;
  }
  if (shown->target) {
    out.target = step.target;
  }
  if (shown->value) {
    out.value = step.value;
  }
  if (shown->store_as) {
    out.store_as = step.store_as;
  }
  if (step.type == StepType::CALL_TOOL && extra) {
    if (const base::DictValue* tool = extra->FindDict(kToolStepKey)) {
      const base::DictValue* args = tool->FindDict("args");
      if (args && ToolArgsHaveControlChars(*args)) {
        return std::nullopt;
      }
      shown_extra.Set(kToolStepKey, ShownTool(*tool));
    }
  }
  if (!shown_extra.empty()) {
    out.extra = base::Value(std::move(shown_extra));
  }
  return out;
}

bool DomainMatchesWhitelist(const std::vector<std::string>& whitelist,
                             std::string_view host) {
  std::string h = base::ToLowerASCII(host);
  for (const auto& entry : whitelist) {
    std::string e = base::ToLowerASCII(entry);
    if (h == e) return true;
    // Suffix match: 'kayak.com' allows 'www.kayak.com', within one site
    // (registrable domain) only. Never from an entry that is itself a
    // registry (a public suffix such as "com", "co.uk" or "github.io", which
    // a file can list or name as a step's host), nor across one below it
    // ("amazonaws.com" doesn't allow a bucket under "s3.amazonaws.com"): it
    // would allow sites that are someone else's. An IP address or a single
    // label ("localhost") matches only itself.
    if (h.size() > e.size() &&
        h.compare(h.size() - e.size() - 1, e.size() + 1, "." + e) == 0) {
      const std::string site =
          net::registry_controlled_domains::GetDomainAndRegistry(
              e, net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);
      if (!site.empty() &&
          site == net::registry_controlled_domains::GetDomainAndRegistry(
                      h, net::registry_controlled_domains::
                             INCLUDE_PRIVATE_REGISTRIES)) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

AutomationSecurity::AutomationSecurity() = default;
AutomationSecurity::~AutomationSecurity() = default;

bool AutomationSecurity::AllowNavigation(const SecurityPolicy& policy,
                                          const std::string& url) const {
  if (policy.trust >= TrustLevel::TRUSTED)
    return true;
  GURL g(url);
  if (!g.is_valid())
    return false;
  return DomainMatchesWhitelist(policy.domain_whitelist, g.host());
}

bool AutomationSecurity::AllowClick(const SecurityPolicy& policy,
                                     const std::string& url,
                                     const std::string& selector) const {
  if (policy.trust >= TrustLevel::TRUSTED)
    return true;
  GURL g(url);
  if (!g.is_valid())
    return false;
  if (!DomainMatchesWhitelist(policy.domain_whitelist, g.host()))
    return false;
  // CASUAL trust forbids submit-shaped clicks unless explicitly approved.
  if (policy.trust == TrustLevel::CASUAL) {
    std::string lower = base::ToLowerASCII(selector);
    if (lower.find("submit") != std::string::npos)
      return false;
  }
  return true;
}

bool AutomationSecurity::AllowType(const SecurityPolicy& policy,
                                    const std::string& url,
                                    const std::string& selector,
                                    const std::string& value) const {
  if (policy.trust >= TrustLevel::TRUSTED)
    return true;
  GURL g(url);
  if (!g.is_valid())
    return false;
  if (!DomainMatchesWhitelist(policy.domain_whitelist, g.host()))
    return false;
  // Block password / card auto-fill below APPROVED.
  if (policy.trust < TrustLevel::APPROVED) {
    if (LooksLikePasswordSelector(selector))
      return false;
    if (LooksLikePaymentCardValue(value))
      return false;
  }
  return true;
}

bool AutomationSecurity::AllowHeadless(const SecurityPolicy& policy) const {
  return policy.trust >= TrustLevel::TRUSTED;
}

std::vector<std::string> AutomationSecurity::ApprovalRequired(
    const SecurityPolicy& policy) const {
  if (policy.trust >= TrustLevel::TRUSTED) return {};
  return policy.require_approval_for;
}

bool AutomationSecurity::AllowSubmit(const SecurityPolicy& policy) const {
  if (policy.trust >= TrustLevel::APPROVED)
    return true;
  return !Contains(ApprovalRequired(policy), "form_submit");
}

bool AutomationSecurity::AllowSensitiveField(
    const SecurityPolicy& policy,
    agent_safety::FieldKind kind) const {
  if (!agent_safety::IsSensitive(kind) || policy.trust >= TrustLevel::APPROVED)
    return true;
  const char* category =
      kind == agent_safety::FieldKind::kPaymentCard ? "payment" : "login";
  return !Contains(ApprovalRequired(policy), category);
}

// static
SecurityPolicy AutomationSecurity::EffectivePolicy(const Script& script) {
  SecurityPolicy p = script.security;
  if (p.sites_unconfirmed) {
    // A model-made draft nobody has reviewed: nothing widens what it may
    // do, whatever trust was set on it meanwhile.
    p.trust = TrustLevel::CASUAL;
  } else {
    for (std::string& host : HostsReferencedBy(script.steps))
      AddUnique(p.domain_whitelist, std::move(host));
  }
  // Never fewer approvals than the defaults: a file (an import from an
  // older build, a legacy or hand-edited one) can't opt out of one; only
  // trust lifts them (ApprovalRequired()). The defaults first, then any
  // other the file lists.
  std::vector<std::string> approvals(std::begin(kDefaultApprovals),
                                     std::end(kDefaultApprovals));
  for (std::string& approval : p.require_approval_for) {
    AddUnique(approvals, std::move(approval));
  }
  p.require_approval_for = std::move(approvals);
  // Below TRUSTED, a run is at most as long as the default allows, whatever
  // its file says (one imported before this build, or a legacy file); a
  // shorter limit is kept.
  if (p.trust < TrustLevel::TRUSTED) {
    p.max_runtime_seconds = WithinLimit(p.max_runtime_seconds,
                                        SecurityPolicy().max_runtime_seconds);
  }
  return p;
}

// static
void AutomationSecurity::ApplyPolicyFloor(SecurityPolicy& policy) {
  policy.require_approval_for.assign(std::begin(kDefaultApprovals),
                                     std::end(kDefaultApprovals));
  const SecurityPolicy defaults;
  policy.max_runtime_seconds =
      WithinLimit(policy.max_runtime_seconds, defaults.max_runtime_seconds);
  policy.max_navigations =
      WithinLimit(policy.max_navigations, defaults.max_navigations);
  policy.max_network_bytes =
      WithinLimit(policy.max_network_bytes, defaults.max_network_bytes);
}

// static
std::string AutomationSecurity::RunWindowStartURL(const Script& script,
                                                  size_t start_index) {
  if (script.security.sites_unconfirmed || HasVoiceSteps(script)) {
    return std::string();
  }
  // The step the runner starts at (out of range: the first, as the runner
  // clamps it) expects the page of the last NAVIGATE before it; with none
  // before it, of the first one.
  const size_t from = start_index < script.steps.size() ? start_index : 0;
  const Step* page_step = nullptr;
  for (size_t i = 0; i < script.steps.size(); ++i) {
    const Step& step = script.steps[i];
    if (step.type != StepType::NAVIGATE || step.target.empty()) {
      continue;
    }
    if (page_step && i >= from) {
      break;
    }
    page_step = &step;
  }
  if (!page_step) {
    return std::string();
  }
  // Its URL as the runner opens it (AutomationRunner::Resolve()): each
  // {{name}} replaced by the run's value of it. One no value is given for
  // (a value only a step sets) opens nothing.
  std::string target = page_step->target;
  for (const auto& [name, value] : script.default_variables) {
    base::ReplaceSubstringsAfterOffset(&target, 0, "{{" + name + "}}", value);
  }
  const GURL url(target);
  if (target.find("{{") != std::string::npos ||
      !agent_safety::IsAllowedNavigationURL(url) ||
      !AutomationSecurity().AllowNavigation(EffectivePolicy(script),
                                            url.spec())) {
    return std::string();
  }
  return url.spec();
}

// static
std::vector<std::string> AutomationSecurity::HostsReferencedBy(
    const std::vector<Step>& steps) {
  std::vector<std::string> hosts;
  for (const Step& s : steps) {
    if (s.type == StepType::NAVIGATE &&
        s.target.find("{{") == std::string::npos) {
      GURL g(s.target);
      if (agent_safety::IsAllowedNavigationURL(g))
        AddUnique(hosts, base::ToLowerASCII(g.host()));
    }
    if (s.extra.is_dict()) {
      const std::string* host = s.extra.GetDict().FindString("host");
      if (host && host->find("{{") == std::string::npos)
        AddUnique(hosts, base::ToLowerASCII(*host));
    }
  }
  return hosts;
}

// static
void AutomationSecurity::ClampTrustForSave(Script& incoming,
                                           const Script* existing) {
  incoming.security.trust =
      existing ? existing->security.trust : TrustLevel::CASUAL;
  if (!existing) {
    // A workflow this save brings in (an import, a page's save of one not
    // stored, a copy, a new one): nothing its JSON says about what it may
    // do is believed, and a schedule it brings is kept but turned off. It
    // runs by itself only once the user turns it on (Save schedule).
    ApplyPolicyFloor(incoming.security);
    if (FiresByItself(incoming.trigger)) {
      incoming.enabled = false;
    }
  }
  if (existing && existing->security.sites_unconfirmed) {
    incoming.security.sites_unconfirmed = true;
    // A file changed outside the browser stays one until it is confirmed.
    incoming.security.changed_outside = existing->security.changed_outside;
  }
  // A routine taught by voice that this save brings in (a new workflow, or
  // one that had no voice steps: an import, a copy, an edit) is a draft:
  // voice mode, which runs it, acts on no site of it until the user has
  // confirmed them (automation_voice_steps.h). Only voice mode's own save
  // of what the user said and did starts it confirmed.
  if (HasVoiceSteps(incoming) && !(existing && HasVoiceSteps(*existing))) {
    incoming.security.sites_unconfirmed = true;
    incoming.security.domain_whitelist.clear();
  }
}

// static
bool AutomationSecurity::ConfirmDraftSites(
    Script& script,
    const std::vector<std::string>& shown) {
  if (!script.security.sites_unconfirmed)
    return false;
  std::vector<std::string> sites = HostsReferencedBy(script.steps);
  // No site to confirm: confirmed with none only when the studio showed
  // none and it may be (a change whose every address comes from a value).
  // Its whitelist then becomes empty.
  if (sites.empty() && !(shown.empty() && MayConfirmWithoutSites(script)))
    return false;
  std::vector<std::string> sorted_sites = sites;
  std::vector<std::string> sorted_shown = shown;
  std::sort(sorted_sites.begin(), sorted_sites.end());
  std::sort(sorted_shown.begin(), sorted_shown.end());
  if (sorted_sites != sorted_shown)
    return false;
  script.security.domain_whitelist = std::move(sites);
  script.security.sites_unconfirmed = false;
  script.security.changed_outside = false;
  return true;
}

// static
bool AutomationSecurity::MayConfirmWithoutSites(const Script& script) {
  if (!script.security.sites_unconfirmed || !script.security.changed_outside ||
      script.security.trust >= TrustLevel::TRUSTED || HasVoiceSteps(script) ||
      !HostsReferencedBy(script.steps).empty()) {
    return false;
  }
  // Every address with a value in it is a value's: the value gives the
  // whole address, its site included. A fixed host with a value after it
  // ("https://shop.example/?q={{q}}"), or after an empty value
  // ("{{x}}https://shop.example/"), is no site HostsReferencedBy() lists,
  // so the review, which would list none, wouldn't show it.
  for (const Step& step : script.steps) {
    if (step.type == StepType::NAVIGATE &&
        step.target.find("{{") != std::string::npos &&
        !AddressIsAValue(step.target)) {
      return false;
    }
  }
  return true;
}

// static
std::string AutomationSecurity::SecurityDigest(const Script& script) {
  base::DictValue d = script.ToJSON();
  // What a load's scrub would remove never counts (it is gone from what
  // runs either way).
  ScrubPlaintextSecrets(d);
  // Run bookkeeping: the id is the file's name, the stats and the last
  // fire time change with every run (here or in another browser).
  d.Remove("id");
  d.Remove("stats");
  if (base::DictValue* trigger = d.FindDict("trigger")) {
    trigger->Remove("last_fired_unix");
  }
  // A DictValue keeps its keys sorted: the same script, the same text.
  std::string json;
  if (!base::JSONWriter::Write(d, &json)) {
    return std::string();
  }
  return base::HexEncodeLower(crypto::hash::Sha256(json));
}

// static
std::string AutomationSecurity::DigestAsStored(const Script& script) {
  // As AutomationStorage writes it, and as its readers parse it.
  base::DictValue d = script.ToJSON();
  ScrubPlaintextSecrets(d);
  std::string bytes;
  if (!base::JSONWriter::WriteWithOptions(
          d, base::JSONWriter::OPTIONS_PRETTY_PRINT, &bytes)) {
    return std::string();
  }
  std::optional<base::Value> parsed =
      base::JSONReader::Read(bytes, base::JSON_ALLOW_TRAILING_COMMAS);
  if (!parsed || !parsed->is_dict()) {
    return std::string();
  }
  std::optional<Script> stored = Script::FromJSON(parsed->GetDict());
  return stored ? SecurityDigest(*stored) : std::string();
}

// static
void AutomationSecurity::ClampUnvouched(Script& script) {
  // Only what the Agent studio's review shows, as the runner uses it (an
  // allowlist: whatever else the file has, any key a later build adds
  // included, is dropped), so the review token, and what a confirmation
  // saves, are of what the user saw.
  Script shown;
  shown.id = script.id;
  shown.name = script.name;
  // Information only (no run reads it).
  shown.created_at_unix = script.created_at_unix;
  // The schedule, shown as its kind and expression: not a time zone or an
  // end the review doesn't show (it runs in the browser's time zone, with
  // no end), nor one the file has turned off (no schedule then), nor an
  // expression that reads otherwise than it runs.
  if (script.enabled && script.trigger.type != TriggerType::MANUAL &&
      !HasUnshownChars(script.trigger.expression)) {
    shown.trigger.type = script.trigger.type;
    shown.trigger.expression = script.trigger.expression;
  }
  // Bookkeeping (outside the digest): the storage sets it from its record.
  shown.trigger.last_fired_unix = script.trigger.last_fired_unix;
  // Each step the review shows in full, with only the fields it shows
  // (ShownStep()); every other step is dropped: control flow, page
  // snapshots, code, the on-device model's steps, steps that run another
  // workflow, and a step whose one-line field, or a site tool's value,
  // would read otherwise than it runs. What the runner would act on when
  // the target shown misses (the fallback selectors and locators, the text
  // anchor, the picture of the recorded target, the description) goes with
  // the rest.
  for (const Step& step : script.steps) {
    if (std::optional<Step> kept = ShownStep(step)) {
      shown.steps.push_back(std::move(*kept));
    }
  }
  // The names of its variables, not their default values, which a run uses
  // wherever their {{name}} is (a scheduled one asks nobody): the review
  // shows a variable's value only where it is a step's whole field. A run
  // is given the values again, or uses none.
  for (const auto& [name, value] : script.default_variables) {
    shown.default_variables[name] = std::string();
  }
  // Nothing the file says about what it may do is believed: the default
  // policy, an unconfirmed draft. Nor its history (it would suggest
  // promotions it never earned here).
  shown.security.require_approval_for.assign(std::begin(kDefaultApprovals),
                                             std::end(kDefaultApprovals));
  shown.security.sites_unconfirmed = true;
  shown.security.changed_outside = true;
  script = std::move(shown);
}

// static
bool AutomationSecurity::MayFireUnattended(const Script& script) {
  return script.enabled && MayRunAsChild(script);
}

// static
bool AutomationSecurity::MayRunAsChild(const Script& script) {
  return !script.security.sites_unconfirmed && !HasVoiceSteps(script);
}

// static
const char* AutomationSecurity::ReviewMessage(const Script& script) {
  return script.security.changed_outside ? kChangedOutsideMessage
                                         : kDraftNeedsReviewMessage;
}

// static
void AutomationSecurity::MarkStepSecret(Step& step,
                                        agent_safety::FieldKind kind,
                                        std::string_view host) {
  step.value.clear();
  base::DictValue extra =
      step.extra.is_dict() ? step.extra.GetDict().Clone() : base::DictValue();
  extra.Set(kSecretKey, kSecretVault);
  extra.Set(kSecretKindKey, agent_safety::FieldKindToString(kind));
  if (!host.empty())
    extra.Set(kSecretHostKey, host);
  step.extra = base::Value(std::move(extra));
}

// static
agent_safety::FieldKind AutomationSecurity::SecretStepKind(const Step& step) {
  if (step.type != StepType::TYPE || !step.extra.is_dict())
    return agent_safety::FieldKind::kOrdinary;
  const base::DictValue& extra = step.extra.GetDict();
  if (!extra.FindString(kSecretKey))
    return agent_safety::FieldKind::kOrdinary;
  const std::string* kind = extra.FindString(kSecretKindKey);
  agent_safety::FieldKind k = agent_safety::FieldKindFromString(
      kind ? std::string_view(*kind) : std::string_view());
  // A secret step of unknown kind is treated as a password.
  return agent_safety::IsSensitive(k) ? k : agent_safety::FieldKind::kPassword;
}

// static
bool AutomationSecurity::ScrubPlaintextSecrets(base::DictValue& script_json) {
  base::ListValue* steps = script_json.FindList("steps");
  if (!steps)
    return false;
  std::vector<std::string> removed_values;
  for (base::Value& v : *steps) {
    if (!v.is_dict())
      continue;
    base::DictValue& d = v.GetDict();
    const std::string* type = d.FindString("type");
    const std::string* value = d.FindString("value");
    if (!type || *type != "type" || !value || value->empty())
      continue;

    agent_safety::FieldKind kind = agent_safety::FieldKind::kOrdinary;
    std::string host;
    if (const base::DictValue* extra = d.FindDict("extra")) {
      if (extra->FindString(kSecretKey)) {
        const std::string* k = extra->FindString(kSecretKindKey);
        kind = agent_safety::FieldKindFromString(
            k ? std::string_view(*k) : std::string_view());
        if (!agent_safety::IsSensitive(kind))
          kind = agent_safety::FieldKind::kPassword;
      }
      if (const std::string* h = extra->FindString(kStepHostKey))
        host = *h;
    }
    const std::string* desc = d.FindString("description");
    if (kind == agent_safety::FieldKind::kOrdinary && desc &&
        desc->find(kLegacyPasswordMask) != std::string::npos) {
      kind = agent_safety::FieldKind::kPassword;
    }
    if (kind == agent_safety::FieldKind::kOrdinary) {
      if (const std::string* target = d.FindString("target"))
        kind = agent_safety::ClassifySelector(*target);
    }
    if (kind == agent_safety::FieldKind::kOrdinary) {
      if (const base::ListValue* fallbacks = d.FindList("selector_fallbacks")) {
        for (const base::Value& f : *fallbacks) {
          if (!f.is_string())
            continue;
          kind = agent_safety::ClassifySelector(f.GetString());
          if (agent_safety::IsSensitive(kind))
            break;
        }
      }
    }
    if (!agent_safety::IsSensitive(kind))
      continue;

    const std::string secret = *value;
    removed_values.push_back(secret);
    if (desc && desc->find(secret) != std::string::npos)
      d.Set("description", SecretStepDescription(kind));
    d.Remove("value");
    base::DictValue extra =
        d.FindDict("extra") ? d.FindDict("extra")->Clone() : base::DictValue();
    extra.Set(kSecretKey, kSecretVault);
    extra.Set(kSecretKindKey, agent_safety::FieldKindToString(kind));
    if (!host.empty() && !extra.FindString(kSecretHostKey))
      extra.Set(kSecretHostKey, host);
    d.Set("extra", std::move(extra));
  }
  if (removed_values.empty())
    return false;

  // Run history stores each step's resolved description ("desc"); redact
  // any that quoted a removed value. Very short values are skipped so a
  // one-character "secret" doesn't blank unrelated entries.
  if (base::DictValue* stats = script_json.FindDict("stats")) {
    if (base::ListValue* history = stats->FindList("run_history")) {
      for (base::Value& run : *history) {
        if (!run.is_dict())
          continue;
        base::ListValue* run_steps = run.GetDict().FindList("steps");
        if (!run_steps)
          continue;
        for (base::Value& rs : *run_steps) {
          if (!rs.is_dict())
            continue;
          const std::string* desc = rs.GetDict().FindString("desc");
          if (!desc)
            continue;
          for (const std::string& secret : removed_values) {
            if (secret.size() >= 3 && desc->find(secret) != std::string::npos) {
              rs.GetDict().Set("desc", "[redacted]");
              break;
            }
          }
        }
      }
    }
  }
  return true;
}

TrustLevel AutomationSecurity::ConsiderPromotion(TrustLevel current,
                                                  const Stats& stats) const {
  switch (current) {
    case TrustLevel::CASUAL:
      // After the first successful run, propose APPROVED.
      if (stats.successes >= 1) return TrustLevel::APPROVED;
      return TrustLevel::CASUAL;
    case TrustLevel::APPROVED:
      // After 5 successful runs, propose TRUSTED.
      if (stats.successes >= 5) return TrustLevel::TRUSTED;
      return TrustLevel::APPROVED;
    case TrustLevel::TRUSTED:
    case TrustLevel::ADMIN:
      // ADMIN requires explicit user opt-in, never auto-promoted.
      return current;
  }
  return current;
}

}  // namespace automation
}  // namespace molt_ai
