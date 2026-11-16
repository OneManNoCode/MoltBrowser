// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/webui/molt_ai/molt_ai_automation_storage.h"

#include <algorithm>
#include <utility>

#include "base/strings/string_number_conversions.h"
#include "chrome/browser/molt_ai/automation/automation_security.h"
#include "chrome/browser/molt_ai/automation/automation_voice_steps.h"

namespace molt_ai {
namespace automation_page {

namespace {

using automation::AutomationSecurity;
using automation::AutomationStorage;
using automation::HasVoiceSteps;
using automation::Script;
using automation::TrustLevel;

base::DictValue Failed(const std::string& error) {
  base::DictValue result;
  result.Set("success", false);
  result.Set("error", error);
  return result;
}

}  // namespace

std::vector<Script> ListWorkflows(AutomationStorage& storage) {
  std::vector<Script> out;
  for (Script& script : storage.ListAll()) {
    if (!HasVoiceSteps(script)) {
      out.push_back(std::move(script));
    }
  }
  return out;
}

std::optional<Script> LoadWorkflow(const std::string& id,
                                   AutomationStorage& storage) {
  std::optional<Script> script = storage.Load(id);
  if (script && HasVoiceSteps(*script)) {
    return std::nullopt;
  }
  return script;
}

size_t StartIndex(const Script& script, int start_from) {
  return std::min(static_cast<size_t>(std::max(0, start_from)),
                  script.steps.size());
}

std::optional<Script> LoadToRun(const std::string& id,
                                int start_from,
                                AutomationStorage& storage) {
  std::optional<Script> script = LoadWorkflow(id, storage);
  if (script) {
    const size_t start = StartIndex(*script, start_from);
    storage.AppendAudit(id, "manual_run_requested",
                        start > 0 ? "from_step=" + base::NumberToString(start)
                                  : std::string("from_ui"));
  }
  return script;
}

base::DictValue SaveWorkflow(Script script,
                             int64_t now_unix,
                             bool schedule_saved,
                             AutomationStorage& storage) {
  if (HasVoiceSteps(script)) {
    return Failed(kVoiceRoutinesElsewhere);
  }
  bool stored_routine = false;
  const bool saved = storage.SaveChecked(
      std::move(script), [&stored_routine, now_unix, schedule_saved](
                             const Script* stored, Script& s) {
        if (stored && HasVoiceSteps(*stored)) {
          stored_routine = true;
          return false;
        }
        if (stored) {
          // The page never edits the policy: the stored one is kept (its
          // trust changed only by promoteTrust).
          s.security = stored->security;
        } else {
          // Not stored (deleted meanwhile, or a dict the page made): taken
          // as an import. No runs, no last fire.
          s.stats = automation::Stats();
          s.trigger.last_fired_unix = 0;
        }
        // The stored trust, or CASUAL with the policy floor and any
        // schedule turned off for a new one.
        AutomationSecurity::ClampTrustForSave(s, stored);
        // An interval saved afresh (Save schedule sends no last fire), or
        // a new one, counts from this save: the scheduler would count one
        // with none from each listing, and never fire it. Any other save
        // leaves one an earlier version saved with none as it is (shown
        // "not started"): only Save schedule starts it.
        if (!stored || schedule_saved) {
          automation::CountIntervalFromNow(s.trigger, now_unix);
        }
        return true;
      });
  if (stored_routine) {
    return Failed(kVoiceRoutinesElsewhere);
  }
  base::DictValue result;
  result.Set("success", saved);
  return result;
}

bool DeleteWorkflow(const std::string& id, AutomationStorage& storage) {
  return LoadWorkflow(id, storage) && storage.Delete(id);
}

bool SaveSample(Script script, AutomationStorage& storage) {
  return storage.Save(script);
}

std::string SaveBlank(Script script, AutomationStorage& storage) {
  return storage.SaveNew(std::move(script)).value_or(std::string());
}

base::DictValue ImportWorkflow(Script script,
                               int64_t now_unix,
                               AutomationStorage& storage) {
  // Never over a stored workflow (of the same name, say): a new id when
  // the original is taken, and the first free one after it.
  if (storage.Load(script.id)) {
    script.id += "-imported-" + base::NumberToString(now_unix);
  }
  // The importer hasn't earned anything yet: no runs, and whatever the
  // JSON claims, CASUAL with the default approvals and limits, and its
  // schedule (kept) turned off until the user turns it on.
  script.stats = automation::Stats();
  AutomationSecurity::ClampTrustForSave(script, /*existing=*/nullptr);
  const bool schedule_off =
      !script.enabled && automation::FiresByItself(script.trigger);
  // Nor has it fired here: its schedule starts over, whatever last fire the
  // JSON gives (an earlier one would run it at once, a later one hold it
  // back), an interval counted from the import.
  script.trigger.last_fired_unix = 0;
  automation::CountIntervalFromNow(script.trigger, now_unix);
  const std::optional<std::string> id = storage.SaveNew(std::move(script));
  if (id) {
    storage.AppendAudit(*id, "imported",
                        schedule_off ? "from_ui, schedule off" : "from_ui");
  }
  base::DictValue result;
  result.Set("success", id.has_value());
  result.Set("script_id", id.value_or(std::string()));
  result.Set("schedule_off", schedule_off);
  return result;
}

std::vector<std::string> AuditTail(int max_lines, AutomationStorage& storage) {
  return storage.ReadAuditTail(max_lines);
}

base::DictValue PromoteTrust(const std::string& id,
                             TrustLevel level,
                             AutomationStorage& storage) {
  bool found = false;
  bool draft = false;
  bool changed_outside = false;
  TrustLevel before = level;
  const bool saved = storage.Update(id, [&](Script& script) {
    if (HasVoiceSteps(script)) {
      return false;
    }
    found = true;
    // A draft from a web agent run, or a workflow whose file was changed
    // outside the browser, is trusted only after its sites are confirmed in
    // the Agent studio.
    if (script.security.sites_unconfirmed) {
      draft = true;
      changed_outside = script.security.changed_outside;
      return false;
    }
    before = script.security.trust;
    script.security.trust = level;
    return true;
  });
  if (!found) {
    return Failed("script not found");
  }
  if (draft) {
    return Failed(changed_outside
                      ? "This workflow was changed outside MoltBrowser. "
                        "Review it in Agent mode first."
                      : "Confirm this draft's sites in Agent mode first.");
  }
  if (saved) {
    storage.AppendAudit(id, "trust_changed",
                        automation::TrustLevelToString(before) + " -> " +
                            automation::TrustLevelToString(level));
  }
  base::DictValue result;
  result.Set("success", saved);
  result.Set("trust", automation::TrustLevelToString(level));
  return result;
}

}  // namespace automation_page
}  // namespace molt_ai
