// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Filesystem persistence for automation scripts.
// Layout:
//   ~/.moltbrowser/automations/
//     ├── <script-id>.molt          (one JSON file per script)
//     ├── audit.log                  (append-only log of every run)
//     └── artifacts/<script-id>/    (screenshots, extracted data)
// That is the shared storage, every profile's ordinary workflows. A
// storage made with a directory keeps the same files there instead: a
// profile's voice routines (molt_ai/voice/voice_routine_store.h), or the
// shared storage's own directory (SharedDirectory()) for callers off the
// UI thread.
//
// Files (molt_ai/common/molt_regular_file.h): a .molt is read only when it
// is a regular file (never through a link) of at most kMaxScriptFileBytes;
// anything else is skipped with a log line, never opened and never written
// over. Each is written atomically (a temporary file, then a rename), never
// over the cap. audit.log is appended to only when it is a regular file,
// and created when missing.
//
// Threads. A storage made with a directory is used on a sequence that may
// block, and checks it (base::ScopedBlockingCall): the profile's routines'
// sequence (VoiceLearning's, which voice mode and the Agent studio share)
// or the shared storage's (SharedSequence(), the Agent studio's). The
// default-constructed shared storage is for the callers that still run on
// the UI thread (ScopedAllowBlockingForMolt): the runner, the scheduler and
// its service, the recorder, the run window, molt://ai-automation and the
// AI chat's saves.
//
// Changes. Within the browser, every change to a directory's scripts
// (Save(), Update(), Delete(), SetTrustLevel(), Take() and the rest, and
// the migration Load() may write) holds that directory's lock, so a
// read-modify-write made with Update() never interleaves with another
// change there, from whichever thread. Take()'s rename is what keeps a
// claim once across browsers.

#ifndef CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_STORAGE_H_
#define CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_STORAGE_H_

#include <stddef.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/function_ref.h"
#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"

namespace base {
class SequencedTaskRunner;
}  // namespace base

namespace molt_ai {
namespace automation {

// A .molt is at most this big: a bigger one isn't read, nor written.
inline constexpr size_t kMaxScriptFileBytes = 8 * 1024 * 1024;
// The end of audit.log ReadAuditTail() reads, at most.
inline constexpr size_t kMaxAuditTailBytes = 1024 * 1024;
// A script set aside by a Take() (<id>.molt.taken) more recently than this
// may be one another browser on the same home directory is moving now;
// older, and not being taken in this browser, its move was cut short.
inline constexpr base::TimeDelta kTakenGracePeriod = base::Minutes(1);

class AutomationStorage {
 public:
  // The shared storage, ~/.moltbrowser/automations/, on the UI thread
  // (see the threads above).
  AutomationStorage();
  // A storage in |directory| (the files above, without the "automations"
  // level), on a sequence that may block.
  explicit AutomationStorage(const base::FilePath& directory);
  ~AutomationStorage();

  AutomationStorage(const AutomationStorage&) = delete;
  AutomationStorage& operator=(const AutomationStorage&) = delete;

  // The shared storage's directory: <|root| or ~/.moltbrowser>/automations.
  static base::FilePath SharedDirectory(
      const base::FilePath& root = base::FilePath());
  // The one sequence on which the Agent studio reads and changes the
  // shared storage (MayBlock, BLOCK_SHUTDOWN, made lazily).
  static scoped_refptr<base::SequencedTaskRunner> SharedSequence();

  // Where this storage's files are (no I/O).
  base::FilePath directory() const { return GetAutomationsDir(); }
  // <directory>/artifacts/<script-id>/ (no I/O).
  base::FilePath ArtifactsDir(const std::string& script_id) const;

  // Resolve and create the per-user automations directory the first time it's
  // needed. On macOS / Linux this is ~/.moltbrowser/automations/.
  bool EnsureDirectory();

  // List all script IDs currently saved on disk (regular files only).
  std::vector<std::string> ListIds();

  // List all scripts (parsed). Skips files that fail to parse.
  std::vector<Script> ListAll();

  // Load one script by id. Returns nullopt if the file doesn't exist or is
  // malformed. A file that still holds a plaintext password value (written
  // before secret steps existed) is scrubbed and rewritten in place first
  // (AutomationSecurity::ScrubPlaintextSecrets).
  std::optional<Script> Load(const std::string& id);

  // Save (create or overwrite). Returns true if the file was successfully
  // written. The file is written atomically (a temporary file + rename).
  // Plaintext secret values are scrubbed from what is written. Nothing is
  // written over anything but a regular file, nor a file over the cap.
  bool Save(const Script& script);

  // A read-modify-write of script |id|: loaded as Load() loads it (known by
  // its file's name), changed by |change|, and saved as Save() saves it,
  // all under the directory's lock (above). False, and nothing written,
  // when it is missing or unreadable, |change| returns false, or the save
  // fails. |change| must not use a storage.
  using ScriptChange = base::FunctionRef<bool(Script& script)>;
  bool Update(const std::string& id, ScriptChange change);

