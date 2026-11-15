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
//                                "fired": <unix seconds>,
//                                "written": <unix seconds>,
//                                "pending": <unix seconds>,
//                                "pending_seen": <unix seconds>}}}
//   <user data dir>/Molt Workflow Record Started   ("1\n", never deleted)
//
// An entry, by the file's base name in the directory (<id>.molt, or a
// script set aside by a move, <id>.molt.<token>.taken): the
// AutomationSecurity::SecurityDigest() of what this browser last wrote
// under that name (one only: an older version doesn't match); "fired", the
// trigger.last_fired_unix it wrote there (a schedule saved afresh writes
// 0; an earlier one, from a page opened before a fire, leaves a past fire
// it recorded as it is), or a fire its scheduler recorded since (one
// recorded while the clock was ahead gives way to the next); and
// "written", when it wrote there last (when it seeded the entry, for one
// seeded). The file's own
// last fire time (bookkeeping, outside the digest: other browsers' runs
// change it) is believed only when it can be a fire since that write (no
// earlier, nor later than now by kLastFiredSlackSeconds): LastFired(). One
// written since that is still to come (another browser's Snooze or Skip,
// or forged) counts, until it comes, as a fire when this browser first
// read it: "pending" is that value and "pending_seen" when it was first
// read, kept in the entry (so across restarts) until another value is
// read or this browser writes or fires there. So an earlier one written
// there forces no catch-up run, a later one written once forces none
// either and holds a schedule back at most an interval, and no fire time
// read from a file is ever recorded as "fired" (but at seeding). A
// deleted file's entry is forgotten (put
// back, it doesn't match), and an entry whose file is gone is dropped at
// the next load,
// unless the file is set aside now (<id>.molt while <id>.molt.*.taken is
// there: another browser's move, or one cut short), so it is vouched for
// again once it is put back. A move copies the entry to the new name
// before its rename and forgets the old one after (Copy(), Forget()), so
// whichever name holds the file if the browser ends in between has one.
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
// every change (Set(), NoteFired(), Copy(), Forget(), Restore()) run only
// under the
// storage's change scope of |directory| (its lock, then its lock file), so
// each is ordered with the file change it mirrors; their writes of the
// record happen there too. Vouches(), FiredFloor(), EntryOf() and loaded()
// take only this record's own lock, the innermost of all (the registry's
// is another, never held with it). LastFired() runs in readers too: it
// takes that lock and, when it notes or drops a pending fire time, writes
// the record. Every write of the record takes its write lock (taken before
// the record's own lock), so the writes are one at a time, each of the
// record as it is then: a reader's never lands after a later change's.
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
// A file's last fire time later than now by more than this is no fire
// (WorkflowRecord::LastFired()).
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
    // When this browser last wrote under the name (unix seconds).
    int64_t written = 0;
    // A last fire time still to come that the file said since then
    // (LastFired()), and when this browser first read it (unix seconds);
    // 0 for none. A write or a fire of this browser's drops them.
    int64_t pending = 0;
    int64_t pending_seen = 0;
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
  // reads it (nullopt when it can't be read; its |written| is set to the
  // time of the load). |audit|: an audit line (event, extra) for the
  // directory's log.
  void LoadLocked(
      base::FunctionRef<std::set<std::string>()> present,
      base::FunctionRef<std::optional<Entry>(const std::string& name)> entry_of,
      base::FunctionRef<void(const std::string& event,
                             const std::string& extra)> audit);

  // Whether |digest| is the one recorded for |name| ("" never is).
  bool Vouches(const std::string& name, std::string_view digest) const;
  // The fire time recorded for |name| (Entry::fired); 0 when none.
  int64_t FiredFloor(const std::string& name) const;
  // The last fire time to believe for |name|, whose file says |file_fired|
  // (unix seconds), never earlier than FiredFloor(): |file_fired| when it
  // can be a fire since this browser last wrote there (at least
  // Entry::written, at most now plus kLastFiredSlackSeconds); when it is
  // later than that, the time this browser first read that value there
  // (Entry::pending_seen, kept in the record until another value is read
  // or this browser writes or fires there); FiredFloor() when it is
  // earlier (a fire before that write is in what it wrote). With no entry
  // for |name|, |file_fired| (at least 0). Writes the record when it notes
  // a value still to come, or drops one.
  int64_t LastFired(const std::string& name, int64_t file_fired);

  // The entry recorded for |name| now, if any (for Restore()).
  std::optional<Entry> EntryOf(const std::string& name) const;

  // Under the storage's change scope of |directory|, each written to the
  // record file at once:
  // Before the write (AutomationStorage sets it, then renames the file
  // into place): this browser writes |digest| under |name| ("" forgets it)
  // now, with |fired| as its last fire time. 0 and a later one are the
  // entry's as they are (a schedule saved afresh starts over; a snooze). An
  // earlier one is a page's opened before a fire: the past fire recorded is
  // kept, and only one still in the future (beyond now by
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
  // one), and a pending fire time is dropped; no entry, nothing.
  void NoteFired(const std::string& name, int64_t fired);
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
  // Writes the record file: under |write_lock_|, a snapshot taken under
  // |lock_|, written after it is released (under the storage's change
  // scope for a change; a reader's for LastFired()). Nothing for a
  // session-only record, nor one not loaded yet.
  void Persist();

  // Held for each write of the record file, before |lock_|.
  base::Lock write_lock_;
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
