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
// keeps the file's own whitelist and approvals and never adds a templated
// host; trust is never raised by loading or saving (a new script starts
// CASUAL, promotion never reaches ADMIN); a step's AI_EXTRACT field list,
// when accepted, gives a schema the inference service supports.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <string>

#include "base/check.h"
#include "base/json/json_reader.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/automation/automation_ai_steps.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/automation/automation_security.h"
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

  // Trust: imports start CASUAL, saving keeps the stored level.
  Script incoming = std::move(*Script::FromJSONString(saved));
  AutomationSecurity::ClampTrustForSave(incoming, nullptr);
  CHECK(incoming.security.trust == TrustLevel::CASUAL);
  AutomationSecurity::ClampTrustForSave(incoming, &*loaded);
  CHECK(incoming.security.trust == loaded->security.trust);

  AutomationSecurity security;
  const SecurityPolicy policy = AutomationSecurity::EffectivePolicy(*script);
  CHECK(!policy.require_approval_for.empty());
  CHECK(policy.trust == script->security.trust);
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
