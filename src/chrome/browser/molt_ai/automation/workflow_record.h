// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The Molt Workflow Record: what this browser last wrote in the shared
// workflow directory (~/.moltbrowser/automations/), kept in the browser's
// own user data directory, where nothing that can only write
// ~/.moltbrowser can reach it. A workflow file is trusted only as far as
// this browser recorded it: AutomationStorage (automation_storage.h) checks
// every file it reads against it, and one that doesn't match loads as an
// unconfirmed import (AutomationSecurity::ClampUnvouched()) until the user
// reviews it in the Agent studio (docs/design/AGENT_ACTIONS.md §3.6).
//
//   <user data dir>/Molt Workflow Record
//     {"format": 1, "directory": "<the shared directory>",
//      "origin": "seeded" | "fresh" | "after_loss" | "after_corrupt" |
//                "other_directory",
//      "created": "<ISO 8601>",
//      "files": {"<file name>": {"digest": "<64 lowercase hex>",
//                                "fired": <unix seconds>}}}
//   <user data dir>/Molt Workflow Record Started   ("1\n", never deleted)
//
// An entry, by the file's base name in the directory (<id>.molt, or a
// script set aside by a move, <id>.molt.<token>.taken): the
// AutomationSecurity::SecurityDigest() of what this browser last wrote
// under that name (one only: an older version doesn't match); and "fired"
// (left out when 0), the trigger.last_fired_unix it wrote there (a
// schedule saved afresh writes 0; an earlier one, from a page opened
// before a fire, leaves a past fire it recorded as it is), or a fire its
// scheduler recorded since (one recorded while the clock was ahead gives
// way to the next; a claimed fire, ClaimFired(), is recorded exactly), or
// a later time that passed while the file was held for review
// (NotePassed()). "fired" is the only last fire the storage uses for
// scheduling (LastFired()): the file's own trigger.last_fired_unix, which
// anything can write, is never believed, and no fire time read from a
// file is recorded (but at seeding). So a fire time written into a file
// outside this browser (another browser's run, Snooze or Skip, or a
// forged one) changes nothing here, and two browsers with different user
// data directories sharing the directory each run their own schedules (a
// workflow can run in both). A deleted file's entry is forgotten (put
// back, it doesn't match), and an entry whose file is gone is dropped at
// the next load, unless the file is set aside now (<id>.molt while
// <id>.molt.*.taken is there: another browser's move, or one cut short),
// so it is vouched for again once it is put back. A move copies the entry
// to the new name before its rename and forgets the old one after
// (Copy(), Forget()), so whichever name holds the file if the browser ends
// in between has one.
//
// The first load. The record is always written at its first load (empty
// when there is nothing to record), and the "Started" marker never goes
// away. Only when both are missing, on a user data dir that isn't new
// (Config::may_seed), does the first load trust ("seed") the workflows
// that are there, once, as the previous build ran them. A lost record
// (marker present) or a corrupt one (kept aside as "Molt Workflow
// Record.bad") starts empty and never seeds; one that isn't a regular file
// (a link, a directory, unreadable) is never read through nor written
// over: the record is then "session only", trusting this run's writes, and
// the next start tries again. So is one inside ~/.moltbrowser, or none
// configured (WorkflowRecord::SessionOnlyFor()). Both files are read and
// written only as regular files (molt_regular_file.h), atomically.
//
// Threads. Configure() and For() run anywhere (no I/O). LoadLocked() and
// every change (Set(), NoteFired(), ClaimFired(), NotePassed(), Copy(),
// Forget(), Restore()) run only under the storage's change scope of
// |directory| (its lock, then its lock file), so each is ordered with the
// file change it mirrors, and their writes of the record happen there too,
// one at a time. Vouches(), LastFired(), EntryOf() and loaded() only read,
// under this record's own lock, the innermost of all (the registry's is
// another, never held with it).
// base only, so molt_ai_unittests tests it on real files.
//
// Tests: ScopedWorkflowRecordForTesting (workflow_record_test_util.h).

