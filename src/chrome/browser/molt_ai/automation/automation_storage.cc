// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/automation_storage.h"

#include <stdint.h>

#include <algorithm>
#include <ctime>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

#include "base/check_op.h"
#include "base/files/file.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/stack_allocated.h"
#include "base/no_destructor.h"
#include "base/rand_util.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/synchronization/lock.h"
#include "base/task/lazy_thread_pool_task_runner.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/task_traits.h"
#include "base/threading/scoped_blocking_call.h"
#include "build/build_config.h"
#include "chrome/browser/molt_ai/automation/automation_security.h"
#include "chrome/browser/molt_ai/automation/workflow_record.h"
#include "chrome/browser/molt_ai/common/molt_home_dir.h"
#include "chrome/browser/molt_ai/common/molt_regular_file.h"

namespace molt_ai {
namespace automation {

namespace {

constexpr char kAutomationsDirName[] = ".moltbrowser";
constexpr char kAutomationsSubdir[] = "automations";
// Another browser on the same home directory takes it before a change
// (MoltFileLock).
constexpr char kLockFileName[] = ".storage.lock";
constexpr char kArtifactsSubdir[] = "artifacts";
constexpr char kAuditLogName[] = "audit.log";
constexpr char kScriptExt[] = ".molt";
// A script taken to be moved (Take()): no longer listed or loaded. Set
// aside as <id>.molt.<token>.taken (round 43: <id>.molt.taken).
constexpr char kTakenExt[] = ".taken";
// A script id, at most.
constexpr size_t kMaxIdBytes = 128;

// The shared storage's sequence for changes (made lazily, and per test
// environment in unit tests). A change posted before shutdown is written.
base::LazyThreadPoolSequencedTaskRunner g_shared_sequence =
    LAZY_THREAD_POOL_SEQUENCED_TASK_RUNNER_INITIALIZER(
        base::TaskTraits(base::MayBlock(),
                         base::TaskPriority::USER_VISIBLE,
                         base::TaskShutdownBehavior::BLOCK_SHUTDOWN));
// And for reads, which shutdown never waits for.
base::LazyThreadPoolSequencedTaskRunner g_shared_read_sequence =
    LAZY_THREAD_POOL_SEQUENCED_TASK_RUNNER_INITIALIZER(
        base::TaskTraits(base::MayBlock(),
                         base::TaskPriority::USER_VISIBLE,
                         base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN));

// How many change scopes this thread is in: PrepareRecord() and a read's
// second look take the directory's lock, which isn't recursive, so neither
// may run inside one.
constinit thread_local int g_change_scope_depth = 0;

// Every change to a directory's scripts holds its lock (one per directory,
// for the browser's lifetime: the shared storage and each profile's).
base::Lock& DirectoryLock(const base::FilePath& directory) {
  static base::NoDestructor<base::Lock> registry_lock;
  static base::NoDestructor<
      std::map<base::FilePath, std::unique_ptr<base::Lock>>>
      locks;
  base::AutoLock hold(*registry_lock);
  std::unique_ptr<base::Lock>& lock = (*locks)[directory];
  if (!lock) {
    lock = std::make_unique<base::Lock>();
  }
  return *lock;
}

// The scripts being taken in this browser now (their set-aside paths), so
// that InterruptedTakes() never reports a move in progress.
class Taking {
 public:
  static Taking& Get() {
    static base::NoDestructor<Taking> taking;
    return *taking;
  }
  // False when |path| is being taken already.
  bool Start(const base::FilePath& path) {
    base::AutoLock hold(lock_);
    return paths_.insert(path).second;
  }
  void Stop(const base::FilePath& path) {
    base::AutoLock hold(lock_);
    paths_.erase(path);
  }
  bool Has(const base::FilePath& path) {
    base::AutoLock hold(lock_);
    return paths_.count(path) > 0;
  }

