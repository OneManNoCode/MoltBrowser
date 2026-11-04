// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// molt://ai-automation's reads and changes of the shared storage
// (molt_ai_automation_ui.cc), each run on the storage's sequences
// (automation/automation_shared_tasks.h), never on the UI thread. A
// read-modify-write (a save that keeps the stored trust, a trust change)
// runs under the directory's lock (AutomationStorage::SaveChecked(),
// Update()), and a new workflow takes a free id under it (SaveNew()), so
// none interleaves with the Agent studio's, the runner's or another
// browser's. Routines taught by voice belong to a profile
// (molt_ai/voice/voice_routine_store.h): this page lists and changes none.
// The page's messages and answers are the same as when this ran on the UI
// thread.
//
// Blocking (file I/O); base::Value and the storage only, so
// molt_ai_unittests tests it on temporary directories.

#ifndef CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_AUTOMATION_STORAGE_H_
#define CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_AUTOMATION_STORAGE_H_

#include <stddef.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/values.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/automation/automation_storage.h"

namespace molt_ai {
namespace automation_page {

// What the page is told for a routine taught by voice.
inline constexpr char kVoiceRoutinesElsewhere[] =
    "Routines taught by voice belong to a profile: they are in Agent mode.";

// listScripts: the workflows but routines taught by voice.
std::vector<automation::Script> ListWorkflows(
    automation::AutomationStorage& storage);

// getScript: the workflow |id|; nullopt for a routine taught by voice.
std::optional<automation::Script> LoadWorkflow(
    const std::string& id,
    automation::AutomationStorage& storage);

// runScript: where a run asked to start from step |start_from| (0-based)
// starts: it, or the end of the steps.
size_t StartIndex(const automation::Script& script, int start_from);
// runScript: LoadWorkflow(), and the run asked for in the audit log
// ("manual_run_requested": "from_step=<StartIndex()>", or "from_ui" from
// the start) when there is one.
std::optional<automation::Script> LoadToRun(
    const std::string& id,
    int start_from,
    automation::AutomationStorage& storage);

// saveScript: {success, error?}. The page's trust is ignored: the stored
// trust is kept, or a new workflow starts CASUAL
// (AutomationSecurity::ClampTrustForSave()); a routine taught by voice is
// neither saved nor saved over.
base::DictValue SaveWorkflow(automation::Script script,
                             automation::AutomationStorage& storage);

// deleteScript: a workflow (never a routine taught by voice) and its
// artifacts.
bool DeleteWorkflow(const std::string& id,
                    automation::AutomationStorage& storage);

// createSampleScript: saved under its fixed id (over an earlier sample).
bool SaveSample(automation::Script script,
                automation::AutomationStorage& storage);

// createBlankScript: saved under its id or the first free one after it;
// the id ("" when not saved).
std::string SaveBlank(automation::Script script,
                      automation::AutomationStorage& storage);

// importScript: {success, script_id}. Never over a stored workflow: one of
// its id is imported as "<id>-imported-<now_unix>" (or the first free id
// after that); its stats start afresh, its trust CASUAL whatever it says,
// and the audit log says "imported".
base::DictValue ImportWorkflow(automation::Script script,
                               int64_t now_unix,
                               automation::AutomationStorage& storage);

// auditTail and getAuditForScript: the log's last |max_lines| lines.
std::vector<std::string> AuditTail(int max_lines,
                                   automation::AutomationStorage& storage);

// promoteTrust (after the user's click): {success, trust} or {success:
// false, error}. Not for a routine taught by voice ("script not found") nor
// an unconfirmed draft; the audit log says "trust_changed".
base::DictValue PromoteTrust(const std::string& id,
                             automation::TrustLevel level,
                             automation::AutomationStorage& storage);

}  // namespace automation_page
}  // namespace molt_ai

#endif  // CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_AUTOMATION_STORAGE_H_
