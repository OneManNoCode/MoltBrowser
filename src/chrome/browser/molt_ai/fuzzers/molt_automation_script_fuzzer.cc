// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Agent-mode workflow files (automation/automation_script.h): a .molt file
// the user imported (anyone may have written it), and the trust policy
// applied to it (automation/automation_security.h). The whole input is the
// file.
//
// Checks: no memory or UB error; a parsed workflow saves and loads back to
// the same file; scrubbing plaintext secrets leaves no value on a step it
// recognizes as secret, is idempotent and still loads; the effective policy
// keeps the file's own whitelist and approvals, never drops a default
// approval, bounds the runtime below TRUSTED and never adds a templated
// host; trust is never raised by loading or saving (a new script starts
// CASUAL with the default approvals, each limit within its default and any
// schedule turned off; promotion never reaches ADMIN); a wait runs only a
// value its studio field shows as it is, 0 to 60 s; a step's AI_EXTRACT
// field list,
// when accepted, gives a schema the inference service supports; the
// security digest is empty or 64 lowercase hex digits, and the same once
// the workflow is written and read back; a file the browser didn't write,
// once clamped, may do nothing on its own (CASUAL, no sites, never fired
// nor run by another workflow), keeps only what the Agent studio's review
// shows (the allowlist: step types, fields, extra keys, no control
// character in a one-line field or a site tool's value, no default value),
// clamped again is the same, its digest is what a reader of its saved file
// computes, and it is confirmed with no site only when its steps target
// none and every address with a {{variable}} in it starts with one (not a
// routine taught by voice), still opening no page by itself;
// an unconfirmed draft's effective trust is CASUAL, whatever its file says.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/check.h"
#include "base/json/json_reader.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/automation/automation_ai_steps.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/automation/automation_security.h"
#include "chrome/browser/molt_ai/automation/automation_voice_steps.h"
#include "chrome/browser/molt_ai/security/agent_safety.h"
#include "chrome/services/molt_ai/inference/engine/json_schema_grammar.h"