 private:
  base::Lock lock_;
  std::set<base::FilePath> paths_;
};

// Reasonably safe filename: only id chars (lowercased ascii, digits, _-)
// already constrain things, but defensively reject path separators.
bool IsValidScriptId(const std::string& id) {
  if (id.empty() || id.size() > kMaxIdBytes)
    return false;
  for (char c : id) {
    if (c == '/' || c == '\\' || c == ':' || c == '.' || c == '\0')
      return false;
  }
  return true;
}

// ISO-8601 in UTC for audit timestamps. A clock the C library can't convert
// (on Windows, one set before 1970) gives the epoch: strftime() on the
// fields a failed conversion leaves ends the process there.
std::string NowISO8601() {
  std::time_t t = std::time(nullptr);
  std::tm gmt{};
#if BUILDFLAG(IS_WIN)
  const bool converted = gmtime_s(&gmt, &t) == 0;
#else
  const bool converted = gmtime_r(&t, &gmt) != nullptr;
#endif
  if (!converted)
    return "1970-01-01T00:00:00Z";
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &gmt);
  return std::string(buf);
}

// 16 random hex digits: a set-aside file's name of its own.
std::string RandomToken() {
  uint64_t bits = base::RandUint64();
  std::string token;
  for (int i = 0; i < 16; ++i) {
    token += "0123456789abcdef"[bits & 0xf];
    bits >>= 4;
  }
  return token;
}

// |id| cut to at most |max_bytes|, never inside a UTF-8 character.
std::string IdPrefix(const std::string& id, size_t max_bytes) {
  size_t size = std::min(id.size(), max_bytes);
  while (size > 0 && size < id.size() &&
         (static_cast<unsigned char>(id[size]) & 0xc0) == 0x80) {
    --size;
  }
  return id.substr(0, size);
}

// The id of the script a set-aside file holds: "<id>.molt.<token>.taken"
// (a token of 1 to 32 lowercase hex digits), or round 43's
// "<id>.molt.taken"; nullopt for any other name.
std::optional<std::string> SetAsideIdOf(const std::string& name) {
  const size_t dot = name.find('.');
  if (dot == std::string::npos)
    return std::nullopt;
  const std::string id = name.substr(0, dot);
  if (!IsValidScriptId(id))
    return std::nullopt;
  const std::string rest = name.substr(dot);
  const std::string head = std::string(kScriptExt) + ".";
  if (rest == std::string(kScriptExt) + kTakenExt)
    return id;
  if (rest.size() <= head.size() + std::string_view(kTakenExt).size() ||
      rest.compare(0, head.size(), head) != 0 ||
      !base::EndsWith(rest, kTakenExt, base::CompareCase::SENSITIVE)) {
    return std::nullopt;
  }
  const std::string token =
      rest.substr(head.size(), rest.size() - head.size() -
                                   std::string_view(kTakenExt).size());
  if (token.size() > 32)
    return std::nullopt;
  for (char c : token) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
      return std::nullopt;
  }
  return id;
}

// Whether a script set aside at |set_aside| may still be moving in another
// browser (within the grace period, by this clock).
bool MayStillBeMoving(base::Time set_aside, base::Time now) {
  const base::TimeDelta age = now - set_aside;
  return !age.is_negative() && age < kTakenGracePeriod;
}

// A script file's contents as its readers parse them; nullopt when they
// don't parse as a script.
std::optional<Script> ParseScriptBytes(const std::string& bytes) {
  std::optional<base::Value> parsed =
      base::JSONReader::Read(bytes, base::JSON_ALLOW_TRAILING_COMMAS);
  if (!parsed || !parsed->is_dict()) {
    return std::nullopt;
  }
  return Script::FromJSON(parsed->GetDict());
}

// A file's name in the directory, as the record keeps it.
std::string NameOf(const base::FilePath& path) {
  return path.BaseName().AsUTF8Unsafe();
}

}  // namespace

class AutomationStorage::BlockingScope {
 public:
  // Checks the sequence may block (never the UI thread).
  explicit BlockingScope(const AutomationStorage&)
      : check_(FROM_HERE, base::BlockingType::MAY_BLOCK) {}
  BlockingScope(const BlockingScope&) = delete;
  BlockingScope& operator=(const BlockingScope&) = delete;

 private:
  base::ScopedBlockingCall check_;
};

class AutomationStorage::ChangeScope {
  // Holds a base::AutoLock: on the stack only.
  STACK_ALLOCATED();

 public:
  explicit ChangeScope(const AutomationStorage& storage)
      : hold_(DirectoryLock(storage.GetAutomationsDir())),
        file_(storage.GetAutomationsDir().Append(
                  base::FilePath::FromUTF8Unsafe(kLockFileName)),
              kStorageLockWait) {
    ++g_change_scope_depth;
  }
  ChangeScope(const ChangeScope&) = delete;
  ChangeScope& operator=(const ChangeScope&) = delete;
  ~ChangeScope() { --g_change_scope_depth; }

 private:
  // This browser's changes, then the other browsers'.
  base::AutoLock hold_;
  MoltFileLock file_;
};

AutomationStorage::AutomationStorage() = default;
AutomationStorage::AutomationStorage(const base::FilePath& directory)
    : directory_(directory) {}
AutomationStorage::~AutomationStorage() = default;

// static
base::FilePath AutomationStorage::SharedDirectory(const base::FilePath& root) {
  AutomationStorage shared;
  if (!root.empty()) {
    shared.SetRootForTesting(root);
  }
  return shared.GetAutomationsDir();
}

// static
void AutomationStorage::ConfigureSharedRecord(const base::FilePath& record_file,
                                              bool may_seed) {
  const base::FilePath dir = SharedDirectory();
  WorkflowRecord::Configure({dir, record_file, may_seed});
  // Loaded at once, whatever this launch does next: what it decides (a new
  // user data directory never seeds) is then on disk before a file can be
  // planted for the next start to take for an upgrade's.
  SharedSequence()->PostTask(FROM_HERE,
                             base::BindOnce(
                                 [](const base::FilePath& dir) {
                                   AutomationStorage(dir).PrepareRecord();
                                 },
                                 dir));
}

// static
scoped_refptr<base::SequencedTaskRunner> AutomationStorage::SharedSequence() {
  return g_shared_sequence.Get();
}

// static
scoped_refptr<base::SequencedTaskRunner>
AutomationStorage::SharedReadSequence() {
  return g_shared_read_sequence.Get();
}

