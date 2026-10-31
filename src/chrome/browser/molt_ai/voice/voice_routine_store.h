// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// A profile's voice routines on disk (docs/design/TOUCHLESS_BROWSING.md
// §6.8). Each profile keeps its own, as Agent-mode workflows (.molt,
// voice_routines.h) in an AutomationStorage of its own in the profile
// directory: <profile>/MoltVoiceRoutines/<id>.molt, with its audit.log.
// So no other profile lists, runs, confirms, edits or deletes them (by
// voice, in the Agent studio or in molt://ai-automation, which shows the
// shared storage only), they are never synced, and they go with the
// profile. Ordinary workflows stay in the shared ~/.moltbrowser/automations/.
//
// Routines from before routines belonged to a profile (rounds 41 and 42)
// are the voice routines (automation::HasVoiceSteps()) still in the shared
// storage: "unclaimed". A profile keeps them only when its user says so,
// by voice or with a click in the Agent studio (Claim()): each is moved
// once (AutomationStorage::Take(): of two profiles claiming it at once,
// one gets it), as it was (its steps, trust and sites), renamed when the
// profile already has a routine of its name. Nothing gives one profile's
// routines to another; a profile whose user said they aren't theirs isn't
// asked again (LearnedData::declined_routines, voice_learning_store.h).
// Where a routine came from isn't read from the file: a file's contents
// never make it any profile's, only where it is.
//
// Names (RoutineNameKey()): a routine taught by voice under a name the
// profile uses replaces that routine (its id kept) only when the user said
// so; a routine brought in under a name in use (claimed, imported, copied)
// is renamed "my news (2)" (UniqueRoutineName()). A name two routines
// answer to (their files edited) runs neither by voice.
//
// Blocking (file I/O, through AutomationStorage): VoiceLearning calls it
// on its sequence, the Agent studio on the UI thread as it does the shared
// storage. base::Value and base::FilePath only: molt_ai_unittests tests it
// on a temporary directory.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_ROUTINE_STORE_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_ROUTINE_STORE_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/automation/automation_storage.h"
#include "chrome/browser/molt_ai/voice/voice_routines_logic.h"

namespace molt_ai {
namespace voice {

// The directory in the profile that holds its voice routines.
inline constexpr char kVoiceRoutinesDirName[] = "MoltVoiceRoutines";

// |profile_path|'s voice routines' directory.
base::FilePath VoiceRoutinesDir(const base::FilePath& profile_path);

// A routine, as voice mode, settings and the Agent studio list it.
struct RoutineSummary {
  std::string id;
  std::string name;
  int steps = 0;
  // Its sites aren't confirmed yet (it has the planner's steps, or came
  // from elsewhere).
  bool needs_sites_confirmed = false;
};

// A routine read to be run.
struct RoutineToRun {
  RoutineToRun();
  ~RoutineToRun();
  RoutineToRun(RoutineToRun&&);
  RoutineToRun& operator=(RoutineToRun&&);

  std::string id;
  std::string name;
  // ReadRoutine()'s result: false (with |why|) for a workflow voice mode
  // can't run.
  bool ok = false;
  std::string why;
  std::vector<RoutineStep> steps;
  bool needs_sites_confirmed = false;
  // AutomationSecurity::EffectivePolicy() of the stored workflow: the
  // sites it may act on.
  automation::SecurityPolicy policy;
  // The sites to confirm (HostsReferencedBy()).
  std::vector<std::string> sites;
};

struct RoutineSaveResult {
  RoutineSaveResult();
  ~RoutineSaveResult();
  RoutineSaveResult(RoutineSaveResult&&);
  RoutineSaveResult& operator=(RoutineSaveResult&&);

  bool saved = false;
  // It replaced the routine of its name.
  bool replaced = false;
  // Why it wasn't saved (the browser's words).
  std::string said;
  // A routine with the planner's steps: the sites it uses, which the user
  // confirms before it runs.
  std::vector<std::string> sites;
};

// A routine a profile kept from before routines belonged to one.
struct ClaimedRoutine {
  std::string id;
  std::string name;
  // Its name before, when it had to change (the profile had one of it).
  std::string renamed_from;
};

struct ClaimResult {
  ClaimResult();
  ~ClaimResult();
  ClaimResult(ClaimResult&&);
  ClaimResult& operator=(ClaimResult&&);

  std::vector<ClaimedRoutine> kept;
  // Not kept: gone (another profile kept it first), not a voice routine,
  // unreadable, or not saved here (it stays where it was).
  int not_kept = 0;
};

class VoiceRoutineStore {
 public:
  // |routines_dir|: the profile's (VoiceRoutinesDir()). |shared_root|: the
  // shared storage's root, empty for ~/.moltbrowser (tests give one).
  VoiceRoutineStore(const base::FilePath& routines_dir,
                    const base::FilePath& shared_root);
  VoiceRoutineStore(const VoiceRoutineStore&) = delete;
  VoiceRoutineStore& operator=(const VoiceRoutineStore&) = delete;
  ~VoiceRoutineStore();

  // The profile's storage (the Agent studio edits a routine through it).
  automation::AutomationStorage& storage() { return mine_; }

  // The profile's voice routines, each known by its file's name; anything
  // else in the directory is left alone.
  std::vector<automation::Script> Routines();
  std::vector<RoutineSummary> Summaries();
  // The routines from before routines belonged to a profile.
  std::vector<RoutineSummary> Unclaimed();

  // Saves |steps| as the routine |name| (RoutineDisplayName()), with id
  // |id|, at |now_unix|. When the profile has a routine of that name, it is
  // replaced (its id kept) if |replace|, else nothing is saved; a name two
  // routines answer to saves nothing.
  RoutineSaveResult Save(const std::string& id,
                         const std::string& name,
                         const std::vector<RoutineStep>& steps,
                         bool replace,
                         int64_t now_unix);
  // The routine called |name| (exactly one), or with id |id| when |name|
  // is empty; nullopt when there is none. One whose steps no longer use
  // exactly the sites confirmed is made a draft again first, and saved
  // (UnconfirmChangedRoutine()).
  std::optional<RoutineToRun> Load(const std::string& name,
                                   const std::string& id);
  // Deletes the routine called |name| (exactly one), or with id |id| when
  // |name| is empty.
  bool Delete(const std::string& name, const std::string& id);
  // AutomationSecurity::ConfirmDraftSites() on exactly |sites|, then saved.
  bool ConfirmSites(const std::string& id,
                    const std::vector<std::string>& sites);
  // Every routine of the profile.
  void DeleteAll();

  // Keeps the routines |ids| from before routines belonged to a profile in
  // this profile (see the file comment), in order.
  ClaimResult Claim(const std::vector<std::string>& ids);

  // The name |script| (a routine the Agent studio brings in or edits: an
  // import, a copy, a rename) is saved under beside the profile's other
  // routines (UniqueRoutineName(); |script|'s own id doesn't count): ""
  // when every copy name is taken.
  std::string NameFor(const automation::Script& script);

 private:
  automation::AutomationStorage mine_;
  automation::AutomationStorage shared_;
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_ROUTINE_STORE_H_