namespace molt_ai::automation {
namespace {

void CheckScrubbed(const base::DictValue& json) {
  const base::ListValue* steps = json.FindList("steps");
  if (!steps) return;
  for (const base::Value& v : *steps) {
    if (!v.is_dict()) continue;
    const base::DictValue& d = v.GetDict();
    const std::string* type = d.FindString("type");
    if (!type || *type != "type") continue;
    const std::string* value = d.FindString("value");
    if (!value || value->empty()) continue;
    // A step that still has a value isn't recognizably secret.
    if (const base::DictValue* extra = d.FindDict("extra")) {
      CHECK(!extra->FindString(kSecretKey)) << "a secret step kept its value";
    }
    if (const std::string* target = d.FindString("target")) {
      CHECK(!agent_safety::SelectorLooksSensitive(*target))
          << "a password/card field's value was kept";
    }
  }
}

}  // namespace
}  // namespace molt_ai::automation

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace molt_ai::automation;
  const std::string json(reinterpret_cast<const char*>(data), size);

  // Scrubbing runs on the parsed file before anything else is done with it
  // (AutomationStorage, on load and save).
  std::optional<base::Value> parsed =
      base::JSONReader::Read(json, base::JSON_ALLOW_TRAILING_COMMAS);
  if (parsed && parsed->is_dict()) {
    base::DictValue dict = parsed->GetDict().Clone();
    const bool changed = AutomationSecurity::ScrubPlaintextSecrets(dict);
    CheckScrubbed(dict);
    base::DictValue again = dict.Clone();
    CHECK(!AutomationSecurity::ScrubPlaintextSecrets(again))
        << "scrubbing isn't idempotent";
    CHECK(again == dict);
    if (!changed) CHECK(dict == parsed->GetDict());
    Script::FromJSON(dict);
  }

  std::optional<Script> script = Script::FromJSONString(json);
  if (!script) return 0;
  CHECK(!script->id.empty());

  // Save and load: the same workflow.
  const std::string saved = script->ToJSONString();
  std::optional<Script> loaded = Script::FromJSONString(saved);
  CHECK(loaded) << "a loaded workflow doesn't load after saving";
  CHECK(loaded->ToJSONString() == saved) << "save/load changed the workflow";

  // The security digest (workflow_record.h): well formed, and what a
  // reader of the saved file computes.
  const std::string digest = AutomationSecurity::SecurityDigest(*script);
  CHECK(digest.empty() || digest.size() == 64u);
  for (char c : digest) {
    CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
  }
  CHECK(AutomationSecurity::DigestAsStored(*script) ==
        AutomationSecurity::SecurityDigest(*loaded))
      << "the digest moves when the workflow is saved";

  // A file the browser didn't write: an unconfirmed import.
  {
    Script clamped = std::move(*Script::FromJSONString(saved));
    AutomationSecurity::ClampUnvouched(clamped);
    const SecurityPolicy p = AutomationSecurity::EffectivePolicy(clamped);
    CHECK(p.trust == TrustLevel::CASUAL);
    CHECK(p.domain_whitelist.empty());
    CHECK(!AutomationSecurity::MayFireUnattended(clamped));
    CHECK(!AutomationSecurity::MayRunAsChild(clamped));
    // Nothing the review doesn't show (§3.6.6's allowlist): no default
    // value, no policy or history of the file's, and each step of a type
    // the review shows in full, with only its fields and extra keys.
    for (const auto& [name, value] : clamped.default_variables) {
      CHECK(value.empty());
    }
    CHECK(clamped.stats.runs == 0 && clamped.stats.run_history.empty());
    CHECK(clamped.security.changed_outside &&
          clamped.security.sites_unconfirmed);
    CHECK(clamped.trigger.timezone.empty() && clamped.trigger.until_unix == 0);
    CHECK(clamped.enabled);
    for (const Step& step : clamped.steps) {
      const Step defaults;
      CHECK(step.description.empty());
      CHECK(step.selector_fallbacks.empty());
      CHECK(!step.locators);
      CHECK(step.transform.empty());
      CHECK(step.timeout_ms == defaults.timeout_ms);
      CHECK(step.max_iterations == defaults.max_iterations);
      CHECK(step.retries == defaults.retries);
      const base::DictValue* extra = step.extra.GetIfDict();
      CHECK(extra || step.extra.is_none());
      if (extra && extra->FindDict(kVoiceStepKey)) {
        // A routine's step: its voice dict and host only, never a secret.
        for (const auto [key, value] : *extra) {
          CHECK(key == kVoiceStepKey || key == kStepHostKey) << key;
        }
        continue;
      }
      const StepType t = step.type;
      CHECK(t == StepType::NAVIGATE || t == StepType::CLICK ||
            t == StepType::TYPE || t == StepType::SCROLL ||
            t == StepType::KEYPRESS || t == StepType::WAIT ||
            t == StepType::WAIT_FOR || t == StepType::EXTRACT ||
            t == StepType::ASSERT || t == StepType::NOTIFY ||
            t == StepType::CALL_TOOL)
          << StepTypeToString(t);
      if (t == StepType::SCROLL || t == StepType::KEYPRESS ||
          t == StepType::WAIT || t == StepType::NOTIFY) {
        CHECK(step.target.empty());
      }
      if (t == StepType::NAVIGATE || t == StepType::CLICK ||
          t == StepType::WAIT_FOR || t == StepType::EXTRACT ||
          t == StepType::CALL_TOOL) {
        CHECK(step.value.empty());
      }
      if (t != StepType::EXTRACT) {
        CHECK(step.store_as.empty());
      }
      for (const std::string* field :
           {&step.target, &step.value, &step.store_as}) {
        for (char ch : *field) {
          const unsigned char c = static_cast<unsigned char>(ch);
          CHECK(c >= 0x20 && c != 0x7F) << "a control character shown";
        }
      }
      if (!extra) {
        continue;
      }
      for (const auto [key, value] : *extra) {
        const bool allowed = key == kStepHostKey ||
                             (t == StepType::TYPE &&
                              (key == kSecretKey || key == kSecretKindKey)) ||
                             (t == StepType::CALL_TOOL && key == "tool");
        CHECK(allowed) << key;
      }
      if (const base::DictValue* tool = extra->FindDict("tool")) {
        constexpr std::string_view kToolKeys[] = {"origin", "frame", "args",
                                                  "secret_args", "form"};
        for (const auto [key, value] : *tool) {
          CHECK(std::find(std::begin(kToolKeys), std::end(kToolKeys), key) !=
                std::end(kToolKeys))
              << key;
        }
        // No value the review would lay out as lines or space.
        const auto shown = [](std::string_view text) {
          for (char ch : text) {
            const unsigned char c = static_cast<unsigned char>(ch);
            CHECK(c >= 0x20 && c != 0x7F) << "a tool value laid out";
          }
        };
        if (const base::DictValue* args = tool->FindDict("args")) {
          for (const auto [name, value] : *args) {
            shown(name);
            if (value.is_string()) {
              shown(value.GetString());
            }
            if (value.is_list()) {
              for (const base::Value& item : value.GetList()) {
                if (item.is_string()) {
                  shown(item.GetString());
                }
              }
            }
          }
        }
      }
    }
    // Confirmed with no site only when its steps target none, every
    // address with a {{variable}} in it starts with one (no fixed host
    // before a value, which the review wouldn't show) and it isn't a
    // routine taught by voice; then its whitelist is empty, so it opens no
    // page by itself: no step's address is allowed (below TRUSTED), nor a
    // run window's first page.
    {
      std::optional<Script> copy =
          Script::FromJSONString(clamped.ToJSONString());
      CHECK(copy);
      const bool values_whole = std::none_of(
          copy->steps.begin(), copy->steps.end(), [](const Step& step) {
            const size_t value = step.target.find("{{");
            return step.type == StepType::NAVIGATE &&
                   value != std::string::npos && value != 0;
          });
      const bool no_site =
          AutomationSecurity::HostsReferencedBy(copy->steps).empty() &&
          values_whole && !HasVoiceSteps(*copy);
      CHECK(AutomationSecurity::MayConfirmWithoutSites(*copy) == no_site);
      CHECK(AutomationSecurity::ConfirmDraftSites(*copy, {}) == no_site);
      CHECK(copy->security.sites_unconfirmed == !no_site);
      if (no_site) {
        CHECK(copy->security.domain_whitelist.empty());
        CHECK(copy->security.trust == TrustLevel::CASUAL);
        const SecurityPolicy confirmed =
            AutomationSecurity::EffectivePolicy(*copy);
        CHECK(confirmed.domain_whitelist.empty());
        for (const Step& step : copy->steps) {
          CHECK(!AutomationSecurity().AllowNavigation(confirmed, step.target));
        }
        CHECK(AutomationSecurity::RunWindowStartURL(*copy, 0).empty());
      }
    }
    // Clamping twice is clamping once (the review token holds), and the
    // digest is what a reader of its saved file computes (the token a save
    // of it answers with).
    const std::string token = AutomationSecurity::SecurityDigest(clamped);
    std::optional<Script> clamped_read =
        Script::FromJSONString(clamped.ToJSONString());
    CHECK(clamped_read);
    CHECK(AutomationSecurity::DigestAsStored(clamped) ==
          AutomationSecurity::SecurityDigest(*clamped_read));
    AutomationSecurity::ClampUnvouched(clamped);
    CHECK(AutomationSecurity::SecurityDigest(clamped) == token);
    CHECK(AutomationSecurity::DigestAsStored(clamped) ==
          AutomationSecurity::SecurityDigest(*clamped_read));
  }

  // Trust: imports start CASUAL, saving keeps the stored level. A new one
  // also gets the policy floor (the default approvals, each limit within
  // its default) and any schedule it brings turned off.
  Script incoming = std::move(*Script::FromJSONString(saved));
  AutomationSecurity::ClampTrustForSave(incoming, nullptr);
  CHECK(incoming.security.trust == TrustLevel::CASUAL);
  {
    const SecurityPolicy defaults;
    CHECK(incoming.security.require_approval_for ==
          std::vector<std::string>({"form_submit", "payment", "login"}));
    CHECK(incoming.security.max_runtime_seconds > 0 &&
          incoming.security.max_runtime_seconds <=
              defaults.max_runtime_seconds);
    CHECK(incoming.security.max_navigations > 0 &&
          incoming.security.max_navigations <= defaults.max_navigations);
    CHECK(incoming.security.max_network_bytes > 0 &&
          incoming.security.max_network_bytes <= defaults.max_network_bytes);
    CHECK(!incoming.enabled || !FiresByItself(incoming.trigger));
  }
  AutomationSecurity::ClampTrustForSave(incoming, &*loaded);
  CHECK(incoming.security.trust == loaded->security.trust);

  AutomationSecurity security;
  const SecurityPolicy policy = AutomationSecurity::EffectivePolicy(*script);
  CHECK(!policy.require_approval_for.empty());
  // Never without a default approval, whatever the file lists; below
  // TRUSTED, a runtime within the default.
  for (const char* category : {"form_submit", "payment", "login"}) {
    CHECK(std::find(policy.require_approval_for.begin(),
                    policy.require_approval_for.end(),
                    category) != policy.require_approval_for.end())
        << "the effective policy dropped " << category;
  }
  if (policy.trust < TrustLevel::TRUSTED) {
    CHECK(policy.max_runtime_seconds > 0 &&
          policy.max_runtime_seconds <= SecurityPolicy().max_runtime_seconds);
  }
  // An unconfirmed draft is CASUAL whatever its file says.
  CHECK(policy.trust == (script->security.sites_unconfirmed
                             ? TrustLevel::CASUAL
                             : script->security.trust));
  for (const std::string& host : script->security.domain_whitelist) {
    if (host.empty()) continue;
    bool kept = false;
    for (const std::string& h : policy.domain_whitelist) kept |= h == host;
    CHECK(kept) << "the effective policy dropped a whitelisted host";
  }
  for (const std::string& host : AutomationSecurity::HostsReferencedBy(script->steps)) {
    CHECK(!host.empty() && host.find("{{") == std::string::npos);
  }
  const TrustLevel promoted =
      security.ConsiderPromotion(script->security.trust, script->stats);
  CHECK(promoted >= script->security.trust) << "promotion demoted";
  if (script->security.trust != TrustLevel::ADMIN) {
    CHECK(promoted != TrustLevel::ADMIN) << "auto-promoted to ADMIN";
  }
  CHECK(security.AllowHeadless(policy) == (policy.trust >= TrustLevel::TRUSTED));

  int checked = 0;
  for (Step& step : script->steps) {
    if (++checked > 64) break;
    // A wait runs only a value the studio's number field shows as it is:
    // "" (2 s), or a number from 0 to 60 with no spaces, '+' or trailing
    // '.' (WaitMillisecondsOf()).
    if (std::optional<int> wait_ms = WaitMillisecondsOf(step.value)) {
      CHECK(*wait_ms >= 0 && *wait_ms <= kMaxWaitSeconds * 1000);
      const std::string& v = step.value;
      if (!v.empty()) {
        CHECK(v.front() == '-' || v.front() == '.' ||
              (v.front() >= '0' && v.front() <= '9'));
        CHECK(v.back() != '.');
        for (char c : v) {
          CHECK(c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f' &&
                c != '\v');
        }
      }
    }
    const std::string& url = step.target;
    const bool nav = security.AllowNavigation(policy, url);
    if (policy.trust < TrustLevel::TRUSTED && nav) {
      CHECK(!policy.domain_whitelist.empty());
    }
    security.AllowClick(policy, url, step.value);
    security.AllowType(policy, url, step.target, step.value);
    security.AllowSubmit(policy);
    const molt_ai::agent_safety::FieldKind kind =
        AutomationSecurity::SecretStepKind(step);
    if (molt_ai::agent_safety::IsSensitive(kind) &&
        policy.trust < TrustLevel::APPROVED) {
      // Listed approvals gate sensitive fields below APPROVED.
      const bool listed = [&] {
        for (const std::string& a : policy.require_approval_for) {
          if (a == (kind == molt_ai::agent_safety::FieldKind::kPaymentCard
                        ? "payment"
                        : "login")) {
            return true;
          }
        }
        return false;
      }();
      CHECK(security.AllowSensitiveField(policy, kind) == !listed);
    }
    if (step.type == StepType::AI_EXTRACT) {
      const ai_steps::ExtractSpecResult spec =
          ai_steps::ExtractSpecFromStep(step.extra);
      if (spec.spec) {
        CHECK(spec.spec->fields.size() <= ai_steps::kMaxExtractFields);
        const molt_ai::SchemaGrammar grammar = molt_ai::JsonSchemaToGrammar(
            ai_steps::BuildExtractSchema(*spec.spec));
        CHECK(grammar.status == molt_ai::SchemaGrammarStatus::kOk)
            << "the service would refuse the extract schema: "
            << grammar.reason;
        ai_steps::DescribeExtractFields(*spec.spec);
      } else {
        CHECK(!spec.error.empty());
      }
    }
    if (step.type == StepType::TYPE && step.value.empty()) {
      AutomationSecurity::MarkStepSecret(
          step, molt_ai::agent_safety::FieldKind::kOneTimeCode, "example.com");
      CHECK(AutomationSecurity::SecretStepKind(step) ==
            molt_ai::agent_safety::FieldKind::kOneTimeCode);
    }
  }
  return 0;
}