base::FilePath AutomationStorage::GetRoot() const {
  if (!root_override_.empty())
    return root_override_;
  // Without a home directory, a relative one, as before.
  return MoltHomeDir().value_or(
      base::FilePath::FromUTF8Unsafe(kAutomationsDirName));
}

base::FilePath AutomationStorage::GetAutomationsDir() const {
  if (!directory_.empty())
    return directory_;
  return GetRoot().Append(base::FilePath::FromUTF8Unsafe(kAutomationsSubdir));
}

base::FilePath AutomationStorage::GetScriptPath(const std::string& id) const {
  return GetAutomationsDir().Append(
      base::FilePath::FromUTF8Unsafe(id + kScriptExt));
}

base::FilePath AutomationStorage::NewSetAsidePath(const std::string& id) const {
  return GetAutomationsDir().Append(base::FilePath::FromUTF8Unsafe(
      id + kScriptExt + "." + RandomToken() + kTakenExt));
}

bool AutomationStorage::IsSetAsideHere(const SetAside& set_aside) const {
  return set_aside.file.DirName() == GetAutomationsDir() &&
         SetAsideIdOf(set_aside.file.BaseName().AsUTF8Unsafe()) == set_aside.id;
}

base::FilePath AutomationStorage::GetAuditLogPath() const {
  return GetAutomationsDir().Append(
      base::FilePath::FromUTF8Unsafe(kAuditLogName));
}

base::FilePath AutomationStorage::ArtifactsDir(
    const std::string& script_id) const {
  return GetAutomationsDir()
      .Append(base::FilePath::FromUTF8Unsafe(kArtifactsSubdir))
      .Append(base::FilePath::FromUTF8Unsafe(script_id));
}

void AutomationStorage::SetRootForTesting(const base::FilePath& root) {
  root_override_ = root;
}

void AutomationStorage::SetBeforeFileChangeForTesting(
    base::RepeatingClosure closure) {
  before_file_change_for_testing_ = std::move(closure);
}

bool AutomationStorage::EnsureDirectory() {
  BlockingScope blocking(*this);
  base::FilePath dir = GetAutomationsDir();
  if (base::DirectoryExists(dir))
    return true;
  base::File::Error err = base::File::FILE_OK;
  if (!base::CreateDirectoryAndGetError(dir, &err)) {
    LOG(ERROR) << "[Automation] Could not create " << dir.value()
               << " err=" << err;
    return false;
  }
  return true;
}

std::vector<std::string> AutomationStorage::ListIds() {
  BlockingScope blocking(*this);
  std::vector<std::string> out;
  base::FilePath dir = GetAutomationsDir();
  if (!base::DirectoryExists(dir))
    return out;

  // A script's write the browser didn't finish (it ended between the
  // temporary file and the rename) left its temporary file: deleted. One
  // of a write in progress (kStaleWriteAge) is left to it.
  DeleteStaleWriteTemporaries(
      dir,
      [](std::string_view name) {
        const std::string_view ext(kScriptExt);
        return name.size() > ext.size() &&
               name.substr(name.size() - ext.size()) == ext &&
               IsValidScriptId(
                   std::string(name.substr(0, name.size() - ext.size())));
      },
      base::Time::Now());

  base::FileEnumerator e(dir, /*recursive=*/false,
                         base::FileEnumerator::FILES,
                         FILE_PATH_LITERAL("*.molt"));
  for (auto path = e.Next(); !path.empty(); path = e.Next()) {
    // A link, a FIFO or a device named *.molt isn't a script.
    if (MoltFileKindAt(path, MoltFileLinks::kNotFollowed) !=
        MoltFileKind::kRegular) {
      LOG(WARNING) << "[Automation] skipped " << path.BaseName().AsUTF8Unsafe()
                   << ": not a regular file";
      continue;
    }
    std::string filename = path.BaseName().RemoveExtension().AsUTF8Unsafe();
    if (!filename.empty())
      out.push_back(filename);
  }
  return out;
}

std::vector<Script> AutomationStorage::ListAll() {
  PrepareRecord();
  std::vector<Script> out;
  for (const auto& id : ListIds()) {
    if (auto s = Load(id))
      out.push_back(std::move(*s));
  }
  return out;
}

std::optional<base::DictValue> AutomationStorage::ReadScriptDict(
    const base::FilePath& path) {
  MoltFileRead read =
      ReadMoltFile(path, kMaxScriptFileBytes, MoltFileLinks::kNotFollowed);
  switch (read.kind) {
    case MoltFileKind::kRegular:
      break;
    case MoltFileKind::kMissing:
      return std::nullopt;
    case MoltFileKind::kNotRegular:
      LOG(WARNING) << "[Automation] skipped " << path.BaseName().AsUTF8Unsafe()
                   << ": not a regular file";
      return std::nullopt;
    case MoltFileKind::kTooBig:
      LOG(WARNING) << "[Automation] skipped " << path.BaseName().AsUTF8Unsafe()
                   << ": over " << kMaxScriptFileBytes << " bytes";
      return std::nullopt;
    case MoltFileKind::kUnreadable:
      LOG(WARNING) << "[Automation] skipped " << path.BaseName().AsUTF8Unsafe()
                   << ": unreadable";
      return std::nullopt;
  }
  std::optional<base::Value> parsed =
      base::JSONReader::Read(read.contents, base::JSON_ALLOW_TRAILING_COMMAS);
  if (!parsed || !parsed->is_dict())
    return std::nullopt;
  return std::move(parsed->GetDict());
}

