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
// under that name (one only: an older version doesn't match), and the
// highest fire time it wrote or saw there (a floor for the file's
// trigger.last_fired_unix, so an earlier one written there forces no
// catch-up run). A deleted file's entry is forgotten (put back, it doesn't
// match), and an entry whose file is gone is dropped at the next load.
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
// every change (Set(), NoteFired(), Move(), Forget()) run only under the
// storage's change scope of |directory| (its lock, then its lock file), so
// each is ordered with the file change it mirrors; their writes of the
// record happen there too, one at a time. Vouches(), FiredFloor() and
// loaded() take only this record's own lock, the innermost of all (the
// registry's is another, never held with it). base only, so
// molt_ai_unittests tests it on real files.
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
  // reads it (nullopt when it can't be read). |audit|: an audit line
  // (event, extra) for the directory's log.
  void LoadLocked(
      base::FunctionRef<std::set<std::string>()> present,
      base::FunctionRef<std::optional<Entry>(const std::string& name)> entry_of,
      base::FunctionRef<void(const std::string& event,
                             const std::string& extra)> audit);

  // Whether |digest| is the one recorded for |name| ("" never is).
  bool Vouches(const std::string& name, std::string_view digest) const;
  // The highest fire time recorded for |name|; 0 when none.
  int64_t FiredFloor(const std::string& name) const;

  // Under the storage's change scope of |directory|, after the change on
  // disk, each written to the record file at once:
  // This browser wrote |digest| under |name| ("" forgets it), fired then
  // (the floor rises to it, never falls).
  void Set(const std::string& name, std::string digest, int64_t fired);
  // A fire at |fired| of |name| (an entry's floor rises; no entry, nothing).
  void NoteFired(const std::string& name, int64_t fired);
  // |from| was renamed |to|: its entry with it. With no entry for |from|,
  // |to|'s is left as it is.
  void Move(const std::string& from, const std::string& to);
  // |name| was deleted.
  void Forget(const std::string& name);

  Origin origin() const;

 private:
  explicit WorkflowRecord(const base::FilePath& directory);

  // (Re)configures; LoadLocked() runs again.
  void Reconfigure(const Config& config);
  // LoadLocked()'s end for a record that is session only, unless it was
  // reconfigured since |generation|.
  void FinishSessionOnly(uint64_t generation);
  // Writes the record file (a snapshot taken under |lock_|, written after
  // it is released, still under the storage's change scope). Nothing for
  // a session-only record, nor one not loaded yet.
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
