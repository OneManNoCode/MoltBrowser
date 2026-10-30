// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/automation_security.h"

#include <algorithm>
#include <iterator>
#include <string_view>
#include <utility>

#include "base/strings/string_util.h"
#include "chrome/browser/molt_ai/automation/automation_voice_steps.h"
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

// Approval categories applied when a script's require_approval_for is empty
// (the struct default; studio-saved scripts were stored that way).
constexpr const char* kDefaultApprovals[] = {"form_submit", "payment",
                                             "login"};

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

bool DomainMatchesWhitelist(const std::vector<std::string>& whitelist,
                             std::string_view host) {
  std::string h = base::ToLowerASCII(host);
  for (const auto& entry : whitelist) {
    std::string e = base::ToLowerASCII(entry);
    if (h == e) return true;
    // Suffix match: 'kayak.com' allows 'www.kayak.com'.
    if (h.size() > e.size() &&
        h.compare(h.size() - e.size() - 1, e.size() + 1, "." + e) == 0) {
      return true;
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
  if (p.require_approval_for.empty()) {
    p.require_approval_for.assign(std::begin(kDefaultApprovals),
                                  std::end(kDefaultApprovals));
  }
  return p;
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
  if (existing && existing->security.sites_unconfirmed)
    incoming.security.sites_unconfirmed = true;
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
  if (sites.empty())
    return false;
  std::vector<std::string> sorted_sites = sites;
  std::vector<std::string> sorted_shown = shown;
  std::sort(sorted_sites.begin(), sorted_sites.end());
  std::sort(sorted_shown.begin(), sorted_shown.end());
  if (sorted_sites != sorted_shown)
    return false;
  script.security.domain_whitelist = std::move(sites);
  script.security.sites_unconfirmed = false;
  return true;
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