std::optional<Script> AutomationStorage::Load(const std::string& id) {
  BlockingScope blocking(*this);
  if (!IsValidScriptId(id))
    return std::nullopt;
  PrepareRecord();
  const base::FilePath path = GetScriptPath(id);
  std::optional<base::DictValue> dict = ReadScriptDict(path);
  if (!dict)
    return std::nullopt;
  // Migration: recordings made before secret steps stored password values
  // in plaintext. Scrub them and rewrite this same file so the plaintext
  // does not linger on disk (read again under the lock: the file as it is
  // now). The record is left as it was: the security digest is the same
  // scrubbed or not, so a file it vouched for still is, and one it didn't
  // (a planted file with a password in it) still isn't.
  if (AutomationSecurity::ScrubPlaintextSecrets(*dict)) {
    ChangeScope locked(*this);
    std::optional<base::DictValue> now = ReadScriptDict(path);
    if (!now)
      return std::nullopt;
    if (AutomationSecurity::ScrubPlaintextSecrets(*now) &&
        WriteScriptJsonLocked(path, *now)) {
      LOG(WARNING) << "[Automation] removed stored secret value(s) from "
                   << id << kScriptExt;
      AppendAudit(id, "secrets_scrubbed", "");
    }
    return Vetted(std::move(*now), id, path, Held::kChangeScope);
  }
  return Vetted(std::move(*dict), id, path, Held::kNothing);
}

std::optional<Script> AutomationStorage::Vetted(base::DictValue dict,
                                                const std::string& id,
                                                const base::FilePath& path,
                                                Held held) {
  std::optional<Script> script = Script::FromJSON(dict);
  if (!script) {
    return std::nullopt;
  }
  // Known by its file's name, whatever the file says (one put back beside
  // another of its id, one copied by hand): never two listed under one id,
  // nor a save of one written over the other.
  script->id = id;
  WorkflowRecord* record = Record();
  if (!record) {
    return script;
  }
  const std::string name = NameOf(path);
  const bool vouched =
      record->Vouches(name, AutomationSecurity::SecurityDigest(*script));
  if (!vouched) {
    if (held == Held::kNothing) {
      // A change of this browser's in progress (the file renamed, the
      // record not set yet, or the reverse) ends before this second look.
      DCHECK_EQ(g_change_scope_depth, 0);
      base::AutoLock hold(DirectoryLock(GetAutomationsDir()));
      std::optional<base::DictValue> again = ReadScriptDict(path);
      if (!again) {
        return std::nullopt;
      }
      AutomationSecurity::ScrubPlaintextSecrets(*again);
      return Vetted(std::move(*again), id, path, Held::kChangeScope);
    }
    // Changed outside this browser: an unconfirmed import.
    AutomationSecurity::ClampUnvouched(*script);
  }
  // Its last fire time (bookkeeping, outside the digest): the file's own
  // only when the record vouches for the file and it can be a fire since
  // this browser last wrote there, else what this browser wrote or fired
  // there, or, still to come, when this browser first read it
  // (WorkflowRecord::LastFired()). A forged earlier one would force a
  // catch-up run; a later one believed would stall the schedule.
  script->trigger.last_fired_unix =
      record->LastFired(name, vouched ? script->trigger.last_fired_unix : 0);
  return script;
}

WorkflowRecord* AutomationStorage::Record() const {
  const base::FilePath dir = GetAutomationsDir();
  if (WorkflowRecord* record = WorkflowRecord::For(dir)) {
    return record;
  }
  // The shared directory without a configured record (tests, a fixture's
  // saves before the browser started): this session's writes only.
  if (dir == SharedDirectory()) {
    return WorkflowRecord::SessionOnlyFor(dir);
  }
  // A storage of its own directory (a profile's routines, a test's): as
  // the files are.
  return nullptr;
}

void AutomationStorage::PrepareRecord() {
  DCHECK_EQ(g_change_scope_depth, 0);
  WorkflowRecord* record = Record();
  if (!record || record->loaded()) {
    return;
  }
  BlockingScope blocking(*this);
  ChangeScope locked(*this);
  if (record->loaded()) {
    return;
  }
  const base::FilePath dir = GetAutomationsDir();
  record->LoadLocked(
      [this] { return PresentNamesLocked(); },
      [this,
       &dir](const std::string& name) -> std::optional<WorkflowRecord::Entry> {
        std::optional<base::DictValue> dict =
            ReadScriptDict(dir.Append(base::FilePath::FromUTF8Unsafe(name)));
        if (!dict) {
          return std::nullopt;
        }
        AutomationSecurity::ScrubPlaintextSecrets(*dict);
        std::optional<Script> script = Script::FromJSON(*dict);
        if (!script) {
          return std::nullopt;
        }
        WorkflowRecord::Entry entry;
        entry.digest = AutomationSecurity::SecurityDigest(*script);
        entry.fired = std::max<int64_t>(0, script->trigger.last_fired_unix);
        if (entry.digest.empty()) {
          return std::nullopt;
        }
        return entry;
      },
      [this](const std::string& event, const std::string& extra) {
        AppendAudit("-", event, extra);
      });
}

