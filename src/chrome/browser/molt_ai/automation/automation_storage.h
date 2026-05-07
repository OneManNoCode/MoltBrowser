// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Filesystem persistence for automation scripts.
// Layout:
//   ~/.moltbrowser/automations/
//     ├── <script-id>.molt          (one JSON file per script)
//     ├── audit.log                  (append-only log of every run)
//     └── artifacts/<script-id>/    (screenshots, extracted data)

#ifndef CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_STORAGE_H_
#define CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_STORAGE_H_

#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"

namespace molt_ai {
namespace automation {

class AutomationStorage {
 public:
  AutomationStorage();
  ~AutomationStorage();

  AutomationStorage(const AutomationStorage&) = delete;
  AutomationStorage& operator=(const AutomationStorage&) = delete;

  // Resolve and create the per-user automations directory the first time it's
  // needed. On macOS / Linux this is ~/.moltbrowser/automations/.
  bool EnsureDirectory();

  // List all script IDs currently saved on disk.
  std::vector<std::string> ListIds();

  // List all scripts (parsed). Skips files that fail to parse.
  std::vector<Script> ListAll();

  // Load one script by id. Returns nullopt if the file doesn't exist or is
  // malformed.
  std::optional<Script> Load(const std::string& id);

  // Save (create or overwrite). Returns true if the file was successfully
  // written. The file is written atomically (.tmp + rename).
  bool Save(const Script& script);

  // Delete a script + its artifacts directory.
  bool Delete(const std::string& id);

  // -------- Audit log --------
  // Append a single line to ~/.moltbrowser/automations/audit.log.
  // Format: ISO8601 | script_id | event | extra
  void AppendAudit(const std::string& script_id,
                   const std::string& event,
                   const std::string& extra);

  // Read the most recent N audit lines (returns oldest-first).
  std::vector<std::string> ReadAuditTail(int max_lines = 200);

  // -------- Artifacts --------
  // Returns ~/.moltbrowser/automations/artifacts/<script-id>/, creating it.
  base::FilePath EnsureArtifactsDir(const std::string& script_id);

  // Test seam: override the root directory (defaults to ~/.moltbrowser).
  void SetRootForTesting(const base::FilePath& root);

 private:
  base::FilePath GetRoot() const;
  base::FilePath GetAutomationsDir() const;
  base::FilePath GetScriptPath(const std::string& id) const;
  base::FilePath GetAuditLogPath() const;

  base::FilePath root_override_;
};

}  // namespace automation
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_STORAGE_H_
