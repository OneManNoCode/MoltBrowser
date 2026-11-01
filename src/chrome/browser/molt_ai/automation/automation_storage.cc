// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/automation_storage.h"

#include <stdint.h>

#include <ctime>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string_view>
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
// A script taken to be moved (Take()): no longer listed or loaded. Set
// aside as <id>.molt.<token>.taken (round 43: <id>.molt.taken).
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

bool AutomationStorage::Update(const std::string& script_id,
                               ScriptChange change) {
  BlockingScope blocking(*this);
  // A copy: |script_id| may be a field of what |change| changes.
  const std::string id = script_id;
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
  if (!IsValidScriptId(id) || taken_.count(id))
    return std::nullopt;
  // A name of this Take()'s own: never over another set-aside file (of a
  // move another browser is making, or one cut short).
  const SetAside set_aside{id, NewSetAsidePath(id)};
  if (!Taking::Get().Start(set_aside.file))
    return std::nullopt;
  base::AutoLock hold(DirectoryLock(GetAutomationsDir()));
  // Only a script's regular file: never a link (renaming one would move
  // the link, and reading it would read whatever it points to).
  if (MoltFileKindAt(GetScriptPath(id), MoltFileLinks::kNotFollowed) !=
          MoltFileKind::kRegular ||
      MoltFileKindAt(set_aside.file, MoltFileLinks::kNotFollowed) !=
          MoltFileKind::kMissing) {
    Taking::Get().Stop(set_aside.file);
    return std::nullopt;
  }
  // One rename: a second taker (another profile, another browser on the
  // same home directory) finds the file gone.
  if (!base::ReplaceFile(GetScriptPath(id), set_aside.file, nullptr)) {
    Taking::Get().Stop(set_aside.file);
    return std::nullopt;
  }
  // Set aside now: InterruptedTakes() leaves it to its taker for a while.
  const base::Time now = base::Time::Now();
  base::TouchFile(set_aside.file, now, now);
  std::optional<base::DictValue> parsed = ReadScriptDict(set_aside.file);
  std::optional<Script> script;
  if (parsed) {
    AutomationSecurity::ScrubPlaintextSecrets(*parsed);
    script = Script::FromJSON(*parsed);
  }
  if (!script) {
    ReturnSetAsideLocked(set_aside);
    return std::nullopt;
  }
  taken_[id] = set_aside.file;
  // Known by its file's name, whatever the file says.
  script->id = id;
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
  {
    base::AutoLock hold(DirectoryLock(GetAutomationsDir()));
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
  base::AutoLock hold(DirectoryLock(GetAutomationsDir()));
  return ReturnSetAsideLocked(set_aside);
}

bool AutomationStorage::ReturnSetAsideLocked(const SetAside& set_aside) {
  const std::string& id = set_aside.id;
  bool returned = false;
  if (MoltFileKindAt(set_aside.file, MoltFileLinks::kNotFollowed) ==
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
      returned =
          base::ReplaceFile(set_aside.file, GetScriptPath(back), nullptr);
      if (returned && back != id) {
        LOG(WARNING) << "[Automation] "
                     << set_aside.file.BaseName().AsUTF8Unsafe()
                     << " returned as " << back << kScriptExt;
      }
    }
  }
  Taking::Get().Stop(set_aside.file);
  return returned;
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
  std::optional<base::DictValue> parsed = ReadScriptDict(set_aside.file);
  if (!parsed)
    return std::nullopt;
  AutomationSecurity::ScrubPlaintextSecrets(*parsed);
  std::optional<Script> script = Script::FromJSON(*parsed);
  if (script)
    script->id = set_aside.id;
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