std::set<std::string> AutomationStorage::PresentNamesLocked() const {
  std::set<std::string> names;
  base::FileEnumerator e(GetAutomationsDir(), /*recursive=*/false,
                         base::FileEnumerator::FILES);
  const std::string_view ext(kScriptExt);
  for (base::FilePath path = e.Next(); !path.empty(); path = e.Next()) {
    const std::string name = NameOf(path);
    const bool script =
        name.size() > ext.size() &&
        std::string_view(name).substr(name.size() - ext.size()) == ext &&
        IsValidScriptId(name.substr(0, name.size() - ext.size()));
    if (!script && !SetAsideIdOf(name)) {
      continue;
    }
    // Never a link, a FIFO or a device.
    if (MoltFileKindAt(path, MoltFileLinks::kNotFollowed) !=
        MoltFileKind::kRegular) {
      continue;
    }
    names.insert(name);
  }
  return names;
}

bool AutomationStorage::Save(const Script& script) {
  BlockingScope blocking(*this);
  if (!IsValidScriptId(script.id))
    return false;
  PrepareRecord();
  if (!EnsureDirectory())
    return false;
  bool saved = false;
  {
    ChangeScope locked(*this);
    saved = SaveLocked(script);
  }
  if (saved)
    AppendAudit(script.id, "saved", "");
  return saved;
}

std::optional<std::string> AutomationStorage::SaveNew(Script script) {
  BlockingScope blocking(*this);
  if (!IsValidScriptId(script.id))
    return std::nullopt;
  PrepareRecord();
  if (!EnsureDirectory())
    return std::nullopt;
  bool saved = false;
  {
    ChangeScope locked(*this);
    // An id nothing is saved under (nor anything else is at).
    std::optional<std::string> id = FreeIdLocked(script.id);
    if (!id)
      return std::nullopt;
    script.id = std::move(*id);
    saved = SaveLocked(script);
  }
  if (!saved)
    return std::nullopt;
  AppendAudit(script.id, "saved", "");
  return script.id;
}

bool AutomationStorage::SaveChecked(Script script, SaveCheck check) {
  BlockingScope blocking(*this);
  if (!IsValidScriptId(script.id))
    return false;
  PrepareRecord();
  if (!EnsureDirectory())
    return false;
  const std::string id = script.id;
  bool saved = false;
  {
    ChangeScope locked(*this);
    const base::FilePath path = GetScriptPath(id);
    std::optional<base::DictValue> dict = ReadScriptDict(path);
    // Known by its file's name, whatever the file says; checked against
    // the record (an unvouched one is an unconfirmed import).
    std::optional<Script> stored =
        dict ? Vetted(std::move(*dict), id, path, Held::kChangeScope)
             : std::nullopt;
    if (!check(stored ? &*stored : nullptr, script) || script.id != id)
      return false;
    saved = SaveLocked(script);
  }
  if (saved)
    AppendAudit(id, "saved", "");
  return saved;
}

bool AutomationStorage::SaveLocked(const Script& script) {
  // Never persist a password / card / one-time-code value, whichever UI or
  // import path produced the script.
  base::DictValue json = script.ToJSON();
  if (AutomationSecurity::ScrubPlaintextSecrets(json))
    LOG(WARNING) << "[Automation] dropped secret value(s) before saving";
  const base::FilePath path = GetScriptPath(script.id);
  const std::string bytes = SerializeScript(json);
  // The record first, then the file (as a move copies its entry before its
  // rename): a browser that ends in between leaves the file as it was,
  // which the record no longer vouches for (one review), never an older
  // version vouched for again. What this browser writes, as a reader will
  // parse it, its last fire time included (0 for a schedule saved afresh:
  // it starts over).
  WorkflowRecord* record = Record();
  const std::string name = NameOf(path);
  std::optional<WorkflowRecord::Entry> before;
  if (record) {
    before = record->EntryOf(name);
    std::optional<Script> written = ParseScriptBytes(bytes);
    record->Set(
        name,
        written ? AutomationSecurity::SecurityDigest(*written) : std::string(),
        written ? written->trigger.last_fired_unix : 0);
  }
  if (before_file_change_for_testing_) {
    before_file_change_for_testing_.Run();
  }
  if (!WriteScriptBytesLocked(path, bytes)) {
    // Not written: the record as it was.
    if (record) {
      record->Restore(name, before);
    }
    return false;
  }
  return true;
}

