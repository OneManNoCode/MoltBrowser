// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// What voice mode learned for one profile (docs/design/TOUCHLESS_BROWSING.md
// §6.8), on the device:
//   - routines taught by voice, kept as Agent-mode workflows
//     (voice_routines.h) in the profile's own storage
//     (voice_routine_store.h), where the profile's Agent studio lists them;
//     the routines from before routines belonged to a profile, offered to
//     it until it keeps them or its user says they aren't theirs;
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
#include "chrome/browser/molt_ai/voice/voice_routine_store.h"
#include "chrome/browser/molt_ai/voice/voice_routines_logic.h"

class Profile;

namespace base {
class SequencedTaskRunner;
}  // namespace base

namespace molt_ai {
namespace voice {

// The learning file's name in the profile directory.
inline constexpr char kVoiceLearningFileName[] = "MoltVoiceLearning.json";

class VoiceLearning : public base::SupportsUserData::Data {
 public:
  // The profile's, created on first use; null for an off-the-record
  // profile (MoltNet's included).
  static VoiceLearning* Get(Profile* profile);

  VoiceLearning(const VoiceLearning&) = delete;
  VoiceLearning& operator=(const VoiceLearning&) = delete;
  ~VoiceLearning() override;

  // The file has been read and the routines listed (until then nothing is
  // learned or used).
  bool loaded() const { return loaded_; }
  // Runs |done| once loaded (at once if it is).
  void WhenLoaded(base::OnceClosure done);

  // "Learn from what I say".
  bool enabled() const { return loaded_ && enabled_; }
  bool setting() const { return enabled_; }
  void SetEnabled(bool enabled);

  const PersonalVocabulary& vocabulary() const { return data_.vocabulary; }
  const SiteHints& hints() const { return data_.hints; }
  // The profile's voice routines (refreshed from storage after each
  // change).
  const std::vector<RoutineSummary>& routines() const { return routines_; }
  // The routines from before routines belonged to a profile, but those
  // the user said in this profile aren't theirs (voice_routine_store.h).
  // Any change to them changes unclaimed_version().
  const std::vector<RoutineSummary>& unclaimed() const { return unclaimed_; }
  int unclaimed_version() const { return unclaimed_version_; }
  // How many of them the user said in this profile aren't theirs.
  size_t declined_count() const { return declined_listed_; }
  bool Declined(const std::string& id) const;

  // Where the profile's routines are (VoiceRoutinesDir()), and the shared
  // storage's root (empty: ~/.moltbrowser): the Agent studio opens a
  // VoiceRoutineStore on them.
  const base::FilePath& routines_dir() const { return routines_dir_; }
  const base::FilePath& shared_root() const { return automations_root_; }

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

  // Routines (in the profile's storage, voice_routine_store.h).
  using SavedCallback = base::OnceCallback<void(RoutineSaveResult)>;
  // Saves |steps| as the routine |name| (RoutineDisplayName()); the
  // routine of that name is replaced (its id kept) only when |replace|.
  void SaveRoutine(std::string name,
                   std::vector<RoutineStep> steps,
                   bool replace,
                   SavedCallback done);
  // The routine called |name| (FindRoutineName()), or with id |id| when
  // |name| is empty; nullopt when there is none. One whose steps no longer
  // use exactly the sites confirmed is made a draft again first, and saved
  // (UnconfirmChangedRoutine()).
  void LoadRoutine(std::string name,
                   std::string id,
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

  // The routines from before routines belonged to a profile, |ids|: kept
  // in this profile (the user said so, by voice or with a click in the
  // Agent studio), each once.
  void ClaimRoutines(std::vector<std::string> ids,
                     base::OnceCallback<void(ClaimResult)> done);
  // Not this profile's: not offered here again.
  void DeclineRoutines(const std::vector<std::string>& ids);
  // Offered here again (the Agent studio's "Show").
  void UndeclineRoutines();

  // Anything changed (for the settings page).
  base::CallbackListSubscription AddChangedCallback(
      base::RepeatingClosure callback);

  // Tests: the shared storage's root (as AutomationStorage::
  // SetRootForTesting()), where the routines from before profiles are,
  // and writing now.
  void SetAutomationsRootForTesting(const base::FilePath& root);
  void FlushForTesting(base::OnceClosure done);

 private:
  explicit VoiceLearning(Profile* profile);

  void OnRead(LearnedData data, bool enabled);
  void OnLoaded();
  // Lists the routines again, then runs |done|.
  void RefreshRoutinesThen(base::OnceClosure done);
  void OnRoutines(base::OnceClosure done,
                  std::vector<RoutineSummary> routines,
                  std::vector<RoutineSummary> unclaimed);
  // The unclaimed routines but the declined ones, from |all_unclaimed_|.
  void FilterUnclaimed();
  // Something learned or forgotten: written soon.
  void Changed();
  void Write();

  base::FilePath path_;
  base::FilePath routines_dir_;
  base::FilePath automations_root_;
  scoped_refptr<base::SequencedTaskRunner> task_runner_;
  bool loaded_ = false;
  bool enabled_ = true;
  LearnedData data_;
  std::vector<RoutineSummary> routines_;
  std::vector<RoutineSummary> all_unclaimed_;
  std::vector<RoutineSummary> unclaimed_;
  int unclaimed_version_ = 0;
  size_t declined_listed_ = 0;
  int saves_ = 0;
  std::vector<base::OnceClosure> when_loaded_;
  base::OneShotTimer write_timer_;
  base::RepeatingClosureList changed_callbacks_;
  base::WeakPtrFactory<VoiceLearning> weak_factory_{this};
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_LEARNING_H_
