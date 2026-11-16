// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// AutomationScheduler — fires scripts based on Trigger configuration.
// Survives browser restarts: on startup it reads Trigger.last_fired_unix
// from each script and runs missed cron triggers (catch-up). Each regular
// profile has one (AutomationSchedulerService), over the one shared
// folder and its workflow record; a fire runs in the profile whose
// scheduler claimed it.
//
// A fire is claimed before it runs: AutomationStorage::ClaimFire(), on
// the storage's change sequence, records it only while the record's last
// fire of the workflow is still the one the scheduler read when it found
// it due, and the fire callback runs only then. So of the schedulers of
// this browser's profiles, one runs a due workflow, once (the others find
// it fired); one held (a Snooze or a Skip), whose due run a Skip recorded
// as passed, or saved afresh since it was read doesn't fire.
//
// Holds. molt://ai-automation's Snooze and Skip are holds in this browser's
// workflow record (AutomationStorage::HoldWith()), read as
// Trigger::held_until_unix: no fire before that time, for cron and interval
// schedules alike. NextFireTime() gives the first time at or after the hold
// (an interval keeps its phase: whole intervals after its last fire), so
// the timer is armed past it and the times it covers are never caught up;
// a timer's reload that finds it held since it was listed doesn't fire it
// (and lists again); and the claim is refused before the hold, which the
// claimed fire ends. A Skip of a run due and not claimed yet records it as
// passed instead (AutomationStorage::HoldOrPassWith()): its claim, on the
// last fire read before, fails.
//
// Threading: lives on the UI thread; uses base::OneShotTimer to wake up
// at the next scheduled time. macOS sleep wake-ups deliver the timer
// correctly; on Linux/Windows we additionally check elapsed wall time
// every minute via a heartbeat. The workflows are read on the shared
// storage's read sequence (automation_shared_tasks.h), after the changes
// posted before (a fire's last_fired_unix among them), one listing at a
// time: never on the UI thread.
//
// Only what may run unattended fires (AutomationSecurity::
// MayFireUnattended()): never a workflow whose file was changed outside the
// browser (the storage loads it as an unconfirmed draft), checked when it is
// listed and again when its timer's reload comes. A catch-up's next fire is
// counted from the catch-up itself, and a timer's reload fires a cron or
// interval workflow only if it is still due after its last fire (not
// fired or held since it was listed), so an overdue workflow runs once.
// One held for review that is the browser's again (its file put back as
// the browser last wrote it) isn't caught up for the times that passed
// while it was held: they are recorded as passed (AutomationStorage::
// RecordPassed()), and it fires at its next time, never at the moment
// whatever put the file back chose. The same goes for one whose file was
// missing (moved away, unreadable, not a workflow file) from the listing
// before, or at its timer's reload: found again, it was put back. A timer
// that comes before its time by the wall clock is armed again, not fired
// (its fire would be recorded before that time, still due).
// Its last fire time is this browser's own, as its workflow record has it
// (the one it saved, or a fire it recorded: automation_storage.h), never
// the file's: a fire time written into the file outside this browser
// (another browser's run, an older build's Snooze or Skip, or a forged one)
// neither forces a catch-up run nor holds the schedule back, and two
// browsers with different user data directories sharing the folder each
// fire on their own schedule (a workflow can run in both). base only (the
// storage's sequences), so molt_ai_unittests tests it.

#ifndef CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_SCHEDULER_H_
#define CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_SCHEDULER_H_

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/automation/automation_storage.h"

namespace molt_ai {
namespace automation {

// Callback invoked when a scheduled trigger fires, once the fire is
// claimed (recorded: AutomationStorage::ClaimFire()). The scheduler does
// NOT itself run the script — it delegates to this callback so the
// owning service can spin up a background browser, attach an
// AutomationRunner, etc.
using ScheduledFireCallback =
    base::RepeatingCallback<void(const Script& script,
                                 base::Time fired_at)>;

class AutomationScheduler {
 public:
  AutomationScheduler();
  ~AutomationScheduler();