bool AutomationStorage::Update(const std::string& script_id,
                               ScriptChange change) {
  BlockingScope blocking(*this);
  // A copy: |script_id| may be a field of what |change| changes.
  const std::string id = script_id;
  if (!IsValidScriptId(id))
    return false;
  PrepareRecord();
  bool saved = false;
  {
    ChangeScope locked(*this);
    const base::FilePath path = GetScriptPath(id);
    std::optional<base::DictValue> dict = ReadScriptDict(path);
    // Known by its file's name, whatever the file says; checked against
    // the record (an unvouched one is an unconfirmed import).
    std::optional<Script> script =
        dict ? Vetted(std::move(*dict), id, path, Held::kChangeScope)
             : std::nullopt;
    if (!script)
      return false;
    if (!change(*script) || script->id != id)
      return false;
    saved = SaveLocked(*script);
  }
  if (saved)
    AppendAudit(id, "saved", "");
  return saved;
}

bool AutomationStorage::UpdateBookkeeping(const std::string& script_id,
                                          ScriptChange change) {
  BlockingScope blocking(*this);
  // A copy: |script_id| may be a field of what |change| changes.
  const std::string id = script_id;
  if (!IsValidScriptId(id))
    return false;
  PrepareRecord();
  ChangeScope locked(*this);
  return UpdateBookkeepingLocked(id, change);
}

bool AutomationStorage::RecordFire(const std::string& id, int64_t fired_unix) {
  BlockingScope blocking(*this);
  if (!IsValidScriptId(id))
    return false;
  PrepareRecord();
  ChangeScope locked(*this);
  // First: a file that can't be written now still never fires again for
  // this time.
  if (WorkflowRecord* record = Record()) {
    record->NoteFired(id + kScriptExt, fired_unix);
  }
  return UpdateBookkeepingLocked(id, [fired_unix](Script& script) {
    script.trigger.last_fired_unix = fired_unix;
    return true;
  });
}

bool AutomationStorage::UpdateBookkeepingLocked(const std::string& id,
                                                ScriptChange change) {
  const base::FilePath path = GetScriptPath(id);
  std::optional<base::DictValue> dict = ReadScriptDict(path);
  // As the file is: never clamped, so what is written back is the file's
  // own security content.
  std::optional<Script> script = dict ? Script::FromJSON(*dict) : std::nullopt;
  if (!script)
    return false;
  script->id = id;
  const std::string before = AutomationSecurity::SecurityDigest(*script);
  if (before.empty() || !change(*script) || script->id != id)
    return false;
  base::DictValue json = script->ToJSON();
  AutomationSecurity::ScrubPlaintextSecrets(json);
  const std::string bytes = SerializeScript(json);
  // Exactly what a reader will parse: written only if its security content
  // is what it was (else this would vouch, or unvouch, by accident).
  std::optional<Script> written = ParseScriptBytes(bytes);
  if (!written || AutomationSecurity::SecurityDigest(*written) != before) {
    LOG(ERROR) << "[Automation] " << id << kScriptExt
               << " not saved: bookkeeping may not change the workflow";
    return false;
  }
  // The record is left as it was: a fire time read from the file is never
  // recorded (RecordFire() records the scheduler's own first).
  return WriteScriptBytesLocked(path, bytes);
}

// static
std::string AutomationStorage::SerializeScript(
    const base::DictValue& script_json) {
  std::string json;
  base::JSONWriter::WriteWithOptions(
      script_json, base::JSONWriter::OPTIONS_PRETTY_PRINT, &json);
  return json;
}

bool AutomationStorage::WriteScriptJsonLocked(
    const base::FilePath& final_path,
    const base::DictValue& script_json) {
  return WriteScriptBytesLocked(final_path, SerializeScript(script_json));
}

bool AutomationStorage::WriteScriptBytesLocked(const base::FilePath& final_path,
                                               const std::string& json) {
  // Never a file the readers would skip.
  if (json.size() > kMaxScriptFileBytes) {
    LOG(ERROR) << "[Automation] not saved: over " << kMaxScriptFileBytes
               << " bytes";
    return false;
  }
  // Never over a link, a FIFO or a directory of that name.
  if (!MoltFileWritePath(final_path, MoltFileLinks::kNotFollowed)) {
    LOG(ERROR) << "[Automation] not saved over "
               << final_path.BaseName().AsUTF8Unsafe()
               << ": not a regular file";
    return false;
  }
  // A temporary file of its own name in the same directory, then a rename:
  // never half a file, and no fixed temporary name to be in the way.
  if (!base::ImportantFileWriter::WriteFileAtomically(final_path, json)) {
    LOG(ERROR) << "[Automation] Failed to write "
               << final_path.BaseName().AsUTF8Unsafe();
    return false;
  }
  return true;
}

bool AutomationStorage::Delete(const std::string& id) {
  BlockingScope blocking(*this);
  if (!IsValidScriptId(id))
    return false;
  PrepareRecord();
  bool deleted_script = false;
  {
    ChangeScope locked(*this);
    // Forgotten first, and even if it isn't deleted: put back, or left
    // there by a browser that ends before it is deleted, it needs a review.
    if (WorkflowRecord* record = Record()) {
      record->Forget(id + kScriptExt);
    }
    if (before_file_change_for_testing_) {
      before_file_change_for_testing_.Run();
    }
    deleted_script = base::DeleteFile(GetScriptPath(id));
  }
  base::DeletePathRecursively(ArtifactsDir(id));
  AppendAudit(id, "deleted", "");
  return deleted_script;
}

