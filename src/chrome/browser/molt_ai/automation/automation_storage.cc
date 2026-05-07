// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/automation_storage.h"

#include <ctime>
#include <fstream>
#include <sstream>
#include <utility>

#include "base/base_paths.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/strings/string_util.h"

namespace molt_ai {
namespace automation {

namespace {

constexpr char kAutomationsDirName[] = ".moltbrowser";
constexpr char kAutomationsSubdir[] = "automations";
constexpr char kArtifactsSubdir[] = "artifacts";
constexpr char kAuditLogName[] = "audit.log";
constexpr char kScriptExt[] = ".molt";

// Reasonably safe filename: only id chars (lowercased ascii, digits, _-)
// already constrain things, but defensively reject path separators.
bool IsValidScriptId(const std::string& id) {
  if (id.empty() || id.size() > 128)
    return false;
  for (char c : id) {
    if (c == '/' || c == '\\' || c == ':' || c == '.' || c == '\0')
      return false;
  }
  return true;
}

// ISO-8601 in UTC for audit timestamps.
std::string NowISO8601() {
  std::time_t t = std::time(nullptr);
  std::tm gmt;
  gmtime_r(&t, &gmt);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &gmt);
  return std::string(buf);
}

}  // namespace

AutomationStorage::AutomationStorage() = default;
AutomationStorage::~AutomationStorage() = default;

base::FilePath AutomationStorage::GetRoot() const {
  if (!root_override_.empty())
    return root_override_;
  base::FilePath home;
  base::PathService::Get(base::DIR_HOME, &home);
  return home.Append(kAutomationsDirName);
}

base::FilePath AutomationStorage::GetAutomationsDir() const {
  return GetRoot().Append(kAutomationsSubdir);
}

base::FilePath AutomationStorage::GetScriptPath(const std::string& id) const {
  return GetAutomationsDir().Append(id + kScriptExt);
}

base::FilePath AutomationStorage::GetAuditLogPath() const {
  return GetAutomationsDir().Append(kAuditLogName);
}

void AutomationStorage::SetRootForTesting(const base::FilePath& root) {
  root_override_ = root;
}

bool AutomationStorage::EnsureDirectory() {
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
  std::vector<std::string> out;
  base::FilePath dir = GetAutomationsDir();
  if (!base::DirectoryExists(dir))
    return out;

  base::FileEnumerator e(dir, /*recursive=*/false,
                         base::FileEnumerator::FILES,
                         FILE_PATH_LITERAL("*.molt"));
  for (auto path = e.Next(); !path.empty(); path = e.Next()) {
    std::string filename = path.BaseName().RemoveExtension().value();
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

std::optional<Script> AutomationStorage::Load(const std::string& id) {
  if (!IsValidScriptId(id))
    return std::nullopt;
  std::string contents;
  if (!base::ReadFileToString(GetScriptPath(id), &contents))
    return std::nullopt;
  return Script::FromJSONString(contents);
}

bool AutomationStorage::Save(const Script& script) {
  if (!IsValidScriptId(script.id))
    return false;
  if (!EnsureDirectory())
    return false;

  // Atomic write: foo.molt.tmp -> foo.molt
  base::FilePath final_path = GetScriptPath(script.id);
  base::FilePath tmp_path = final_path.AddExtension(FILE_PATH_LITERAL(".tmp"));

  std::string json = script.ToJSONString();
  if (!base::WriteFile(tmp_path, json)) {
    LOG(ERROR) << "[Automation] Failed to write " << tmp_path.value();
    return false;
  }

  base::File::Error rename_err = base::File::FILE_OK;
  if (!base::ReplaceFile(tmp_path, final_path, &rename_err)) {
    base::DeleteFile(tmp_path);
    LOG(ERROR) << "[Automation] Rename failed err=" << rename_err;
    return false;
  }

  AppendAudit(script.id, "saved", "");
  return true;
}

bool AutomationStorage::Delete(const std::string& id) {
  if (!IsValidScriptId(id))
    return false;
  bool deleted_script = base::DeleteFile(GetScriptPath(id));
  base::FilePath artifacts =
      GetAutomationsDir().Append(kArtifactsSubdir).Append(id);
  base::DeletePathRecursively(artifacts);
  AppendAudit(id, "deleted", "");
  return deleted_script;
}

void AutomationStorage::AppendAudit(const std::string& script_id,
                                     const std::string& event,
                                     const std::string& extra) {
  if (!EnsureDirectory())
    return;
  std::string line =
      NowISO8601() + " | " + script_id + " | " + event;
  if (!extra.empty())
    line += " | " + extra;
  line += "\n";

  // Open in append mode (std::ofstream is fine for this small write).
  std::ofstream f(GetAuditLogPath().value(), std::ios::app);
  if (f.is_open()) {
    f << line;
  }
}

std::vector<std::string> AutomationStorage::ReadAuditTail(int max_lines) {
  std::vector<std::string> out;
  std::ifstream f(GetAuditLogPath().value());
  if (!f.is_open())
    return out;
  std::string line;
  while (std::getline(f, line)) {
    out.push_back(line);
    if (static_cast<int>(out.size()) > max_lines)
      out.erase(out.begin());
  }
  return out;
}

base::FilePath AutomationStorage::EnsureArtifactsDir(
    const std::string& script_id) {
  base::FilePath dir =
      GetAutomationsDir().Append(kArtifactsSubdir).Append(script_id);
  if (!base::DirectoryExists(dir))
    base::CreateDirectory(dir);
  return dir;
}

}  // namespace automation
}  // namespace molt_ai
