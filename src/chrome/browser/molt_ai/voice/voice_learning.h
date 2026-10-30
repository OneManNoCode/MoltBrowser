// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// What voice mode learned for one profile (docs/design/TOUCHLESS_BROWSING.md
// §6.8), on the device:
//   - routines taught by voice, kept as Agent-mode workflows
//     (voice_routines.h) in the automation engine's storage, where the
//     Agent studio lists them;
//   - the personal vocabulary (names said often, corrections) and site
//     hints, in the profile's MoltVoiceLearning.json (voice_learning_store.h),
//     never synced, read back as untrusted;
//   - the "Learn from what I say" switch, kept in that file (on unless the
//     user turns it off). Off, nothing more is learned and what was learned
//     isn't used (it stays listed, to delete); routines, which the user
//     saves on purpose, still work.
//
// Never for an off-the-record profile, and so never for MoltNet windows'
// profiles (Get() is null there): nothing is learned there, and nothing
// learned is used there.
//
// File I/O runs on one sequence (MayBlock); changes are written a moment
// later, the whole file atomically. UI thread.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_LEARNING_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_LEARNING_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/callback_list.h"
#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/supports_user_data.h"
#include "base/timer/timer.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/voice/voice_learning_store.h"
#include "chrome/browser/molt_ai/voice/voice_routines_logic.h"

class Profile;

namespace base {
class SequencedTaskRunner;
}  // namespace base

namespace molt_ai {
namespace voice {

// The learning file's name in the profile directory.
inline constexpr char kVoiceLearningFileName[] = "MoltVoiceLearning.json";

// A saved routine, as voice mode and settings list it.
struct RoutineSummary {
  std::string id;
  std::string name;
  int steps = 0;
  // Its sites aren't confirmed yet (it has the planner's steps).
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

class VoiceLearning : public base::SupportsUserData::Data {
 public:
  // The profile's, created on first use; null for an off-the-record
  // profile (MoltNet's included).
  static VoiceLearning* Get(Profile* profile);

  VoiceLearning(const VoiceLearning&) = delete;
  VoiceLearning& operator=(const VoiceLearning&) = delete;
  ~VoiceLearning() override;

  // The file has been read (until then nothing is learned or used).
  bool loaded() const { return loaded_; }
  // Runs |done| once loaded (at once if it is).
  void WhenLoaded(base::OnceClosure done);

  // "Learn from what I say".
  bool enabled() const { return loaded_ && enabled_; }
  bool setting() const { return enabled_; }
  void SetEnabled(bool enabled);

  const PersonalVocabulary& vocabulary() const { return data_.vocabulary; }
  const SiteHints& hints() const { return data_.hints; }
  // The saved voice routines (refreshed from storage after each change).
  const std::vector<RoutineSummary>& routines() const { return routines_; }

  // Learning; nothing while !enabled().
  void NoteName(std::string_view phrase);
  void NoteCorrection(std::string_view heard, std::string_view meant);
  void NoteCorrectionUsed(std::string_view heard);
  void HintWorked(std::string_view host, SitePart part, std::string_view name);
  void HintFailed(std::string_view host, SitePart part);

  // Forgetting (whatever the switch says).
  size_t ForgetWord(std::string_view phrase);
  bool ForgetHint(std::string_view host);
  // Words, corrections, hints and every routine taught by voice.
  void ForgetEverything(base::OnceClosure done);

  // Routines (in the automation engine's storage).
  using SavedCallback = base::OnceCallback<void(bool saved, std::string said)>;
  // Saves |steps| as the routine |name| (RoutineDisplayName()).
  void SaveRoutine(std::string name,
                   std::vector<RoutineStep> steps,
                   SavedCallback done);
  // The routine called |name| (FindRoutineName()), or nullopt.
  void LoadRoutine(std::string name,
                   base::OnceCallback<void(std::optional<RoutineToRun>)> done);
  // Deletes the voice routine called |name|, or with id |id| when |name| is
  // empty.
  void DeleteRoutine(std::string name,
                     std::string id,
                     base::OnceCallback<void(bool)> done);
  // After the user confirmed the sites of the routine |id| by voice
  // (voice_routine_sites.h): AutomationSecurity::ConfirmDraftSites() on
  // exactly |sites|, then saved.
  void ConfirmRoutineSites(std::string id,
                           std::vector<std::string> sites,
                           base::OnceCallback<void(bool)> done);
  void RefreshRoutines();

  // Anything changed (for the settings page).
  base::CallbackListSubscription AddChangedCallback(
      base::RepeatingClosure callback);

  // Tests: the automation storage's root (as AutomationStorage::
  // SetRootForTesting()), and writing now.
  void SetAutomationsRootForTesting(const base::FilePath& root);
  void FlushForTesting(base::OnceClosure done);

 private:
  explicit VoiceLearning(Profile* profile);

  void OnRead(LearnedData data, bool enabled);
  void OnRoutines(std::vector<RoutineSummary> routines);
  // Something learned or forgotten: written soon.
  void Changed();
  void Write();

  base::FilePath path_;
  base::FilePath automations_root_;
  scoped_refptr<base::SequencedTaskRunner> task_runner_;
  bool loaded_ = false;
  bool enabled_ = true;
  LearnedData data_;
  std::vector<RoutineSummary> routines_;
  int saves_ = 0;
  std::vector<base::OnceClosure> when_loaded_;
  base::OneShotTimer write_timer_;
  base::RepeatingClosureList changed_callbacks_;
  base::WeakPtrFactory<VoiceLearning> weak_factory_{this};
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_LEARNING_H_