std::optional<Script> AutomationStorage::Take(const std::string& id) {
  BlockingScope blocking(*this);
  if (!IsValidScriptId(id) || taken_.count(id))
    return std::nullopt;
  PrepareRecord();
  // A name of this Take()'s own: never over another set-aside file (of a
  // move another browser is making, or one cut short).
  const SetAside set_aside{id, NewSetAsidePath(id)};
  if (!Taking::Get().Start(set_aside.file))
    return std::nullopt;
  ChangeScope locked(*this);
  // Only a script's regular file: never a link (renaming one would move
  // the link, and reading it would read whatever it points to).
  if (MoltFileKindAt(GetScriptPath(id), MoltFileLinks::kNotFollowed) !=
          MoltFileKind::kRegular ||
      MoltFileKindAt(set_aside.file, MoltFileLinks::kNotFollowed) !=
          MoltFileKind::kMissing) {
    Taking::Get().Stop(set_aside.file);
    return std::nullopt;
  }
  // Its record goes with it: under the new name first, so that whichever
  // name holds the file if the browser ends mid-move is vouched for (the
  // other entry is dropped at the next load).
  WorkflowRecord* record = Record();
  const bool copied =
      record && record->Copy(id + kScriptExt, NameOf(set_aside.file));
  // One rename: a second taker (another profile, another browser on the
  // same home directory) finds the file gone.
  if (!base::ReplaceFile(GetScriptPath(id), set_aside.file, nullptr)) {
    if (copied) {
      record->Forget(NameOf(set_aside.file));
    }
    Taking::Get().Stop(set_aside.file);
    return std::nullopt;
  }
  if (record) {
    record->Forget(id + kScriptExt);
  }
  // Set aside now: InterruptedTakes() leaves it to its taker for a while.
  const base::Time now = base::Time::Now();
  base::TouchFile(set_aside.file, now, now);
  std::optional<base::DictValue> parsed = ReadScriptDict(set_aside.file);
  std::optional<Script> script;
  if (parsed) {
    AutomationSecurity::ScrubPlaintextSecrets(*parsed);
    // Known by its file's name, whatever the file says; checked against
    // the record.
    script = Vetted(std::move(*parsed), id, set_aside.file, Held::kChangeScope);
  }
  if (!script) {
    ReturnSetAsideLocked(set_aside);
    return std::nullopt;
  }
  taken_[id] = set_aside.file;
  return script;
}

void AutomationStorage::FinishTake(const std::string& id) {
  auto it = taken_.find(id);
  if (it == taken_.end())
    return;
  const SetAside set_aside{id, it->second};
  taken_.erase(it);
  FinishSetAside(set_aside);
}

bool AutomationStorage::ReturnTaken(const std::string& id) {
  auto it = taken_.find(id);
  if (it == taken_.end())
    return false;
  const SetAside set_aside{id, it->second};
  taken_.erase(it);
  return ReturnSetAside(set_aside);
}

void AutomationStorage::FinishSetAside(const SetAside& set_aside) {
  BlockingScope blocking(*this);
  if (!IsSetAsideHere(set_aside))
    return;
  PrepareRecord();
  {
    ChangeScope locked(*this);
    // Forgotten first, as Delete() does.
    if (WorkflowRecord* record = Record()) {
      record->Forget(NameOf(set_aside.file));
    }
    if (MoltFileKindAt(set_aside.file, MoltFileLinks::kNotFollowed) ==
        MoltFileKind::kRegular) {
      base::DeleteFile(set_aside.file);
    }
  }
  Taking::Get().Stop(set_aside.file);
  AppendAudit(set_aside.id, "moved", "");
}

bool AutomationStorage::ReturnSetAside(const SetAside& set_aside) {
  BlockingScope blocking(*this);
  if (!IsSetAsideHere(set_aside))
    return false;
  PrepareRecord();
  ChangeScope locked(*this);
  return ReturnSetAsideLocked(set_aside);
}

bool AutomationStorage::ReturnSetAsideLocked(const SetAside& set_aside) {
  const std::string& id = set_aside.id;
  bool returned = false;
  if (MoltFileKindAt(set_aside.file, MoltFileLinks::kNotFollowed) ==
      MoltFileKind::kRegular) {
    // Back under its id, or a free one made from it: never over a script
    // saved under that id meanwhile.
    std::optional<std::string> back = FreeIdLocked(id);
    if (back) {
      // Its record with it, first under the name it gets back, as Take()
      // does. A set-aside file the record has nothing for (unvouched, or
      // set aside without this browser's record: another browser's move, a
      // rename cut short) leaves the record of <back>.molt as it was, kept
      // for it while it was set aside (workflow_record.h).
      WorkflowRecord* record = Record();
      const std::string to = *back + kScriptExt;
      const bool copied = record && record->Copy(NameOf(set_aside.file), to);
      returned =
          base::ReplaceFile(set_aside.file, GetScriptPath(*back), nullptr);
      if (record && returned) {
        record->Forget(NameOf(set_aside.file));
      } else if (copied) {
        record->Forget(to);
      }
      if (returned && *back != id) {
        LOG(WARNING) << "[Automation] "
                     << set_aside.file.BaseName().AsUTF8Unsafe()
                     << " returned as " << *back << kScriptExt;
      }
    }
  }
  Taking::Get().Stop(set_aside.file);
  return returned;
}