#ifndef CHROME_BROWSER_MOLT_AI_AUTOMATION_WORKFLOW_RECORD_H_
#define CHROME_BROWSER_MOLT_AI_AUTOMATION_WORKFLOW_RECORD_H_

#include <stddef.h>
#include <stdint.h>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "base/files/file_path.h"
#include "base/functional/function_ref.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"

namespace molt_ai {
namespace automation {

// The record's name in the user data directory (its marker's:
// MarkerFileFor()).
inline constexpr base::FilePath::CharType kWorkflowRecordFileName[] =
    FILE_PATH_LITERAL("Molt Workflow Record");
// The record is read up to this size; a bigger one is corrupt.
inline constexpr size_t kMaxWorkflowRecordBytes = 16 * 1024 * 1024;
// And holds at most this many entries.
inline constexpr size_t kMaxWorkflowRecordEntries = 65536;
// A recorded fire time later than now (WorkflowRecord::Set()), or than a
// fire happening now (WorkflowRecord::NoteFired()), by more than this is no
// past fire: a snooze, or a clock that was ahead.
inline constexpr int64_t kLastFiredSlackSeconds = 5 * 60;

class WorkflowRecord {
 public:
  struct Config {
    // The shared storage's directory (AutomationStorage::SharedDirectory()).
    base::FilePath directory;
    // <user data dir>/Molt Workflow Record; empty: session only.
    base::FilePath record_file;
    // Not a new user data directory: the first load may seed (above).
    bool may_seed = false;
  };
  struct Entry {
    std::string digest;
    // The last fire time this browser wrote under the name, or a later one
    // it fired at (unix seconds).
    int64_t fired = 0;
  };
  enum class Origin {
    kSeeded,
    kFresh,
    kAfterLoss,
    kAfterCorrupt,
    kOtherDirectory,
    kSessionOnly,
  };

  // The record of |config.directory| is the one at |config.record_file|
  // from now on (loaded at its next LoadLocked()). Writes this process made
  // there before (a session-only record's) are kept. No I/O; any thread.
  static void Configure(const Config& config);
  // The configured record of |directory|; null if none.
  static WorkflowRecord* For(const base::FilePath& directory);
  // A session-only record of |directory| (made on first use), for the
  // shared directory when no record was configured: it trusts only what
  // this process writes there (fails closed across restarts).
  static WorkflowRecord* SessionOnlyFor(const base::FilePath& directory);
  // Tests: forgets every record (none may be in use).
  static void ResetForTesting();

  // <record file> Started.
  static base::FilePath MarkerFileFor(const base::FilePath& record_file);

  WorkflowRecord(const WorkflowRecord&) = delete;
  WorkflowRecord& operator=(const WorkflowRecord&) = delete;
  ~WorkflowRecord();

  // Whether the record was loaded since it was last configured.
  bool loaded() const;

  // Under the storage's change scope: loads (and on an upgrade, once,
  // seeds) the record, then writes it, as above; nothing once loaded.
  // |present|: the names of the regular script and set-aside files in the
  // directory. |entry_of|: what a present file holds, read as the storage
  // reads it, its own last fire time included (nullopt when it can't be
  // read). |audit|: an audit line (event, extra) for the directory's log.
  void LoadLocked(
      base::FunctionRef<std::set<std::string>()> present,
      base::FunctionRef<std::optional<Entry>(const std::string& name)> entry_of,
      base::FunctionRef<void(const std::string& event,
                             const std::string& extra)> audit);

  // Whether |digest| is the one recorded for |name| ("" never is).
  bool Vouches(const std::string& name, std::string_view digest) const;
  // |name|'s last fire, for scheduling: the one recorded (Entry::fired),
  // whatever its file says; 0 when there is no entry.
  int64_t LastFired(const std::string& name) const;

  // The entry recorded for |name| now, if any (for Restore()).
  std::optional<Entry> EntryOf(const std::string& name) const;

