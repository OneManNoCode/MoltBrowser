// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/automation_storage.h"

#include <ctime>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <utility>

#include "base/base_paths.h"
#include "base/files/file.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/synchronization/lock.h"
#include "base/task/lazy_thread_pool_task_runner.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/task_traits.h"
#include "base/threading/scoped_blocking_call.h"
#include "build/build_config.h"
#include "chrome/browser/molt_ai/automation/automation_security.h"
#include "chrome/browser/molt_ai/common/molt_blocking_scope.h"
#include "chrome/browser/molt_ai/common/molt_regular_file.h"

namespace molt_ai {
namespace automation {

namespace {

constexpr char kAutomationsDirName[] = ".moltbrowser";
constexpr char kAutomationsSubdir[] = "automations";
constexpr char kArtifactsSubdir[] = "artifacts";
constexpr char kAuditLogName[] = "audit.log";
constexpr char kScriptExt[] = ".molt";
// A script taken to be moved (Take()): no longer listed or loaded.
constexpr char kTakenExt[] = ".taken";
// A script id, at most.
constexpr size_t kMaxIdBytes = 128;

// The Agent studio's sequence for the shared storage (made lazily, and per
// test environment in unit tests). A change posted before shutdown is
// written.
base::LazyThreadPoolSequencedTaskRunner g_shared_sequence =
    LAZY_THREAD_POOL_SEQUENCED_TASK_RUNNER_INITIALIZER(
        base::TaskTraits(base::MayBlock(),
                         base::TaskPriority::USER_VISIBLE,
                         base::TaskShutdownBehavior::BLOCK_SHUTDOWN));

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

// Whether a script set aside at |set_aside| may still be moving in another
// browser (within the grace period, by this clock).
bool MayStillBeMoving(base::Time set_aside, base::Time now) {
  const base::TimeDelta age = now - set_aside;
  return !age.is_negative() && age < kTakenGracePeriod;
}

}  // namespace

class AutomationStorage::BlockingScope {
 public:
  explicit BlockingScope(const AutomationStorage& storage) {
    if (storage.directory_.empty()) {
      // The callers still on the UI thread.
      allow_.emplace();
    }
    // Checks the sequence may block (a storage made with a directory).
    check_.emplace(FROM_HERE, base::BlockingType::MAY_BLOCK);
  }
  BlockingScope(const BlockingScope&) = delete;
  BlockingScope& operator=(const BlockingScope&) = delete;

