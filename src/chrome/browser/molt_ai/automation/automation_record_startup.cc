// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/automation_record_startup.h"

#include "base/files/file_path.h"
#include "base/path_service.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/first_run/first_run.h"
#include "chrome/browser/molt_ai/automation/automation_storage.h"
#include "chrome/browser/molt_ai/automation/workflow_record.h"
#include "chrome/browser/profiles/profile_selections.h"
#include "chrome/common/chrome_paths.h"
#include "components/keyed_service/core/keyed_service.h"
#include "components/prefs/pref_service.h"

namespace molt_ai {
namespace automation {

void ConfigureWorkflowRecordOnce() {
  static bool configured = false;
  if (configured) {
    return;
  }
  configured = true;
  base::FilePath user_data_dir;
  const bool have_user_data_dir =
      base::PathService::Get(chrome::DIR_USER_DATA, &user_data_dir);
  // A new user data directory never seeds: Chrome's first run (cached by
  // ChromeBrowserMainParts before any profile), or Local State made by this
  // launch (a new --user-data-dir with --no-first-run). Without Local State
  // it can't be told: new.
  PrefService* local_state =
      g_browser_process ? g_browser_process->local_state() : nullptr;
  const bool fresh =
      first_run::IsChromeFirstRun() || !local_state ||
      local_state->GetInitializationStatus() ==
          PrefService::INITIALIZATION_STATUS_CREATED_NEW_PREF_STORE;
  WorkflowRecord::Configure({AutomationStorage::SharedDirectory(),
                             have_user_data_dir
                                 ? user_data_dir.Append(kWorkflowRecordFileName)
                                 : base::FilePath(),
                             /*may_seed=*/!fresh});
}

// static
AutomationRecordStartupFactory* AutomationRecordStartupFactory::GetInstance() {
  static base::NoDestructor<AutomationRecordStartupFactory> instance;
  return instance.get();
}

AutomationRecordStartupFactory::AutomationRecordStartupFactory()
    : ProfileKeyedServiceFactory(
          "MoltAutomationRecordStartup",
          // Whichever profile comes first: the record is process-wide, and
          // a session may open only the profile picker or a guest window.
          ProfileSelections::Builder()
              .WithRegular(ProfileSelection::kOriginalOnly)
              .WithGuest(ProfileSelection::kOriginalOnly)
              .WithSystem(ProfileSelection::kOriginalOnly)
              .Build()) {}

AutomationRecordStartupFactory::~AutomationRecordStartupFactory() = default;

std::unique_ptr<KeyedService>
AutomationRecordStartupFactory::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  // Only the first profile's call does anything.
  ConfigureWorkflowRecordOnce();
  return nullptr;
}

bool AutomationRecordStartupFactory::ServiceIsCreatedWithBrowserContext()
    const {
  return true;
}

bool AutomationRecordStartupFactory::ServiceIsNULLWhileTesting() const {
  return true;
}

}  // namespace automation
}  // namespace molt_ai