  // Under the storage's change scope of |directory|, each written to the
  // record file at once:
  // Before the write (AutomationStorage sets it, then renames the file
  // into place): this browser writes |digest| under |name| ("" forgets it)
  // now, with |fired| as its last fire time. 0 and a later one are the
  // entry's as they are (a schedule saved afresh starts over; a snooze). An
  // earlier one is a page's opened before a fire (a page's save that keeps
  // the schedule writes the stored one instead: AutomationStorage::
  // SaveChecked()), or a schedule saved afresh counted from now: the past
  // fire recorded is kept, and only one still in the future (beyond now by
  // kLastFiredSlackSeconds: a snooze, or a clock that was ahead) gives way
  // to it.
  void Set(const std::string& name, std::string digest, int64_t fired);
  // |name|'s entry exactly as |entry| was (EntryOf() before a Set() whose
  // file couldn't be written); none: forgotten.
  void Restore(const std::string& name, const std::optional<Entry>& entry);
  // A fire of |name| at |fired| by this browser's scheduler, on its own
  // clock, never a time read from a file: an entry's fire time rises to
  // it, or comes down to it from beyond it by kLastFiredSlackSeconds (a
  // fire recorded while the clock was ahead, which this fire shows wasn't
  // one); no entry, nothing.
  void NoteFired(const std::string& name, int64_t fired);
  // The scheduler's claim of a fire of |name| at |fired| (its own clock),
  // made when it read |expected| as the last fire: only while the entry's
  // fire time is still |expected|, and never a second fire in that second
  // (|fired| == |expected|). The fire time is then |fired| exactly (down
  // from one still in the future: a snooze, a Skip or a clock that was
  // ahead, which the fire, due now, has overtaken), so a claim always moves
  // it and a second one of the same read fails. Whether it was claimed.
  bool ClaimFired(const std::string& name, int64_t expected, int64_t fired);
  // Due times of |name| up to |passed| went by while its file was held for
  // review (changed outside this browser; the scheduler's clock, never a
  // time read from a file), and it is this browser's again: its fire time
  // rises to |passed|, so they aren't caught up (run at the moment the file
  // was put back). Only ever later; no entry, nothing. Whether it changed.
  bool NotePassed(const std::string& name, int64_t passed);
  // Before |from| is renamed |to|: |from|'s entry, under |to| too (then
  // the rename, then Forget(|from|); Forget(|to|) if it failed). False,
  // and nothing changed, with no entry for |from| (|to|'s, if any, stays).
  bool Copy(const std::string& from, const std::string& to);
  // |name| was deleted, or renamed (its entry copied first).
  void Forget(const std::string& name);

  Origin origin() const;

 private:
  explicit WorkflowRecord(const base::FilePath& directory);

  // (Re)configures; LoadLocked() runs again.
  void Reconfigure(const Config& config);
  // LoadLocked()'s end for a record that is session only, unless it was
  // reconfigured since |generation|.
  void FinishSessionOnly(uint64_t generation);
  // Writes the record file: a snapshot taken under |lock_|, written after
  // it is released, under the storage's change scope (so one at a time,
  // in the order of the changes). Nothing for a session-only record, nor
  // one not loaded yet.
  void Persist();

  mutable base::Lock lock_;
  Config config_ GUARDED_BY(lock_);
  // Bumped by each Reconfigure(): a load that started before one doesn't
  // count.
  uint64_t generation_ GUARDED_BY(lock_) = 0;
  bool needs_load_ GUARDED_BY(lock_) = true;
  bool session_only_ GUARDED_BY(lock_) = false;
  Origin origin_ GUARDED_BY(lock_) = Origin::kSessionOnly;
  // When the record file was first written (ISO 8601).
  std::string created_ GUARDED_BY(lock_);
  std::map<std::string, Entry> entries_ GUARDED_BY(lock_);
};

}  // namespace automation
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AUTOMATION_WORKFLOW_RECORD_H_
