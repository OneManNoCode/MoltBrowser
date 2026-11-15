// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/workflow_record.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/memory/ptr_util.h"
#include "base/no_destructor.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/common/molt_home_dir.h"
#include "chrome/browser/molt_ai/common/molt_regular_file.h"

namespace molt_ai {
namespace automation {

namespace {

constexpr int kFormat = 1;
// A file name in the record, at most.
constexpr size_t kMaxNameBytes = 200;
// A digest: SHA-256 in lowercase hex.
constexpr size_t kDigestChars = 64;
constexpr char kMarkerContents[] = "1\n";

// The records, one per directory. Its lock is never held with a record's.
struct Slot {
  std::unique_ptr<WorkflowRecord> record;
  bool configured = false;
};
struct Registry {
  base::Lock lock;
  std::map<base::FilePath, Slot> slots;
};

Registry& GetRegistry() {
  static base::NoDestructor<Registry> registry;
  return *registry;
}

const char* OriginName(WorkflowRecord::Origin origin) {
  switch (origin) {
    case WorkflowRecord::Origin::kSeeded:
      return "seeded";
    case WorkflowRecord::Origin::kFresh:
      return "fresh";
    case WorkflowRecord::Origin::kAfterLoss:
      return "after_loss";
    case WorkflowRecord::Origin::kAfterCorrupt:
      return "after_corrupt";
    case WorkflowRecord::Origin::kOtherDirectory:
      return "other_directory";
    case WorkflowRecord::Origin::kSessionOnly:
      return "session_only";
  }
  return "fresh";
}

WorkflowRecord::Origin OriginFromName(const std::string& name) {
  for (WorkflowRecord::Origin origin :
       {WorkflowRecord::Origin::kSeeded, WorkflowRecord::Origin::kFresh,
        WorkflowRecord::Origin::kAfterLoss,
        WorkflowRecord::Origin::kAfterCorrupt,
        WorkflowRecord::Origin::kOtherDirectory}) {
    if (name == OriginName(origin)) {
      return origin;
    }
  }
  // Informational only.
  return WorkflowRecord::Origin::kFresh;
}

std::string NowIso8601() {
  base::Time::Exploded e;
  base::Time::Now().UTCExplode(&e);
  return base::StringPrintf("%04d-%02d-%02dT%02d:%02d:%02dZ", e.year, e.month,
                            e.day_of_month, e.hour, e.minute, e.second);
}

// Now, in unix seconds (as the scheduler counts fire times).
int64_t NowUnix() {
  return static_cast<int64_t>(base::Time::Now().ToTimeT());
}

// A time in the record: a non-negative number of seconds; nullopt for
// anything else.
std::optional<int64_t> RecordedSeconds(const base::Value* value) {
  if (!value) {
    return std::nullopt;
  }
  int64_t seconds = 0;
  if (value->is_int()) {
    seconds = value->GetInt();
  } else if (value->is_double()) {
    seconds = base::saturated_cast<int64_t>(value->GetDouble());
  } else {
    return std::nullopt;
  }
  if (seconds < 0) {
    return std::nullopt;
  }
  return seconds;
}

// Whether a script set aside from |name| ("<id>.molt") is among |names|
// (<id>.molt.<token>.taken: a script id has no dot).
bool IsSetAsideIn(const std::string& name, const std::set<std::string>& names) {
  if (!base::EndsWith(name, ".molt", base::CompareCase::SENSITIVE)) {
    return false;
  }
  const std::string prefix = name + ".";
  for (auto it = names.lower_bound(prefix);
       it != names.end() &&
       base::StartsWith(*it, prefix, base::CompareCase::SENSITIVE);
       ++it) {
    if (base::EndsWith(*it, ".taken", base::CompareCase::SENSITIVE)) {
      return true;
    }
  }
  return false;
}

// A script's file name in the directory, as the record keeps it: 1 to 200
// bytes, no separator, not hidden, a script's or a set-aside script's.
bool IsValidName(std::string_view name) {
  if (name.empty() || name.size() > kMaxNameBytes || name[0] == '.') {
    return false;
  }
  for (char c : name) {
    if (c == '/' || c == '\\' || c == ':' || c == '\0') {
      return false;
    }
  }
  return base::EndsWith(name, ".molt", base::CompareCase::SENSITIVE) ||
         base::EndsWith(name, ".taken", base::CompareCase::SENSITIVE);
}

bool IsValidDigest(std::string_view digest) {
  if (digest.size() != kDigestChars) {
    return false;
  }
  for (char c : digest) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

struct ParsedRecord {
  std::string directory;
  WorkflowRecord::Origin origin = WorkflowRecord::Origin::kFresh;
  std::string created;
  std::map<std::string, WorkflowRecord::Entry> files;
};

// The record file's contents, checked: nullopt when it isn't a record at
// all (corrupt); a malformed entry is dropped on its own.
std::optional<ParsedRecord> ParseRecord(std::string_view contents) {
  // Written by base::JSONWriter: plain JSON.
  std::optional<base::Value> value =
      base::JSONReader::Read(contents, base::JSON_PARSE_RFC);
  if (!value || !value->is_dict()) {
    return std::nullopt;
  }
  const base::DictValue& root = value->GetDict();
  if (root.FindInt("format") != kFormat) {
    return std::nullopt;
  }
  const std::string* directory = root.FindString("directory");
  const base::DictValue* files = root.FindDict("files");
  if (!directory || !files || files->size() > kMaxWorkflowRecordEntries) {
    return std::nullopt;
  }
  ParsedRecord parsed;
  parsed.directory = *directory;
  if (const std::string* origin = root.FindString("origin")) {
    parsed.origin = OriginFromName(*origin);
  }
  if (const std::string* created = root.FindString("created");
      created && created->size() <= 64) {
    parsed.created = *created;
  }
  for (const auto item : *files) {
    const std::string& name = item.first;
    if (!IsValidName(name) || !item.second.is_dict()) {
      continue;
    }
    const base::DictValue& entry = item.second.GetDict();
    const std::string* digest = entry.FindString("digest");
    if (!digest || !IsValidDigest(*digest)) {
      continue;
    }
    // "fired" is left out when 0; "written" is always there.
    std::optional<int64_t> fired = 0;
    if (const base::Value* value_fired = entry.Find("fired")) {
      fired = RecordedSeconds(value_fired);
    }
    const std::optional<int64_t> written =
        RecordedSeconds(entry.Find("written"));
    if (!fired || !written) {
      continue;
    }
    WorkflowRecord::Entry& kept = parsed.files[name];
    kept = {*digest, *fired, *written};
    // A pending fire time and when it was first read (LastFired()), left
    // out when there is none; a malformed pair is none.
    const std::optional<int64_t> pending =
        RecordedSeconds(entry.Find("pending"));
    const std::optional<int64_t> pending_seen =
        RecordedSeconds(entry.Find("pending_seen"));
    if (pending && pending_seen && *pending > 0) {
      kept.pending = *pending;
      kept.pending_seen = *pending_seen;
    }
  }
  return parsed;
}

// Whether |path| is |dir| or inside it, by name or once links are
// resolved.
bool IsInside(const base::FilePath& dir, const base::FilePath& path) {
  if (dir.empty()) {
    return false;
  }
  if (dir == path || dir.IsParent(path)) {
    return true;
  }
  const base::FilePath real_dir = base::MakeAbsoluteFilePath(dir);
  const base::FilePath real_parent = base::MakeAbsoluteFilePath(path.DirName());
  return !real_dir.empty() && !real_parent.empty() &&
         (real_dir == real_parent || real_dir.IsParent(real_parent));
}

// Writes |contents| to |path| atomically, only over nothing or a regular
// file (never through a link).
bool WriteRegularFile(const base::FilePath& path, std::string_view contents) {
  std::optional<base::FilePath> target =
      MoltFileWritePath(path, MoltFileLinks::kNotFollowed);
  if (!target) {
    LOG(ERROR) << "[Automation] not written over "
               << path.BaseName().AsUTF8Unsafe() << ": not a regular file";
    return false;
  }
  if (!base::ImportantFileWriter::WriteFileAtomically(*target, contents)) {
    LOG(ERROR) << "[Automation] could not write "
               << path.BaseName().AsUTF8Unsafe();
    return false;
  }
  return true;
}

}  // namespace

WorkflowRecord::WorkflowRecord(const base::FilePath& directory) {
  config_.directory = directory;
}

WorkflowRecord::~WorkflowRecord() = default;

// static
void WorkflowRecord::Configure(const Config& config) {
  Registry& registry = GetRegistry();
  WorkflowRecord* record = nullptr;
  {
    base::AutoLock hold(registry.lock);
    Slot& slot = registry.slots[config.directory];
    if (!slot.record) {
      slot.record = base::WrapUnique(new WorkflowRecord(config.directory));
    }
    record = slot.record.get();
  }
  record->Reconfigure(config);
  base::AutoLock hold(registry.lock);
  registry.slots[config.directory].configured = true;
}

// static
WorkflowRecord* WorkflowRecord::For(const base::FilePath& directory) {
  Registry& registry = GetRegistry();
  base::AutoLock hold(registry.lock);
  auto it = registry.slots.find(directory);
  if (it == registry.slots.end() || !it->second.configured) {
    return nullptr;
  }
  return it->second.record.get();
}

// static
WorkflowRecord* WorkflowRecord::SessionOnlyFor(
    const base::FilePath& directory) {
  Registry& registry = GetRegistry();
  base::AutoLock hold(registry.lock);
  Slot& slot = registry.slots[directory];
  if (!slot.record) {
    slot.record = base::WrapUnique(new WorkflowRecord(directory));
    LOG(WARNING) << "[Automation] no workflow record for "
                 << directory.AsUTF8Unsafe()
                 << ": only this session's writes there are trusted";
  }
  return slot.record.get();
}

// static
void WorkflowRecord::ResetForTesting() {
  Registry& registry = GetRegistry();
  base::AutoLock hold(registry.lock);
  registry.slots.clear();
}

// static
base::FilePath WorkflowRecord::MarkerFileFor(
    const base::FilePath& record_file) {
  return base::FilePath(record_file.value() + FILE_PATH_LITERAL(" Started"));
}

void WorkflowRecord::Reconfigure(const Config& config) {
  base::AutoLock hold(lock_);
  config_ = config;
  ++generation_;
  needs_load_ = true;
  session_only_ = false;
}

bool WorkflowRecord::loaded() const {
  base::AutoLock hold(lock_);
  return !needs_load_;
}

WorkflowRecord::Origin WorkflowRecord::origin() const {
  base::AutoLock hold(lock_);
  return origin_;
}

void WorkflowRecord::FinishSessionOnly(uint64_t generation) {
  base::AutoLock hold(lock_);
  if (generation != generation_) {
    return;
  }
  session_only_ = true;
  origin_ = Origin::kSessionOnly;
  needs_load_ = false;
}

void WorkflowRecord::LoadLocked(
    base::FunctionRef<std::set<std::string>()> present,
    base::FunctionRef<std::optional<Entry>(const std::string& name)> entry_of,
    base::FunctionRef<void(const std::string& event, const std::string& extra)>
        audit) {
  Config config;
  uint64_t generation = 0;
  {
    base::AutoLock hold(lock_);
    if (!needs_load_) {
      return;
    }
    config = config_;
    generation = generation_;
  }

  // Nowhere it can be kept, or somewhere what writes the workflows could
  // write it too: this session's writes only.
  if (config.record_file.empty()) {
    FinishSessionOnly(generation);
    return;
  }
  std::optional<base::FilePath> molt_home = MoltHomeDir();
  if (IsInside(config.directory, config.record_file) ||
      (molt_home && IsInside(*molt_home, config.record_file))) {
    LOG(ERROR) << "[Automation] the workflow record can't be kept in "
                  "MoltBrowser's data folder; only this session's writes "
                  "are trusted";
    FinishSessionOnly(generation);
    return;
  }

  const std::set<std::string> names = present();
  const base::FilePath marker = MarkerFileFor(config.record_file);
  const bool marker_missing =
      MoltFileKindAt(marker, MoltFileLinks::kNotFollowed) ==
      MoltFileKind::kMissing;
  MoltFileRead read = ReadMoltFile(config.record_file, kMaxWorkflowRecordBytes,
                                   MoltFileLinks::kNotFollowed);
  std::map<std::string, Entry> files;
  Origin origin = Origin::kFresh;
  std::string created;
  bool marker_after = false;
  switch (read.kind) {
    case MoltFileKind::kRegular:
    case MoltFileKind::kTooBig: {
      std::optional<ParsedRecord> parsed;
      if (read.kind == MoltFileKind::kRegular) {
        parsed = ParseRecord(read.contents);
      }
      if (parsed && parsed->directory == config.directory.AsUTF8Unsafe()) {
        files = std::move(parsed->files);
        origin = parsed->origin;
        created = std::move(parsed->created);
      } else if (parsed) {
        // Another directory's record (MoltBrowser's data folder moved):
        // nothing here was recorded.
        origin = Origin::kOtherDirectory;
        audit("record_reset", "another directory");
        LOG(WARNING) << "[Automation] the workflow record is of another "
                        "directory: every workflow needs a review";
      } else {
        // Kept aside (one copy), never seeded from.
        base::ReplaceFile(config.record_file,
                          base::FilePath(config.record_file.value() +
                                         FILE_PATH_LITERAL(".bad")),
                          nullptr);
        origin = Origin::kAfterCorrupt;
        audit("record_reset", "corrupt");
        LOG(ERROR) << "[Automation] the workflow record is corrupt: every "
                      "workflow needs a review";
      }
      // The marker repaired.
      marker_after = marker_missing;
      break;
    }
    case MoltFileKind::kMissing:
      if (!marker_missing) {
        // Lost: never seeded again.
        origin = Origin::kAfterLoss;
        audit("record_reset", "lost");
        LOG(ERROR) << "[Automation] the workflow record is gone: every "
                      "workflow needs a review";
      } else if (!config.may_seed) {
        // A new user data directory: the marker first, so that it never
        // seeds, even if the record isn't written.
        WriteRegularFile(marker, kMarkerContents);
        origin = Origin::kFresh;
      } else {
        // The first start of this version: the workflows there now, as
        // the previous build ran them (their own last fire times too).
        // Once (the marker follows the record). A fire the file claims
        // later is believed only if it is since now (LastFired()).
        const int64_t now = NowUnix();
        for (const std::string& name : names) {
          if (std::optional<Entry> entry = entry_of(name)) {
            entry->written = now;
            files[name] = std::move(*entry);
          }
        }
        origin = Origin::kSeeded;
        audit("record_seeded",
              base::NumberToString(files.size()) + " workflows");
        std::string seeded;
        for (const auto& [name, entry] : files) {
          seeded += (seeded.empty() ? "" : ", ") + name;
        }
        LOG(WARNING) << "[Automation] workflows trusted as they were: "
                     << (seeded.empty() ? std::string("none") : seeded);
        marker_after = true;
      }
      break;
    case MoltFileKind::kNotRegular:
    case MoltFileKind::kUnreadable:
      // Never read through, never written over.
      LOG(ERROR) << "[Automation] the workflow record isn't a readable "
                    "regular file; only this session's writes are trusted";
      FinishSessionOnly(generation);
      return;
  }

  // Files no longer there (deleted by another browser, by hand); but a
  // script set aside now (another browser's move in progress, or one cut
  // short) keeps its entry, to be vouched for again once it is put back.
  for (auto it = files.begin(); it != files.end();) {
    if (names.count(it->first) || IsSetAsideIn(it->first, names)) {
      ++it;
    } else {
      it = files.erase(it);
    }
  }
  {
    base::AutoLock hold(lock_);
    if (generation != generation_) {
      return;
    }
    // What this process wrote before the record was configured: later
    // than anything recorded.
    for (const auto& [name, entry] : entries_) {
      files[name] = entry;
    }
    entries_ = std::move(files);
    origin_ = origin;
    created_ = created.empty() ? NowIso8601() : std::move(created);
    session_only_ = false;
    needs_load_ = false;
  }
  // Always, empty included: a later start never takes this one for an
  // upgrade.
  Persist();
  if (marker_after) {
    WriteRegularFile(marker, kMarkerContents);
  }
}

bool WorkflowRecord::Vouches(const std::string& name,
                             std::string_view digest) const {
  if (digest.empty()) {
    return false;
  }
  base::AutoLock hold(lock_);
  auto it = entries_.find(name);
  return it != entries_.end() && it->second.digest == digest;
}

int64_t WorkflowRecord::FiredFloor(const std::string& name) const {
  base::AutoLock hold(lock_);
  auto it = entries_.find(name);
  return it == entries_.end() ? 0 : it->second.fired;
}

std::optional<WorkflowRecord::Entry> WorkflowRecord::EntryOf(
    const std::string& name) const {
  base::AutoLock hold(lock_);
  auto it = entries_.find(name);
  if (it == entries_.end()) {
    return std::nullopt;
  }
  return it->second;
}

int64_t WorkflowRecord::LastFired(const std::string& name, int64_t file_fired) {
  const int64_t now = NowUnix();
  int64_t last = 0;
  bool changed = false;
  {
    base::AutoLock hold(lock_);
    auto it = entries_.find(name);
    if (it == entries_.end()) {
      return std::max<int64_t>(0, file_fired);
    }
    Entry& entry = it->second;
    if (file_fired >= entry.written &&
        file_fired > now + kLastFiredSlackSeconds) {
      // Written since, and still to come: another browser's Snooze or
      // Skip, or forged. Until it comes, a fire when this browser first
      // read it: no catch-up run of what was due before (this browser's
      // own last fire may be long past, the other browser's run since
      // unseen), and no schedule held back more than an interval (believed,
      // a forged one would stall it). Kept in the record: read afresh at
      // each start, a value written once would hold the schedule back
      // again at each one.
      if (entry.pending != file_fired) {
        entry.pending = file_fired;
        entry.pending_seen = now;
        changed = true;
      }
      last = std::max(entry.fired, entry.pending_seen);
    } else {
      if (entry.pending != 0) {
        entry.pending = 0;
        entry.pending_seen = 0;
        changed = true;
      }
      // Earlier: in what this browser wrote there, or forged (it would
      // force a catch-up run). Otherwise another browser's run since this
      // one last wrote the file.
      last = file_fired < entry.written ? entry.fired
                                        : std::max(entry.fired, file_fired);
    }
  }
  if (changed) {
    Persist();
  }
  return last;
}

void WorkflowRecord::Set(const std::string& name,
                         std::string digest,
                         int64_t fired) {
  const int64_t now = NowUnix();
  {
    base::AutoLock hold(lock_);
    if (digest.empty()) {
      entries_.erase(name);
    } else {
      // What it writes: 0 (a schedule saved afresh starts over) and a later
      // fire time (a snooze) as they are. An earlier one comes from a page
      // opened before the last fire, which would run it again: the fire
      // recorded is kept, unless it is still in the future (a snooze, or a
      // clock that was ahead), which any write replaces.
      int64_t written_fired = std::max<int64_t>(0, fired);
      auto it = entries_.find(name);
      if (written_fired > 0 && it != entries_.end() &&
          written_fired < it->second.fired &&
          it->second.fired <= now + kLastFiredSlackSeconds) {
        written_fired = it->second.fired;
      }
      entries_[name] = {std::move(digest), written_fired,
                        std::max<int64_t>(0, now)};
    }
  }
  Persist();
}

void WorkflowRecord::Restore(const std::string& name,
                             const std::optional<Entry>& entry) {
  {
    base::AutoLock hold(lock_);
    if (entry) {
      entries_[name] = *entry;
    } else {
      entries_.erase(name);
    }
  }
  Persist();
}

void WorkflowRecord::NoteFired(const std::string& name, int64_t fired) {
  bool changed = false;
  {
    base::AutoLock hold(lock_);
    auto it = entries_.find(name);
    // A later fire, or this one after a fire recorded while the clock was
    // ahead (no fire happens later than one happening now): it would
    // otherwise hold back every catch-up run until the clock reached it.
    if (it != entries_.end() &&
        (fired > it->second.fired ||
         (fired > 0 && it->second.fired > fired + kLastFiredSlackSeconds))) {
      it->second.fired = fired;
      // Its fire is written to the file next.
      it->second.pending = 0;
      it->second.pending_seen = 0;
      changed = true;
    }
  }
  if (changed) {
    Persist();
  }
}

bool WorkflowRecord::Copy(const std::string& from, const std::string& to) {
  {
    base::AutoLock hold(lock_);
    auto it = entries_.find(from);
    if (it == entries_.end()) {
      return false;
    }
    Entry entry = it->second;
    entries_[to] = std::move(entry);
  }
  Persist();
  return true;
}

void WorkflowRecord::Forget(const std::string& name) {
  bool changed = false;
  {
    base::AutoLock hold(lock_);
    changed = entries_.erase(name) > 0;
  }
  if (changed) {
    Persist();
  }
}

void WorkflowRecord::Persist() {
  // One write at a time, each of the record as it is when it starts: a
  // reader's (LastFired()) never lands after a later change's.
  base::AutoLock write(write_lock_);
  base::FilePath record_file;
  base::DictValue root;
  {
    base::AutoLock hold(lock_);
    if (needs_load_ || session_only_ || config_.record_file.empty()) {
      return;
    }
    record_file = config_.record_file;
    base::DictValue files;
    for (const auto& [name, entry] : entries_) {
      if (!IsValidName(name) || !IsValidDigest(entry.digest)) {
        continue;
      }
      base::DictValue value;
      value.Set("digest", entry.digest);
      if (entry.fired > 0) {
        value.Set("fired", static_cast<double>(entry.fired));
      }
      value.Set("written", static_cast<double>(entry.written));
      if (entry.pending > 0) {
        value.Set("pending", static_cast<double>(entry.pending));
        value.Set("pending_seen", static_cast<double>(entry.pending_seen));
      }
      files.Set(name, std::move(value));
    }
    root.Set("format", kFormat);
    root.Set("directory", config_.directory.AsUTF8Unsafe());
    root.Set("origin", OriginName(origin_));
    root.Set("created", created_);
    root.Set("files", std::move(files));
  }
  std::string bytes;
  if (!base::JSONWriter::WriteWithOptions(
          root, base::JSONWriter::OPTIONS_PRETTY_PRINT, &bytes) ||
      bytes.size() > kMaxWorkflowRecordBytes) {
    LOG(ERROR) << "[Automation] the workflow record is too big to write";
    return;
  }
  // A failed write keeps the change in memory: after a restart that file
  // needs a review (fails closed).
  WriteRegularFile(record_file, bytes);
}

}  // namespace automation
}  // namespace molt_ai
