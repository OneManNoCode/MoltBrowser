// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/webui/molt_ai/molt_ai_agent_storage.h"

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/time/time.h"
#include "chrome/browser/molt_ai/automation/automation_security.h"
#include "chrome/browser/molt_ai/automation/automation_storage.h"
#include "chrome/browser/molt_ai/automation/automation_voice_steps.h"

namespace molt_ai {
namespace agent_studio {

namespace {

using automation::AutomationSecurity;
using automation::AutomationStorage;
using automation::HasVoiceSteps;
using automation::Script;
using automation::TrustLevel;
using voice::VoiceRoutineStore;

constexpr char kMadeBeforeProfiles[] =
    "This routine was made before routines belonged to a profile. Keep it "
    "here first.";
constexpr char kVoiceStepsNotHere[] =
    "Steps taught by voice can't be added to this workflow.";
constexpr char kKeepsVoiceSteps[] =
    "A routine taught by voice keeps its voice steps.";
constexpr char kNoRoutinesHere[] =
    "Routines taught by voice can't be kept in this window.";
constexpr char kNotFound[] = "Workflow not found";
constexpr char kReviewChangedFirst[] =
    "Review this workflow and confirm its sites first.";
constexpr char kChangedSinceReview[] =
    "This workflow changed since you reviewed it. Review it again.";
constexpr char kGoneSinceOpened[] =
    "This workflow was deleted or changed since you opened it. Open it "
    "again.";
constexpr char kNoLongerADraft[] =
    "This workflow was confirmed or changed since you opened it. Open it "
    "again.";

// The longest id a workflow's file can have (AutomationStorage).
constexpr size_t kMaxIdBytes = 128;

// The profile's routines' store; null off the record.
std::unique_ptr<VoiceRoutineStore> RoutinesStore(const StudioPlaces& places) {
  if (places.routines_dir.empty()) {
    return nullptr;
  }
  return std::make_unique<VoiceRoutineStore>(places.routines_dir,
                                             places.shared_root);
}

Answer Refused(const std::string& error) {
  Answer answer;
  answer.out.Set("error", error);
  return answer;
}

Answer NotOk(const std::string& error = std::string()) {
  Answer answer;
  answer.out.Set("ok", false);
  if (!error.empty()) {
    answer.out.Set("error", error);
  }
  return answer;
}

// getRuns: the workflow's runs, each with its per-step timeline.
base::DictValue RunsDict(const std::string& id, const Script* script) {
  base::ListValue runs;
  if (script) {
    for (const auto& r : script->stats.run_history) {
      base::DictValue rd;
      rd.Set("ts", static_cast<double>(r.timestamp_unix));
      rd.Set("ok", r.success);
      rd.Set("durationMs", r.duration_ms);
      rd.Set("stepsExecuted", r.steps_executed);
      rd.Set("totalSteps", r.total_steps);
      rd.Set("message", r.message);
      int failed = -1;
      base::ListValue steps;
      for (const auto& sr : r.steps) {
        base::DictValue sd;
        sd.Set("index", sr.index);
        sd.Set("type", automation::StepTypeToString(sr.type));
        sd.Set("description", sr.description);
        sd.Set("ok", sr.ok);
        sd.Set("durationMs", sr.duration_ms);
        if (!sr.note.empty()) {
          sd.Set("note", sr.note);
        }
        if (!sr.found_by.empty()) {
          sd.Set("foundBy", sr.found_by);
        }
        if (!sr.screenshot.empty()) {
          sd.Set("shot", sr.screenshot);
        }
        if (!sr.ok && failed < 0) {
          failed = sr.index;
        }
        steps.Append(std::move(sd));
      }
      rd.Set("failedStepIndex", failed);
      rd.Set("steps", std::move(steps));
      runs.Append(std::move(rd));
    }
  }
  base::DictValue out;
  out.Set("id", id);
  out.Set("runs", std::move(runs));
  return out;
}

// What the page gets back from a save: the id, the name (made unique for a
// routine), the trust and, for a draft, the sites its steps now target.
base::DictValue SavedDict(const Script& script) {
  base::DictValue out;
  out.Set("id", script.id);
  out.Set("name", script.name);
  out.Set("trust", TrustLevelToString(script.security.trust));
  // Still a draft (saving never confirms one): the sites its steps now
  // target, for the review panel, whether it may be confirmed with none
  // (the browser decides, never the page), and the token of what was saved
  // (the stored file, as it will be read back).
  if (script.security.sites_unconfirmed) {
    out.Set("draftSites", DraftSites(script));
    out.Set("confirmWithoutSites",
            AutomationSecurity::MayConfirmWithoutSites(script));
    out.Set("reviewToken",
            automation::AutomationSecurity::DigestAsStored(script));
  }
  return out;
}

// The studio never edits the security policy, so whatever the page's dict
// carries is ignored: an existing workflow keeps the policy stored on disk
// (trust only changes with promoteTrust); a new one gets the default
// CASUAL one, or APPROVED when the user just recorded it here. Nor does
// its editor edit the schedule (only the Schedule sheet does: kSchedule):
// an existing workflow keeps its stored schedule and whether it is on, so
// an editor opened before a schedule was saved, turned on or off, doesn't
// undo it; a new one's schedule (an import's) is turned off
// (ClampTrustForSave()).
void KeepPolicy(Script& script, const Script* existing, bool recorded_here) {
  if (existing) {
    script.security = existing->security;
    script.trigger = existing->trigger;
    script.enabled = existing->enabled;
  } else {
    script.security = automation::SecurityPolicy();
  }
  AutomationSecurity::ClampTrustForSave(script, existing);
  if (recorded_here && !existing) {
    // The user just performed these steps themselves, on these sites;
    // replaying them — including the form they submitted or the saved
    // password they used — is the point of recording. APPROVED allows
    // exactly that and no more: actions stay on the sites the steps
    // target, other sites / RUN_JS / headless runs still need Trusted.
    // Imports and duplicates never get here and start CASUAL.
    script.security.trust = TrustLevel::APPROVED;
  }
}

// |request| on its workflow |found|, in |storage| (the profile's routines'
// when |routines|).
Answer OnFound(const WorkflowRequest& request,
               AutomationStorage& storage,
               VoiceRoutineStore* routines,
               Script& found) {
  using Kind = WorkflowRequest::Kind;
  const std::string& id = request.id;
  Answer answer;
  switch (request.kind) {
    case Kind::kGet: {
      answer.out = found.ToJSON();
      // The UI reads {{variable}} defaults under "variables"; expose an
      // alias so it round-trips (the Script model stores them under
      // "default_variables").
      if (const base::DictValue* dv =
              answer.out.FindDict("default_variables")) {
        answer.out.Set("variables", dv->Clone());
      }
      // A draft (from an agent run, or a file changed outside the browser:
      // security.changed_outside says which): the sites to confirm,
      // whether it may be confirmed with none (a change whose steps target
      // no site), and the token of what is shown.
      if (found.security.sites_unconfirmed) {
        answer.out.Set("draftSites", DraftSites(found));
        answer.out.Set("confirmWithoutSites",
                       AutomationSecurity::MayConfirmWithoutSites(found));
        answer.out.Set("reviewToken", ReviewToken(found));
      }
      return answer;
    }
    case Kind::kRuns:
      answer.out = RunsDict(id, &found);
      return answer;
    case Kind::kDelete: {
      const bool ok = storage.Delete(id);
      if (ok && routines) {
        storage.AppendAudit(id, "voice_routine_deleted", "in Agent mode");
        answer.routines_changed = true;
      }
      answer.out.Set("ok", ok);
      return answer;
    }
    case Kind::kDuplicate: {
      Script& copy = found;
      copy.id = NewWorkflowId();
      while (storage.Load(copy.id)) {
        copy.id += "_";
      }
      copy.name = request.name;
      copy.created_at_unix = static_cast<int64_t>(std::time(nullptr));
      copy.stats = automation::Stats();      // fresh history
      copy.trigger = automation::Trigger();  // copy starts manual
      // A copy is a new workflow: it earns trust on its own.
      AutomationSecurity::ClampTrustForSave(copy, /*existing=*/nullptr);
      // A routine's copy is the profile's too, under a name of its own.
      if (routines) {
        copy.name = routines->NameFor(copy);
      }
      if (!copy.name.empty() && storage.Save(copy)) {
        answer.routines_changed = routines != nullptr;
        answer.out.Set("id", copy.id);
        answer.out.Set("name", copy.name);
      }
      return answer;
    }
    case Kind::kCheckRun:
      // An unconfirmed draft doesn't run (the runner refuses it too).
      if (found.security.sites_unconfirmed) {
        return NotOk(AutomationSecurity::ReviewMessage(found));
      }
      // Nor here a routine taught by voice: voice mode runs it.
      if (HasVoiceSteps(found)) {
        return NotOk(automation::kVoiceRoutineMessage);
      }
      answer.out.Set("ok", true);
      answer.script = std::move(found);
      return answer;
    case Kind::kSchedule: {
      // An unconfirmed draft is never scheduled, nor a routine taught by
      // voice.
      if (found.security.sites_unconfirmed) {
        return NotOk(AutomationSecurity::ReviewMessage(found));
      }
      if (HasVoiceSteps(found)) {
        return NotOk(automation::kVoiceRoutineMessage);
      }
      const char* draft = nullptr;
      automation::Trigger trigger;
      const bool saved = storage.Update(id, [&](Script& stored) {
        if (stored.security.sites_unconfirmed) {
          draft = AutomationSecurity::ReviewMessage(stored);
          return false;
        }
        if (HasVoiceSteps(stored)) {
          return false;
        }
        const automation::Trigger before = stored.trigger;
        stored.trigger = request.trigger;
        automation::CountIntervalFromNow(stored.trigger,
                                         base::Time::Now().ToTimeT());
        stored.enabled =
            request.trigger.type != automation::TriggerType::MANUAL;
        trigger = stored.trigger;
        // The same schedule keeps its hold (a Snooze or a Skip made in
        // molt://ai-automation: AutomationStorage::Update()), which the
        // answer carries, so the next run shown honours it; a new one ends
        // it.
        if (!stored.security.changed_outside &&
            automation::SameSchedule(before, trigger)) {
          trigger.held_until_unix = before.held_until_unix;
        }
        return true;
      });
      if (draft) {
        return NotOk(draft);
      }
      answer.out.Set("ok", saved);
      if (saved) {
        // As saved (an interval's count started), for the editor's next
        // save: the page's own shape of it isn't a trigger.
        answer.out.Set("trigger", automation::TriggerToDict(trigger));
        answer.trigger = trigger;
      }
      return answer;
    }
    case Kind::kTrust:
      if (found.security.sites_unconfirmed) {
        // A draft from a web agent run, or a file changed outside the
        // browser, is trusted only after its sites are confirmed
        // (confirmDraftSites).
        return NotOk(found.security.changed_outside
                         ? kReviewChangedFirst
                         : "Confirm this draft's sites first.");
      }
      if ((request.level != "approved" && request.level != "casual") ||
          found.security.trust > TrustLevel::APPROVED) {
        return NotOk("Manage this workflow's trust in molt://ai-automation");
      }
      if (!storage.SetTrustLevel(
              id, automation::TrustLevelFromString(request.level))) {
        return NotOk();
      }
      answer.out.Set("ok", true);
      answer.out.Set("trust", request.level);
      return answer;
    case Kind::kConfirmSites: {
      bool changed_since_review = false;
      bool steps_changed = false;
      bool was_changed_outside = false;
      std::string sites;
      std::string trust;
      automation::Trigger trigger;
      const bool saved = storage.Update(id, [&](Script& stored) {
        // Exactly what the user reviewed: a file changed since (outside
        // the browser, or saved again) isn't confirmed blindly.
        if (request.review_token.empty() ||
            ReviewToken(stored) != request.review_token) {
          changed_since_review = true;
          return false;
        }
        was_changed_outside = stored.security.changed_outside;
        if (!AutomationSecurity::ConfirmDraftSites(stored, request.sites)) {
          steps_changed = true;
          return false;
        }
        // A file changed outside starts its schedule over, as one saved
        // afresh: counted from the fire this browser recorded before the
        // change, its next time may be long past (a run at once).
        if (was_changed_outside) {
          stored.trigger.last_fired_unix = 0;
        }
        automation::CountIntervalFromNow(stored.trigger,
                                         base::Time::Now().ToTimeT());
        for (const std::string& host : stored.security.domain_whitelist) {
          sites += (sites.empty() ? "" : " ") + host;
        }
        trust = TrustLevelToString(stored.security.trust);
        trigger = stored.trigger;
        return true;
      });
      if (changed_since_review) {
        return NotOk(kChangedSinceReview);
      }
      if (steps_changed) {
        return NotOk(
            "The draft's steps changed. Save them, then confirm again.");
      }
      if (!saved) {
        return NotOk();
      }
      // A change confirmed with no site (its steps target none) says so.
      storage.AppendAudit(id,
                          was_changed_outside ? "outside_change_confirmed"
                                              : "draft_sites_confirmed",
                          sites.empty() ? std::string("no_sites") : sites);
      answer.routines_changed = routines != nullptr;
      answer.out.Set("ok", true);
      answer.out.Set("trust", trust);
      // As saved (started over, for a file changed outside), for the
      // editor's next save: the one it opened would write back the fire
      // from before the change.
      answer.out.Set("trigger", automation::TriggerToDict(trigger));
      return answer;
    }
  }
  return answer;
}

// The message's answer when its workflow is nowhere.
Answer NotFound(const WorkflowRequest& request) {
  using Kind = WorkflowRequest::Kind;
  switch (request.kind) {
    case Kind::kGet:
    case Kind::kDuplicate:
      return Answer();
    case Kind::kRuns: {
      Answer answer;
      answer.out = RunsDict(request.id, nullptr);
      return answer;
    }
    case Kind::kDelete:
    case Kind::kCheckRun:
    case Kind::kSchedule:
      return NotOk();
    case Kind::kTrust:
    case Kind::kConfirmSites:
      return NotOk(kNotFound);
  }
  return Answer();
}

// A new routine (brought in: an import, a blank one), its id made for it:
// whatever either storage holds under that id is another workflow, never
// this one's policy nor written over. It is saved under the id, or it with
// "_" added, that neither the profile's routines nor the shared storage
// have (the studio looks in the profile's routines first, so a routine of
// a shared workflow's id would hide it), with SaveNew() (never over a
// file), and with the default policy: a draft (ClampTrustForSave()).
Answer SaveNewRoutine(const StudioPlaces& places,
                      VoiceRoutineStore& routines,
                      Script script) {
  std::optional<AutomationStorage> shared;
  if (!places.shared_dir.empty()) {
    shared.emplace(places.shared_dir);
  }
  const std::vector<std::string> listed = routines.storage().ListIds();
  const std::set<std::string> mine(listed.begin(), listed.end());
  while (mine.count(script.id) || (shared && shared->Load(script.id))) {
    if (script.id.size() >= kMaxIdBytes) {
      return Answer();
    }
    script.id += "_";
  }
  KeepPolicy(script, /*existing=*/nullptr, /*recorded_here=*/false);
  // Its name, made its own beside every routine of the profile (none has
  // its id).
  script.name = routines.NameFor(script);
  if (script.name.empty()) {
    return Refused("Too many routines have that name. Choose another.");
  }
  base::DictValue saved = SavedDict(script);
  const std::optional<std::string> id =
      routines.storage().SaveNew(std::move(script));
  if (!id) {
    return Answer();
  }
  saved.Set("id", *id);
  Answer answer;
  answer.out = std::move(saved);
  answer.routines_changed = true;
  return answer;
}

}  // namespace

Answer::Answer() = default;
Answer::~Answer() = default;
Answer::Answer(Answer&&) = default;
Answer& Answer::operator=(Answer&&) = default;

WorkflowRequest::WorkflowRequest() = default;
WorkflowRequest::~WorkflowRequest() = default;
WorkflowRequest::WorkflowRequest(const WorkflowRequest&) = default;
WorkflowRequest& WorkflowRequest::operator=(const WorkflowRequest&) = default;

SaveRequest::SaveRequest() = default;
SaveRequest::~SaveRequest() = default;
SaveRequest::SaveRequest(SaveRequest&&) = default;
SaveRequest& SaveRequest::operator=(SaveRequest&&) = default;

RoutinesListing::RoutinesListing() = default;
RoutinesListing::~RoutinesListing() = default;
RoutinesListing::RoutinesListing(RoutinesListing&&) = default;
RoutinesListing& RoutinesListing::operator=(RoutinesListing&&) = default;

std::optional<Answer> AnswerInRoutines(const StudioPlaces& places,
                                       const WorkflowRequest& request) {
  std::unique_ptr<VoiceRoutineStore> routines = RoutinesStore(places);
  if (!routines) {
    return std::nullopt;
  }
  std::optional<Script> found = routines->storage().Load(request.id);
  if (!found || !HasVoiceSteps(*found)) {
    return std::nullopt;
  }
  return OnFound(request, routines->storage(), routines.get(), *found);
}

Answer AnswerInSharedStorage(const StudioPlaces& places,
                             const WorkflowRequest& request) {
  if (places.shared_dir.empty()) {
    return NotFound(request);
  }
  AutomationStorage shared(places.shared_dir);
  std::optional<Script> found = shared.Load(request.id);
  // A voice routine here is one from before profiles: kept here first.
  if (!found || HasVoiceSteps(*found)) {
    return NotFound(request);
  }
  return OnFound(request, shared, nullptr, *found);
}

Holds RoutinesHold(const StudioPlaces& places, const std::string& id) {
  std::unique_ptr<VoiceRoutineStore> routines = RoutinesStore(places);
  if (!routines) {
    return Holds::kNothing;
  }
  std::optional<Script> found = routines->storage().Load(id);
  return found && HasVoiceSteps(*found) ? Holds::kRoutine : Holds::kNothing;
}

Holds SharedStorageHolds(const StudioPlaces& places, const std::string& id) {
  if (places.shared_dir.empty()) {
    return Holds::kNothing;
  }
  AutomationStorage shared(places.shared_dir);
  std::optional<Script> found = shared.Load(id);
  if (!found) {
    return Holds::kNothing;
  }
  return HasVoiceSteps(*found) ? Holds::kRoutine : Holds::kOrdinary;
}

Answer SaveRoutine(const StudioPlaces& places,
                   SaveRequest request,
                   Holds in_shared_storage) {
  std::unique_ptr<VoiceRoutineStore> routines = RoutinesStore(places);
  if (!routines) {
    return Refused(kNoRoutinesHere);
  }
  Script& script = request.script;
  if (request.new_workflow) {
    return SaveNewRoutine(places, *routines, std::move(script));
  }
  std::optional<Script> existing = routines->storage().Load(script.id);
  if (existing && !HasVoiceSteps(*existing)) {
    existing.reset();
  }
  if (!existing && in_shared_storage == Holds::kRoutine) {
    return Refused(kMadeBeforeProfiles);
  }
  if (!existing && in_shared_storage == Holds::kOrdinary) {
    return Refused(kVoiceStepsNotHere);
  }
  // An edit of a routine gone or unreadable since the page read it
  // (deleted in another view or by voice): saved as a new routine, a draft
  // the page was shown (one changed outside the browser, which voice mode
  // never confirms on its sites alone) would no longer be one.
  if (!existing) {
    return Refused(kGoneSinceOpened);
  }
  // The page edited a draft that isn't one any more (its sites confirmed
  // meanwhile, by voice or in another view): saved under that confirmation,
  // steps it never covered would run.
  if (request.edited_draft && !existing->security.sites_unconfirmed) {
    return Refused(kNoLongerADraft);
  }
  KeepPolicy(script, &*existing, /*recorded_here=*/false);
  // A routine's name, made its own beside the profile's others ("my news
  // (2)"): voice mode runs exactly one by each name. On this sequence, so
  // voice mode can't save one of that name in between.
  script.name = routines->NameFor(script);
  if (script.name.empty()) {
    return Refused("Too many routines have that name. Choose another.");
  }
  if (!routines->storage().Save(script)) {
    return Answer();
  }
  Answer answer;
  answer.out = SavedDict(script);
  answer.routines_changed = true;
  return answer;
}

Answer SaveSharedWorkflow(const StudioPlaces& places,
                          SaveRequest request,
                          Holds in_routines) {
  Script& script = request.script;
  const bool voice_steps = HasVoiceSteps(script);
  if (in_routines == Holds::kRoutine) {
    // One of the profile's routines: it keeps its voice steps (a save with
    // them goes to SaveRoutine()).
    return voice_steps ? Answer() : Refused(kKeepsVoiceSteps);
  }
  std::optional<AutomationStorage> shared;
  std::optional<Script> existing;
  if (!places.shared_dir.empty()) {
    shared.emplace(places.shared_dir);
    // A new workflow's id was made for it: whatever is stored under it is
    // another workflow (its save picks a free id).
    if (!request.new_workflow) {
      existing = shared->Load(script.id);
    }
  }
  if (existing && HasVoiceSteps(*existing)) {
    return Refused(kMadeBeforeProfiles);
  }
  if (voice_steps) {
    // No routines here (off the record), or an ordinary workflow of that
    // id.
    return Refused(existing ? kVoiceStepsNotHere : kNoRoutinesHere);
  }
  if (!shared) {
    return Answer();
  }
  Answer answer;
  if (request.new_workflow) {
    KeepPolicy(script, nullptr, request.recorded_here);
    // Never fired here: its schedule starts over, whatever last fire the
    // page sent (an import's, from its file: an earlier one would run it at
    // once, a later one hold it back), an interval counted from now.
    script.trigger.last_fired_unix = 0;
    automation::CountIntervalFromNow(script.trigger,
                                     base::Time::Now().ToTimeT());
    base::DictValue saved = SavedDict(script);
    // Never over another file, whatever is there by now.
    const std::optional<std::string> id = shared->SaveNew(std::move(script));
    if (id) {
      if (request.recorded_here) {
        shared->AppendAudit(*id, "trust_changed",
                            "casual -> approved (recorded in Agent studio)");
      }
      saved.Set("id", *id);
      answer.out = std::move(saved);
    }
    return answer;
  }
  // An edit of a workflow the page read: the policy as stored when it is
  // written, decided under the directory's lock (the runner and
  // molt://ai-automation change it on the UI thread). Gone or unreadable by
  // then (deleted, or changed outside the browser), it is refused: saved as
  // a new workflow, what the page was shown of a draft (a file changed
  // outside) would no longer be one.
  // Nor is the edit of a draft saved over a workflow that is no longer one
  // (confirmed meanwhile, in another view, or a file put there since): it
  // would take on that confirmation, its sites and its trust, for steps
  // and a schedule it never covered.
  const bool edited_draft = request.edited_draft;
  bool gone = false;
  bool no_longer_a_draft = false;
  bool now_a_routine = false;
  base::DictValue saved;
  if (!shared->SaveChecked(std::move(script),
                           [&](const Script* stored, Script& edit) {
                             if (!stored) {
                               gone = true;
                               return false;
                             }
                             if (HasVoiceSteps(*stored)) {
                               now_a_routine = true;
                               return false;
                             }
                             if (edited_draft &&
                                 !stored->security.sites_unconfirmed) {
                               no_longer_a_draft = true;
                               return false;
                             }
                             KeepPolicy(edit, stored,
                                        /*recorded_here=*/false);
                             saved = SavedDict(edit);
                             return true;
                           })) {
    if (gone) {
      return Refused(kGoneSinceOpened);
    }
    if (no_longer_a_draft) {
      return Refused(kNoLongerADraft);
    }
    return now_a_routine ? Refused(kMadeBeforeProfiles) : Answer();
  }
  answer.out = std::move(saved);
  return answer;
}

RoutinesListing ListRoutines(const StudioPlaces& places) {
  RoutinesListing listing;
  std::unique_ptr<VoiceRoutineStore> routines = RoutinesStore(places);
  if (routines) {
    listing.routines = routines->Routines();
    listing.unclaimed = routines->Unclaimed();
  }
  return listing;
}

std::vector<Script> ListSharedWorkflows(const StudioPlaces& places) {
  std::vector<Script> out;
  if (places.shared_dir.empty()) {
    return out;
  }
  AutomationStorage shared(places.shared_dir);
  for (Script& script : shared.ListAll()) {
    // Voice routines in the shared storage are listed apart (the routines
    // from before profiles).
    if (!HasVoiceSteps(script)) {
      out.push_back(std::move(script));
    }
  }
  return out;
}

std::optional<Script> LoadSharedWorkflow(const StudioPlaces& places,
                                         const std::string& id) {
  if (places.shared_dir.empty()) {
    return std::nullopt;
  }
  AutomationStorage shared(places.shared_dir);
  return shared.Load(id);
}

SaveKind KindOfSave(const std::string& sent_id,
                    const std::string& recorded_id,
                    const std::set<std::string>& minted) {
  SaveKind kind;
  if (!sent_id.empty()) {
    kind.id = sent_id;
    return kind;
  }
  kind.new_workflow = true;
  if (!recorded_id.empty() && minted.count(recorded_id)) {
    kind.id = recorded_id;
    kind.recorded_here = true;
  }
  return kind;
}

std::string UnusedWorkflowId(const StudioPlaces& places,
                             std::string id,
                             const std::set<std::string>& minted) {
  std::optional<AutomationStorage> shared;
  if (!places.shared_dir.empty()) {
    shared.emplace(places.shared_dir);
  }
  while (minted.count(id) || (shared && shared->Load(id))) {
    id += "_";
  }
  return id;
}

base::ListValue DraftSites(const Script& script) {
  base::ListValue sites;
  for (std::string& host :
       AutomationSecurity::HostsReferencedBy(script.steps)) {
    sites.Append(std::move(host));
  }
  return sites;
}

std::string ReviewToken(const Script& script) {
  return AutomationSecurity::SecurityDigest(script);
}

std::string NewWorkflowId() {
  // The time, and a random part: two made in one second (two views, two
  // saves; a clock set back) are never alike.
  uint64_t bits = base::RandUint64();
  std::string random;
  for (int i = 0; i < 8; ++i) {
    random += "0123456789abcdef"[bits & 0xf];
    bits >>= 4;
  }
  return "wf_" +
         base::NumberToString(static_cast<int64_t>(std::time(nullptr))) + "_" +
         random;
}

}  // namespace agent_studio
}  // namespace molt_ai
