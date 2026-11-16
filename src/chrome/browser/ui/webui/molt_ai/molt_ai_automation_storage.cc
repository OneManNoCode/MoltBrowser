// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/webui/molt_ai/molt_ai_automation_storage.h"

#include <algorithm>
#include <utility>

#include "base/functional/function_ref.h"
#include "base/strings/string_number_conversions.h"
#include "chrome/browser/molt_ai/automation/automation_scheduler.h"
#include "chrome/browser/molt_ai/automation/automation_security.h"
#include "chrome/browser/molt_ai/automation/automation_voice_steps.h"

namespace molt_ai {
namespace automation_page {

namespace {

using automation::AutomationScheduler;
using automation::AutomationSecurity;
using automation::AutomationStorage;
using automation::HasVoiceSteps;
using automation::Script;
using automation::Trigger;
using automation::TriggerType;
using automation::TrustLevel;

constexpr char kScriptNotFound[] = "script not found";

base::DictValue Failed(const std::string& error) {
  base::DictValue result;
  result.Set("success", false);
  result.Set("error", error);
  return result;
}

// Why an unconfirmed draft isn't changed here: it is reviewed in Agent mode
// first.
const char* ReviewFirst(const Script& script) {
  return script.security.changed_outside
             ? "This workflow was changed outside MoltBrowser. Review it in "
               "Agent mode first."
             : "Confirm this draft's sites in Agent mode first.";
}

// Why |script| can't be held (snoozed or skipped); nullopt when it can.
std::optional<std::string> HoldRefused(const Script& script) {
  // Voice mode runs those, and this page lists none.
  if (HasVoiceSteps(script)) {
    return kScriptNotFound;
  }
  // A draft doesn't fire anyway; its schedule is reviewed first.
  if (script.security.sites_unconfirmed) {
    return ReviewFirst(script);
  }
  if (script.trigger.type != TriggerType::CRON &&
      script.trigger.type != TriggerType::INTERVAL) {
    return "Only a schedule that repeats can be snoozed or skipped";
  }
  // Saved with no last fire by an earlier version: the scheduler counts it
  // from each listing, so a hold would hold nothing to come. Only Save
  // schedule starts it.
  if (script.trigger.type == TriggerType::INTERVAL &&
      script.trigger.last_fired_unix <= 0) {
    // In the page's words (refuseNotStarted()), its dash included.
    return "Not started \xE2\x80\x94 click Save schedule to start it";
  }
  return std::nullopt;
}

// The hold |until| gives for workflow |id| as stored (or, in |error|, why
// none), set under the directory's lock (AutomationStorage::HoldWith()),
// and the page's answer: {success, held_until_unix, next_fire_unix?} (its
// next run after |now|, the hold counted), or {success: false, error}. A
// hold set is in the audit log as |audit_event|, "until=<unix time>", so a
// run that doesn't happen because of one is explained there.
using HoldUntil =
    base::FunctionRef<std::optional<int64_t>(const Script&, std::string&)>;
base::DictValue HoldAnswer(const std::string& id,
                           base::Time now,
                           AutomationStorage& storage,
                           const char* audit_event,
                           HoldUntil until) {
  std::string error = kScriptNotFound;
  Trigger trigger;
  const std::optional<int64_t> held =
      storage.HoldWith(id, [&](const Script& script) -> std::optional<int64_t> {
        if (std::optional<std::string> refused = HoldRefused(script)) {
          error = std::move(*refused);
          return std::nullopt;
        }
        std::optional<int64_t> held_until = until(script, error);
        if (held_until) {
          // The answer should the record have no entry to hold (a file this
          // browser didn't write is a draft, refused above).
          error = "This schedule couldn't be held";
          trigger = script.trigger;
        }
        return held_until;
      });
  if (!held) {
    return Failed(error);
  }
  storage.AppendAudit(id, audit_event, "until=" + base::NumberToString(*held));
  trigger.held_until_unix = *held;
  base::DictValue result;
  result.Set("success", true);
  result.Set("held_until_unix", static_cast<double>(*held));
  const base::Time next = AutomationScheduler::NextFireTime(trigger, now);
  if (!next.is_null()) {
    result.Set("next_fire_unix", static_cast<double>(next.ToTimeT()));
  }
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
          // Nor moves the last fire later: that would hold the schedule
          // back (a fire time far ahead, from a page or anything that can
          // send this message, would stall it), and Snooze and Skip are
          // holds of their own (Snooze(), SkipNext()). An earlier one is
          // left to the storage (AutomationStorage::SaveChecked()).
          s.trigger.last_fired_unix = std::min(s.trigger.last_fired_unix,
                                               stored->trigger.last_fired_unix);
        } else {
          // Not stored (deleted meanwhile, or a dict the page made): taken
          // as an import. No runs, no last fire, no sites of its own.
          s.stats = automation::Stats();
          s.trigger.last_fired_unix = 0;
          s.security.domain_whitelist.clear();
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
        } else if (s.trigger.last_fired_unix <= 0 &&
                   s.trigger.type == stored->trigger.type &&
                   s.trigger.expression == stored->trigger.expression &&
                   s.trigger.timezone == stored->trigger.timezone &&
                   s.trigger.until_unix == stored->trigger.until_unix) {
          // A card listed before the schedule was started elsewhere (an
          // open card isn't refreshed): it stays as stored, started or
          // not.
          s.trigger.last_fired_unix = stored->trigger.last_fired_unix;
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
  // schedule (kept) turned off until the user turns it on. Nor sites of its
  // own, which nothing on this page shows: only the hosts its steps name
  // count (EffectivePolicy()), as for Agent mode's import.
  script.stats = automation::Stats();
  script.security.domain_whitelist.clear();
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
  const char* draft = nullptr;
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
      draft = ReviewFirst(script);
      return false;
    }
    before = script.security.trust;
    script.security.trust = level;
    return true;
  });
  if (!found) {
    return Failed(kScriptNotFound);
  }
  if (draft) {
    return Failed(draft);
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

base::DictValue Snooze(const std::string& id,
                       int seconds,
                       int64_t now_unix,
                       AutomationStorage& storage) {
  return HoldAnswer(id, base::Time::FromTimeT(now_unix), storage, "snoozed",
                    [now_unix, seconds](const Script&, std::string&) {
                      return std::optional<int64_t>(now_unix + seconds);
                    });
}

base::DictValue SkipNext(const std::string& id,
                         base::Time now,
                         AutomationStorage& storage) {
  const auto past_next_run = [now](
                                 const Script& script,
                                 std::string& error) -> std::optional<int64_t> {
    // Its first run strictly after now, as the scheduler counts it from its
    // last fire and any hold already set: an overdue interval's next time,
    // not the one already passed (held just past that, the hold would be
    // over already).
    Trigger after_now = script.trigger;
    after_now.held_until_unix = std::max<int64_t>(
        after_now.held_until_unix, static_cast<int64_t>(now.ToTimeT()) + 1);
    const base::Time next = AutomationScheduler::NextFireTime(after_now, now);
    if (next.is_null()) {
      error = "No upcoming run to skip";
      return std::nullopt;
    }
    // Held just past it: the run after it is the next.
    return static_cast<int64_t>(next.ToTimeT()) + 1;
  };
  return HoldAnswer(id, now, storage, "run_skipped", past_next_run);
}

}  // namespace automation_page
}  // namespace molt_ai
