// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// AutomationScheduler — fires scripts based on Trigger configuration.
// Survives browser restarts: on startup it reads Trigger.last_fired_unix
// from each script and runs missed cron triggers (catch-up). Per user
// spec, runs always use the user's main profile so cookies persist.
//
// Threading: lives on the UI thread; uses base::OneShotTimer to wake up
// at the next scheduled time. macOS sleep wake-ups deliver the timer
// correctly; on Linux/Windows we additionally check elapsed wall time
// every minute via a heartbeat. The workflows are read on the shared
// storage's read sequence (automation_shared_tasks.h), after the changes
// posted before (a fire's last_fired_unix among them), one listing at a
// time: never on the UI thread.

#ifndef CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_SCHEDULER_H_
#define CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_SCHEDULER_H_

#include <memory>
#include <optional>
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

// Callback invoked when a scheduled trigger fires. The scheduler does
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
  // expired, malformed cron).
  static base::Time NextFireTime(const Trigger& trigger, base::Time now);

  // Test seam: drives the scheduler against |now| instead of real wall
  // clock. Visible for unit tests.
  void TickForTesting(base::Time now);

 private:
  void Tick();
  // Lists the workflows (on the storage's read sequence), then OnListed().
  void ArmNext(base::Time now);
  void OnListed(base::Time now, std::vector<Script> scripts);
  // The next timer's workflow, read again, fired.
  void OnTimerScriptLoaded(std::optional<Script> script);

  base::FilePath storage_dir_;
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