  AutomationScheduler(const AutomationScheduler&) = delete;
  AutomationScheduler& operator=(const AutomationScheduler&) = delete;

  // The shared storage's directory (AutomationStorage::directory()).
  void SetStorageDir(const base::FilePath& dir);
  void SetFireCallback(ScheduledFireCallback cb);

  // Reads scripts from disk and arms the next timer. Run any cron
  // triggers whose previous fire was missed during browser shutdown.
  void Start();

  // Disable all timers. Call at browser shutdown.
  void Stop();

  // Re-evaluate next-fire after a script was added/edited/deleted.
  void Reschedule();

  // Compute the next fire time for a single trigger, given "now".
  // Returns base::Time() (null) if the trigger cannot fire (e.g.
  // expired, malformed cron). A held trigger's (held_until_unix) is its
  // first time at or after the hold: a cron's first match, an interval's
  // first whole interval after its last fire.
  static base::Time NextFireTime(const Trigger& trigger, base::Time now);

  // The last time |trigger| was due at or before |now|, counted from its
  // last fire: an interval's keeps its phase (its last fire and whole
  // intervals after it); anything else's is |now| (a cron's next time after
  // either is the same). Due times that don't run are recorded as passed up
  // to it (AutomationStorage::RecordPassed(): a workflow held for review
  // across them; molt://ai-automation's Skip of a run due now:
  // AutomationStorage::HoldOrPassWith()).
  static int64_t PassedUntil(const Trigger& trigger, base::Time now);

  // Test seam: drives the scheduler against |now| instead of real wall
  // clock. Visible for unit tests.
  void TickForTesting(base::Time now);

 private:
  void Tick();
  // Lists the workflows (on the storage's read sequence), then OnListed().
  void ArmNext(base::Time now);
  void OnListed(base::Time now, std::vector<Script> scripts);
  // The next timer's workflow |id|, read again, fired (none: it doesn't
  // load, and is held).
  void OnTimerScriptLoaded(const std::string& id, std::optional<Script> script);
  // A fire of |script| at |now|: claimed on the storage's change sequence
  // (AutomationStorage::ClaimFire(), against the last fire it was read
  // with), then, if it was, the fire callback.
  void Claim(Script script, base::Time now);
  void OnFireClaimed(Script script, base::Time fired_at, bool claimed);

  base::FilePath storage_dir_;
  // The workflows the last listing (or a timer's reload since) found held
  // for review (changed outside the browser), or a timer's reload found
  // missing: one listed as the browser's again has had its file put back
  // as the browser last wrote it, and a due time that passed while it was
  // held isn't caught up.
  std::set<std::string> held_;
  // Every workflow the last listing found, once there was one. One missing
  // from it (its file moved away, unreadable, or not a workflow file) that
  // a later listing finds as the browser's own had its file put back, and
  // is taken as held: a due time that passed meanwhile isn't caught up.
  std::optional<std::set<std::string>> listed_;
  // A listing is on its way; another was asked for meanwhile (it follows,
  // so a catch-up fired by the first is seen fired).
  bool listing_ = false;
  bool list_again_ = false;
  ScheduledFireCallback fire_cb_;
  base::OneShotTimer next_timer_;
  base::RepeatingTimer heartbeat_;        // catches missed wakeups
  bool started_ = false;
  base::WeakPtrFactory<AutomationScheduler> weak_factory_{this};
};

// Cron parser shared with the UI for previewing next-fire times.
// Supports the standard 5-field syntax:
//   minute hour day-of-month month day-of-week
// where each field is one of:
//   *                    any
//   N                    literal number
//   N-M                  range
//   */N                  every N
//   N,M,...              list
// Example expressions: "0 9 * * 1-5" (weekdays at 9am).
//
// Returns base::Time() if the expression is malformed or has no future
// match within ~365 days.
base::Time NextCronFire(const std::string& expression,
                        const std::string& timezone,
                        base::Time now);

}  // namespace automation
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_SCHEDULER_H_