  // Delete a script + its artifacts directory.
  bool Delete(const std::string& id);

  // Moving a script to another storage, once (a voice routine a profile
  // claims). Take() renames <id>.molt aside in one step, so of two takers
  // only one gets it, and returns it read as Load() reads it (its secrets
  // scrubbed in what it returns); nullopt when it is gone, being taken in
  // this browser, not a regular file (a link is never taken, nor read) or
  // unreadable, and then it stays as it was. Each Take() sets it aside
  // under a name of its own, <id>.molt.<random>.taken, so a script saved
  // under that id meanwhile and taken by another browser is set aside
  // beside it, never over it. After a Take(), this storage's FinishTake()
  // deletes it once it is saved elsewhere, or ReturnTaken() puts it back:
  // as <id>.molt, or, when a script of that id was saved meanwhile, under
  // the first free id made by adding "_" (never over another file).
  std::optional<Script> Take(const std::string& id);
  void FinishTake(const std::string& id);
  bool ReturnTaken(const std::string& id);

  // A script set aside by a Take(): its id and its file.
  struct SetAside {
    std::string id;
    base::FilePath file;
  };
  // Moves cut short (the browser ended between a Take() and its
  // FinishTake() or ReturnTaken()): the scripts set aside (round 43's
  // <id>.molt.taken too) that no Take() in this browser is moving now and
  // that were set aside at least kTakenGracePeriod ago (by the clock; one
  // set aside "in the future" counts as cut short). Each is then finished
  // or returned by the caller, which knows whether its move was saved
  // (VoiceRoutineStore::RecoverInterruptedClaims()).
  std::vector<SetAside> InterruptedTakes();
  // A script set aside, read as Take() read it; nullopt when it isn't a
  // regular file or can't be read.
  std::optional<Script> ReadSetAside(const SetAside& set_aside);
  // As FinishTake() and ReturnTaken(), for a move cut short.
  void FinishSetAside(const SetAside& set_aside);
  bool ReturnSetAside(const SetAside& set_aside);

  // The promoteTrust path: sets the stored script's trust level and writes
  // a "trust_changed" audit line. Callers are UI handlers acting on a user
  // click (molt://ai-automation's trust panel, the Agent studio's Trust
  // button); saving / importing never changes trust (see
  // AutomationSecurity::ClampTrustForSave). Returns false if the script is
  // missing or could not be saved.
  bool SetTrustLevel(const std::string& id, TrustLevel level);

  // -------- Audit log --------
  // Append a single line to ~/.moltbrowser/automations/audit.log.
  // Format: ISO8601 | script_id | event | extra
  void AppendAudit(const std::string& script_id,
                   const std::string& event,
                   const std::string& extra);

  // Read the most recent N audit lines (returns oldest-first), from the
  // last kMaxAuditTailBytes of the log at most.
  std::vector<std::string> ReadAuditTail(int max_lines = 200);

  // -------- Artifacts --------
  // Returns ~/.moltbrowser/automations/artifacts/<script-id>/, creating it.
  base::FilePath EnsureArtifactsDir(const std::string& script_id);

  // Test seam: override the root directory (defaults to ~/.moltbrowser).
  // The shared storage only.
  void SetRootForTesting(const base::FilePath& root);

 private:
  // Blocking allowed (the default storage, on the UI thread) or checked (a
  // storage made with a directory), for the scope of a call.
  class BlockingScope;

  base::FilePath GetRoot() const;
  base::FilePath GetAutomationsDir() const;
  base::FilePath GetScriptPath(const std::string& id) const;
  // A new set-aside file for |id|: <id>.molt.<random>.taken.
  base::FilePath NewSetAsidePath(const std::string& id) const;
  // Whether |set_aside| names a set-aside file of this directory.
  bool IsSetAsideHere(const SetAside& set_aside) const;
  base::FilePath GetAuditLogPath() const;
  // The script file at |path| read and parsed (a regular file, capped);
  // nullopt otherwise, with a log line for a file skipped.
  std::optional<base::DictValue> ReadScriptDict(const base::FilePath& path);
  // Under the directory's lock: atomically writes |script_json|
  // (pretty-printed) to |path|, unless something but a regular file is
  // there or it is over the cap.
  bool WriteScriptJsonLocked(const base::FilePath& path,
                             const base::DictValue& script_json);
  // Under the directory's lock: Save()'s write.
  bool SaveLocked(const Script& script);
  // Under the directory's lock: ReturnTaken()'s rename.
  bool ReturnSetAsideLocked(const SetAside& set_aside);

  base::FilePath root_override_;
  // A storage of its own: its files are here.
  base::FilePath directory_;
  // The scripts this storage took and hasn't finished or returned yet: id
  // -> its set-aside file.
  std::map<std::string, base::FilePath> taken_;
};

}  // namespace automation
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_STORAGE_H_
