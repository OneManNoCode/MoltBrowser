// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_routine_store.h"

#include <algorithm>
#include <set>
#include <utility>

#include "chrome/browser/molt_ai/automation/automation_security.h"
#include "chrome/browser/molt_ai/automation/automation_voice_steps.h"
#include "chrome/browser/molt_ai/voice/voice_routines.h"

namespace molt_ai {
namespace voice {

namespace {

using automation::AutomationSecurity;
using automation::AutomationStorage;
using automation::Script;

// A script id, at most (AutomationStorage's).
constexpr size_t kMaxIdBytes = 128;

// The voice routines in |storage|, each known by its file's name, oldest
// first.
std::vector<Script> VoiceRoutinesIn(AutomationStorage& storage) {
  std::vector<Script> out;
  for (const std::string& id : storage.ListIds()) {
    std::optional<Script> script = storage.Load(id);
    if (script && automation::HasVoiceSteps(*script)) {
      script->id = id;
      out.push_back(std::move(*script));
    }
  }
  std::sort(out.begin(), out.end(), [](const Script& a, const Script& b) {
    return a.created_at_unix != b.created_at_unix
               ? a.created_at_unix < b.created_at_unix
               : a.id < b.id;
  });
  return out;
}

RoutineSummary SummaryOf(const Script& script) {
  RoutineSummary summary;
  summary.id = script.id;
  summary.name = script.name;
  summary.steps = static_cast<int>(script.steps.size());
  summary.needs_sites_confirmed = !RoutineSitesConfirmed(script);
  return summary;
}

std::vector<std::string> NamesOf(const std::vector<Script>& routines) {
  std::vector<std::string> names;
  for (const Script& script : routines) {
    names.push_back(script.name);
  }
  return names;
}

// The routine with id |id| when it isn't empty, else the one routine
// called |name|; null when there is none (or |name| is two routines').
Script* Find(std::vector<Script>& routines,
             const std::string& name,
             const std::string& id) {
  if (!id.empty()) {
    for (Script& script : routines) {
      if (script.id == id) {
        return &script;
      }
    }
    return nullptr;
  }
  const std::vector<std::string> names = NamesOf(routines);
  const RoutineNameMatch match = MatchRoutineName(names, name);
  if (!match.name) {
    return nullptr;
  }
  return &routines[static_cast<size_t>(match.name - names.data())];
}

RoutineSaveResult Refused(std::string said) {
  RoutineSaveResult result;
  result.said = std::move(said);
  return result;
}

}  // namespace

base::FilePath VoiceRoutinesDir(const base::FilePath& profile_path) {
  return profile_path.AppendASCII(kVoiceRoutinesDirName);
}

RoutineToRun::RoutineToRun() = default;
RoutineToRun::~RoutineToRun() = default;
RoutineToRun::RoutineToRun(RoutineToRun&&) = default;
RoutineToRun& RoutineToRun::operator=(RoutineToRun&&) = default;

RoutineSaveResult::RoutineSaveResult() = default;
RoutineSaveResult::~RoutineSaveResult() = default;
RoutineSaveResult::RoutineSaveResult(RoutineSaveResult&&) = default;
RoutineSaveResult& RoutineSaveResult::operator=(RoutineSaveResult&&) = default;

ClaimResult::ClaimResult() = default;
ClaimResult::~ClaimResult() = default;
ClaimResult::ClaimResult(ClaimResult&&) = default;
ClaimResult& ClaimResult::operator=(ClaimResult&&) = default;

VoiceRoutineStore::VoiceRoutineStore(const base::FilePath& routines_dir,
                                     const base::FilePath& shared_root)
    : mine_(routines_dir) {
  if (!shared_root.empty()) {
    shared_.SetRootForTesting(shared_root);
  }
}

VoiceRoutineStore::~VoiceRoutineStore() = default;

std::vector<Script> VoiceRoutineStore::Routines() {
  return VoiceRoutinesIn(mine_);
}

std::vector<RoutineSummary> VoiceRoutineStore::Summaries() {
  std::vector<RoutineSummary> out;
  for (const Script& script : Routines()) {
    out.push_back(SummaryOf(script));
  }
  return out;
}

std::vector<RoutineSummary> VoiceRoutineStore::Unclaimed() {
  std::vector<RoutineSummary> out;
  for (const Script& script : VoiceRoutinesIn(shared_)) {
    out.push_back(SummaryOf(script));
  }
  return out;
}

RoutineSaveResult VoiceRoutineStore::Save(const std::string& id,
                                          const std::string& name,
                                          const std::vector<RoutineStep>& steps,
                                          bool replace,
                                          int64_t now_unix) {
  std::vector<Script> routines = Routines();
  const std::vector<std::string> names = NamesOf(routines);
  const RoutineNameMatch in_use = MatchRoutineName(names, name);
  if (in_use.count > 1) {
    return Refused("You have " + std::to_string(in_use.count) +
                   " routines called " + name +
                   ". Choose another name, or rename them in Agent mode.");
  }
  std::string save_id = id;
  if (in_use.name) {
    if (!replace) {
      return Refused("You already have a routine called " + name + ".");
    }
    // Replaced where it is: the same id, so the studio's card and the
    // audit log follow it.
    save_id = routines[static_cast<size_t>(in_use.name - names.data())].id;
  }
  BuiltRoutine built = BuildRoutine(save_id, name, steps, now_unix);
  if (!built.script) {
    return Refused(built.refusal);
  }
  if (!mine_.Save(*built.script)) {
    return Refused("I couldn't save it.");
  }
  const bool draft = built.script->security.sites_unconfirmed;
  mine_.AppendAudit(
      save_id, in_use.name ? "voice_routine_replaced" : "voice_routine_saved",
      draft ? "draft: the planner's steps" : "");
  if (built.script->security.trust == automation::TrustLevel::APPROVED) {
    mine_.AppendAudit(save_id, "trust_changed",
                      "casual -> approved (taught by voice)");
  }
  RoutineSaveResult result;
  result.saved = true;
  result.replaced = in_use.name != nullptr;
  if (draft) {
    result.sites = AutomationSecurity::HostsReferencedBy(built.script->steps);
  }
  return result;
}

std::optional<RoutineToRun> VoiceRoutineStore::Load(const std::string& name,
                                                    const std::string& id) {
  std::vector<Script> routines = Routines();
  Script* script = Find(routines, name, id);
  if (!script) {
    return std::nullopt;
  }
  // Its steps use other sites than the ones confirmed (a step added or
  // changed in the file): a draft again, confirmed again before it runs.
  if (UnconfirmChangedRoutine(*script) && mine_.Save(*script)) {
    mine_.AppendAudit(script->id, "draft_sites_unconfirmed",
                      "its steps use other sites");
  }
  RoutineToRun run;
  run.id = script->id;
  run.name = script->name;
  ReadRoutineResult read = ReadRoutine(*script);
  run.ok = read.ok;
  run.why = read.why;
  run.steps = std::move(read.steps);
  run.needs_sites_confirmed = read.needs_sites_confirmed;
  run.policy = AutomationSecurity::EffectivePolicy(*script);
  run.sites = AutomationSecurity::HostsReferencedBy(script->steps);
  return run;
}

bool VoiceRoutineStore::Delete(const std::string& name, const std::string& id) {
  std::vector<Script> routines = Routines();
  const Script* script = Find(routines, name, id);
  if (!script) {
    return false;
  }
  const std::string found = script->id;
  if (!mine_.Delete(found)) {
    return false;
  }
  mine_.AppendAudit(found, "voice_routine_deleted", "");
  return true;
}

bool VoiceRoutineStore::ConfirmSites(const std::string& id,
                                     const std::vector<std::string>& sites) {
  std::vector<Script> routines = Routines();
  Script* script = Find(routines, std::string(), id);
  if (!script || !AutomationSecurity::ConfirmDraftSites(*script, sites) ||
      !mine_.Save(*script)) {
    return false;
  }
  std::string list;
  for (const std::string& host : script->security.domain_whitelist) {
    list += (list.empty() ? "" : " ") + host;
  }
  mine_.AppendAudit(id, "draft_sites_confirmed", list + " (by voice)");
  return true;
}

void VoiceRoutineStore::DeleteAll() {
  for (const Script& script : Routines()) {
    if (mine_.Delete(script.id)) {
      mine_.AppendAudit(script.id, "voice_routine_deleted",
                        "forget everything");
    }
  }
}

ClaimResult VoiceRoutineStore::Claim(const std::vector<std::string>& ids) {
  ClaimResult result;
  std::vector<std::string> names = NamesOf(Routines());
  const std::vector<std::string> listed = mine_.ListIds();
  std::set<std::string> taken_ids(listed.begin(), listed.end());
  for (const std::string& id : ids) {
    // Moved aside first: another profile claiming it now finds it gone.
    std::optional<Script> taken = shared_.Take(id);
    if (!taken) {
      ++result.not_kept;
      continue;
    }
    if (!automation::HasVoiceSteps(*taken)) {
      // An ordinary workflow: every profile's, never one's own.
      shared_.ReturnTaken(id);
      ++result.not_kept;
      continue;
    }
    Script script = std::move(*taken);
    // Kept as it was: its steps, trust and sites (claiming changes none).
    // Only its name, beside this profile's, and its id if this profile has
    // one of it.
    ClaimedRoutine kept;
    const std::string name = UniqueRoutineName(names, script.name);
    while (taken_ids.count(script.id) && script.id.size() < kMaxIdBytes) {
      script.id += '_';
    }
    if (name.empty() || taken_ids.count(script.id)) {
      shared_.ReturnTaken(id);
      ++result.not_kept;
      continue;
    }
    if (name != script.name) {
      kept.renamed_from = script.name;
      script.name = name;
    }
    if (!mine_.Save(script)) {
      shared_.ReturnTaken(id);
      ++result.not_kept;
      continue;
    }
    shared_.FinishTake(id);
    mine_.AppendAudit(script.id, "voice_routine_claimed",
                      kept.renamed_from.empty()
                          ? std::string("from before routines belonged to a "
                                        "profile")
                          : "from before routines belonged to a profile, "
                            "renamed");
    names.push_back(script.name);
    taken_ids.insert(script.id);
    kept.id = script.id;
    kept.name = script.name;
    result.kept.push_back(std::move(kept));
  }
  return result;
}

std::string VoiceRoutineStore::NameFor(const Script& script) {
  std::vector<std::string> names;
  for (const Script& routine : Routines()) {
    if (routine.id != script.id) {
      names.push_back(routine.name);
    }
  }
  return UniqueRoutineName(names, script.name);
}

}  // namespace voice
}  // namespace molt_ai