std::optional<std::string> AutomationStorage::FreeIdLocked(
    const std::string& id) const {
  const auto is_free = [this](const std::string& candidate) {
    return MoltFileKindAt(GetScriptPath(candidate),
                          MoltFileLinks::kNotFollowed) ==
           MoltFileKind::kMissing;
  };
  std::string simple = id;
  while (true) {
    if (is_free(simple))
      return simple;
    if (simple.size() >= kMaxIdBytes)
      break;
    simple += '_';
  }
  // Every simple name taken (by scripts, or by anything else): a fresh one,
  // "<id>_<16 hex digits>", the id cut to fit. A few tries: two random
  // suffixes alike are next to impossible.
  constexpr size_t kSuffixBytes = 17;
  const std::string prefix = IdPrefix(id, kMaxIdBytes - kSuffixBytes);
  for (int attempt = 0; attempt < 4; ++attempt) {
    const std::string fresh = prefix + "_" + RandomToken();
    if (IsValidScriptId(fresh) && is_free(fresh))
      return fresh;
  }
  return std::nullopt;
}

std::vector<AutomationStorage::SetAside> AutomationStorage::InterruptedTakes() {
  BlockingScope blocking(*this);
  std::vector<SetAside> out;
  const base::FilePath dir = GetAutomationsDir();
  if (!base::DirectoryExists(dir))
    return out;
  const base::Time now = base::Time::Now();
  base::FileEnumerator e(dir, /*recursive=*/false, base::FileEnumerator::FILES,
                         FILE_PATH_LITERAL("*.taken"));
  for (base::FilePath path = e.Next(); !path.empty(); path = e.Next()) {
    std::optional<std::string> id =
        SetAsideIdOf(path.BaseName().AsUTF8Unsafe());
    base::File::Info info;
    if (!id || Taking::Get().Has(path) ||
        MoltFileKindAt(path, MoltFileLinks::kNotFollowed) !=
            MoltFileKind::kRegular ||
        !base::GetFileInfo(path, &info) ||
        MayStillBeMoving(info.last_modified, now)) {
      continue;
    }
    out.push_back({*id, path});
  }
  return out;
}

std::optional<Script> AutomationStorage::ReadSetAside(
    const SetAside& set_aside) {
  BlockingScope blocking(*this);
  if (!IsSetAsideHere(set_aside))
    return std::nullopt;
  PrepareRecord();
  std::optional<base::DictValue> parsed = ReadScriptDict(set_aside.file);
  if (!parsed)
    return std::nullopt;
  AutomationSecurity::ScrubPlaintextSecrets(*parsed);
  return Vetted(std::move(*parsed), set_aside.id, set_aside.file,
                Held::kNothing);
}

bool AutomationStorage::SetTrustLevel(const std::string& id,
                                      TrustLevel level) {
  TrustLevel before = level;
  if (!Update(id, [&](Script& script) {
        // Never a workflow that needs a review first (an unconfirmed draft,
        // one changed outside the browser): both UIs refuse it too.
        if (script.security.sites_unconfirmed) {
          return false;
        }
        before = script.security.trust;
        script.security.trust = level;
        return true;
      })) {
    return false;
  }
  AppendAudit(id, "trust_changed",
              TrustLevelToString(before) + " -> " + TrustLevelToString(level));
  return true;
}

void AutomationStorage::AppendAudit(const std::string& script_id,
                                     const std::string& event,
                                     const std::string& extra) {
  BlockingScope blocking(*this);
  if (!EnsureDirectory())
    return;
  std::string line =
      NowISO8601() + " | " + script_id + " | " + event;
  if (!extra.empty())
    line += " | " + extra;
  line += "\n";

  // Created when missing (base::AppendToFile() isn't), and only ever a
  // regular file: never through a link, never waiting on a FIFO.
  if (!AppendToMoltFile(GetAuditLogPath(), line)) {
    LOG(WARNING) << "[Automation] audit.log not written: not a regular file";
  }
}

std::vector<std::string> AutomationStorage::ReadAuditTail(int max_lines) {
  BlockingScope blocking(*this);
  std::vector<std::string> out;
  std::optional<std::string> contents =
      ReadMoltFileTail(GetAuditLogPath(), kMaxAuditTailBytes);
  if (!contents)
    return out;
  out = base::SplitString(*contents, "\n", base::KEEP_WHITESPACE,
                          base::SPLIT_WANT_NONEMPTY);
  if (static_cast<int>(out.size()) > max_lines)
    out.erase(out.begin(), out.end() - max_lines);
  return out;
}

base::FilePath AutomationStorage::EnsureArtifactsDir(
    const std::string& script_id) {
  BlockingScope blocking(*this);
  base::FilePath dir = ArtifactsDir(script_id);
  if (!base::DirectoryExists(dir))
    base::CreateDirectory(dir);
  return dir;
}

}  // namespace automation
}  // namespace molt_ai