 private:
  std::optional<ScopedAllowBlockingForMolt> allow_;
  std::optional<base::ScopedBlockingCall> check_;
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
scoped_refptr<base::SequencedTaskRunner> AutomationStorage::SharedSequence() {
  return g_shared_sequence.Get();
}

base::FilePath AutomationStorage::GetRoot() const {
  if (!root_override_.empty())
    return root_override_;
  base::FilePath home;
  base::PathService::Get(base::DIR_HOME, &home);
  return home.Append(base::FilePath::FromUTF8Unsafe(kAutomationsDirName));
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

base::FilePath AutomationStorage::GetTakenPath(const std::string& id) const {
  return GetAutomationsDir().Append(
      base::FilePath::FromUTF8Unsafe(id + kScriptExt + kTakenExt));
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
  const base::FilePath path = GetScriptPath(id);
  std::optional<base::DictValue> dict = ReadScriptDict(path);
  if (!dict)
    return std::nullopt;
  // Migration: recordings made before secret steps stored password values
  // in plaintext. Scrub them and rewrite this same file so the plaintext
  // does not linger on disk (read again under the lock: the file as it is
  // now).
  if (AutomationSecurity::ScrubPlaintextSecrets(*dict)) {
    base::AutoLock hold(DirectoryLock(GetAutomationsDir()));
    std::optional<base::DictValue> now = ReadScriptDict(path);
    if (!now)
      return std::nullopt;
    if (AutomationSecurity::ScrubPlaintextSecrets(*now) &&
        WriteScriptJsonLocked(path, *now)) {
      LOG(WARNING) << "[Automation] removed stored secret value(s) from "
                   << id << kScriptExt;
      AppendAudit(id, "secrets_scrubbed", "");
    }
    dict = std::move(now);
  }
  return Script::FromJSON(*dict);
}

bool AutomationStorage::Save(const Script& script) {
  BlockingScope blocking(*this);
  if (!IsValidScriptId(script.id))
    return false;
  if (!EnsureDirectory())
    return false;
  bool saved = false;
  {
    base::AutoLock hold(DirectoryLock(GetAutomationsDir()));
    saved = SaveLocked(script);
  }
  if (saved)
    AppendAudit(script.id, "saved", "");
  return saved;
}

bool AutomationStorage::SaveLocked(const Script& script) {
  // Never persist a password / card / one-time-code value, whichever UI or
  // import path produced the script.
  base::DictValue json = script.ToJSON();
  if (AutomationSecurity::ScrubPlaintextSecrets(json))
    LOG(WARNING) << "[Automation] dropped secret value(s) before saving";
  return WriteScriptJsonLocked(GetScriptPath(script.id), json);
}

bool AutomationStorage::Update(const std::string& id, ScriptChange change) {
  BlockingScope blocking(*this);
  if (!IsValidScriptId(id))
    return false;
  bool saved = false;
  {
    base::AutoLock hold(DirectoryLock(GetAutomationsDir()));
    std::optional<base::DictValue> dict = ReadScriptDict(GetScriptPath(id));
    std::optional<Script> script =
        dict ? Script::FromJSON(*dict) : std::nullopt;
    if (!script)
      return false;
    // Known by its file's name, whatever the file says.
    script->id = id;
    if (!change(*script) || script->id != id)
      return false;
    saved = SaveLocked(*script);
  }
  if (saved)
    AppendAudit(id, "saved", "");
  return saved;
}

bool AutomationStorage::WriteScriptJsonLocked(
    const base::FilePath& final_path,
    const base::DictValue& script_json) {
  std::string json;
  base::JSONWriter::WriteWithOptions(
      script_json, base::JSONWriter::OPTIONS_PRETTY_PRINT, &json);
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
  bool deleted_script = false;
  {
    base::AutoLock hold(DirectoryLock(GetAutomationsDir()));
    deleted_script = base::DeleteFile(GetScriptPath(id));
  }
  base::DeletePathRecursively(ArtifactsDir(id));
  AppendAudit(id, "deleted", "");
  return deleted_script;
}

std::optional<Script> AutomationStorage::Take(const std::string& id) {
  BlockingScope blocking(*this);
  if (!IsValidScriptId(id))
    return std::nullopt;
  const base::FilePath taken = GetTakenPath(id);
  if (!Taking::Get().Start(taken))
    return std::nullopt;
  base::AutoLock hold(DirectoryLock(GetAutomationsDir()));
  // Only a script's regular file, and never over one set aside before (a
  // move another browser is making, or one cut short, which
  // InterruptedTakes() reports).
  if (MoltFileKindAt(GetScriptPath(id), MoltFileLinks::kNotFollowed) !=
          MoltFileKind::kRegular ||
      MoltFileKindAt(taken, MoltFileLinks::kNotFollowed) !=
          MoltFileKind::kMissing) {
    Taking::Get().Stop(taken);
    return std::nullopt;
  }
  // One rename: a second taker (another profile, another browser on the
  // same home directory) finds the file gone.
  if (!base::ReplaceFile(GetScriptPath(id), taken, nullptr)) {
    Taking::Get().Stop(taken);
    return std::nullopt;
  }
  // Set aside now: InterruptedTakes() leaves it to its taker for a while.
  const base::Time now = base::Time::Now();
  base::TouchFile(taken, now, now);
  std::optional<base::DictValue> parsed = ReadScriptDict(taken);
  std::optional<Script> script;
  if (parsed) {
    AutomationSecurity::ScrubPlaintextSecrets(*parsed);
    script = Script::FromJSON(*parsed);
  }
  if (!script) {
    ReturnTakenLocked(id);
    return std::nullopt;
  }
  // Known by its file's name, whatever the file says.
  script->id = id;
  return script;
}

void AutomationStorage::FinishTake(const std::string& id) {
  BlockingScope blocking(*this);
  if (!IsValidScriptId(id))
    return;
  const base::FilePath taken = GetTakenPath(id);
  {
    base::AutoLock hold(DirectoryLock(GetAutomationsDir()));
    base::DeleteFile(taken);
  }
  Taking::Get().Stop(taken);
  AppendAudit(id, "moved", "");
}

bool AutomationStorage::ReturnTaken(const std::string& id) {
  BlockingScope blocking(*this);
  if (!IsValidScriptId(id))
    return false;
  base::AutoLock hold(DirectoryLock(GetAutomationsDir()));
  return ReturnTakenLocked(id);
}

bool AutomationStorage::ReturnTakenLocked(const std::string& id) {
  const base::FilePath taken = GetTakenPath(id);
  bool returned = false;
  if (MoltFileKindAt(taken, MoltFileLinks::kNotFollowed) ==
      MoltFileKind::kRegular) {
    // Back under its id, or the first free one after it: never over a
    // script saved under that id meanwhile.
    std::string back = id;
    while (MoltFileKindAt(GetScriptPath(back), MoltFileLinks::kNotFollowed) !=
               MoltFileKind::kMissing &&
           back.size() < kMaxIdBytes) {
      back += '_';
    }
    if (MoltFileKindAt(GetScriptPath(back), MoltFileLinks::kNotFollowed) ==
        MoltFileKind::kMissing) {
      returned = base::ReplaceFile(taken, GetScriptPath(back), nullptr);
      if (returned && back != id) {
        LOG(WARNING) << "[Automation] " << id << kScriptExt << kTakenExt
                     << " returned as " << back << kScriptExt;
      }
    }
  }
  Taking::Get().Stop(taken);
  return returned;
}

std::vector<std::string> AutomationStorage::InterruptedTakes() {
  BlockingScope blocking(*this);
  std::vector<std::string> out;
  const base::FilePath dir = GetAutomationsDir();
  if (!base::DirectoryExists(dir))
    return out;
  const std::string suffix = std::string(kScriptExt) + kTakenExt;
  const base::Time now = base::Time::Now();
  base::FileEnumerator e(dir, /*recursive=*/false, base::FileEnumerator::FILES,
                         FILE_PATH_LITERAL("*.molt.taken"));
  for (base::FilePath path = e.Next(); !path.empty(); path = e.Next()) {
    const std::string name = path.BaseName().AsUTF8Unsafe();
    if (name.size() <= suffix.size())
      continue;
    const std::string id = name.substr(0, name.size() - suffix.size());
    base::File::Info info;
    if (!IsValidScriptId(id) || Taking::Get().Has(path) ||
        MoltFileKindAt(path, MoltFileLinks::kNotFollowed) !=
            MoltFileKind::kRegular ||
        !base::GetFileInfo(path, &info) ||
        MayStillBeMoving(info.last_modified, now)) {
      continue;
    }
    out.push_back(id);
  }
  return out;
}

std::optional<Script> AutomationStorage::ReadTaken(const std::string& id) {
  BlockingScope blocking(*this);
  if (!IsValidScriptId(id))
    return std::nullopt;
  std::optional<base::DictValue> parsed = ReadScriptDict(GetTakenPath(id));
  if (!parsed)
    return std::nullopt;
  AutomationSecurity::ScrubPlaintextSecrets(*parsed);
  std::optional<Script> script = Script::FromJSON(*parsed);
  if (script)
    script->id = id;
  return script;
}

bool AutomationStorage::SetTrustLevel(const std::string& id,
                                      TrustLevel level) {
  TrustLevel before = level;
  if (!Update(id, [&](Script& script) {
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
