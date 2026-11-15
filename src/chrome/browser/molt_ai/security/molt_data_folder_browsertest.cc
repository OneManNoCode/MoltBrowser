// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser's data folder, ~/.moltbrowser (molt_ai/common/molt_home_dir.h),
// holds its workflows, with their trust and triggers, and its settings. No
// site may open it, save into it or write it through the File System Access
// API: it is on Chromium's sensitive-path list beside ~/.ssh and ~/.config
// (chrome_file_system_access_permission_context.cc). Chromium's own unit
// test of that list (unit_tests) runs on the build machine; this one runs in
// every test build, so a rebase that drops the entry fails here.
//
// What the browser trusts of a workflow file there is recorded in the user
// data directory, never in that folder (molt_ai/automation/workflow_record.h):
// the record is there once the shared storage was used, and a workflow
// file written there after startup loads as a draft to review.

#include <optional>
#include <utility>

#include "base/files/file_enumerator.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/path_service.h"
#include "base/test/scoped_path_override.h"
#include "base/test/test_future.h"
#include "base/threading/thread_restrictions.h"
#include "chrome/browser/file_system_access/chrome_file_system_access_permission_context.h"
#include "chrome/browser/file_system_access/file_system_access_permission_context_factory.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/automation/automation_shared_tasks.h"
#include "chrome/browser/molt_ai/automation/automation_storage.h"
#include "chrome/browser/molt_ai/automation/workflow_record.h"
#include "chrome/browser/molt_ai/common/molt_home_dir_test_util.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/common/chrome_paths.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "content/public/browser/file_system_access_permission_context.h"
#include "content/public/browser/global_routing_id.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace {

using AccessTrigger = ChromeFileSystemAccessPermissionContext::AccessTrigger;
using HandleType = ChromeFileSystemAccessPermissionContext::HandleType;
using SensitiveEntryResult =
    ChromeFileSystemAccessPermissionContext::SensitiveEntryResult;

class MoltDataFolderBrowserTest : public InProcessBrowserTest {
 protected:
  SensitiveEntryResult Confirm(const base::FilePath& path,
                               HandleType handle_type,
                               AccessTrigger access_trigger) {
    base::test::TestFuture<SensitiveEntryResult> result;
    context()->ConfirmSensitiveEntryAccess(
        url::Origin::Create(GURL("https://example.com")),
        content::PathInfo(path), handle_type, access_trigger,
        content::GlobalRenderFrameHostId(), result.GetCallback());
    return result.Get();
  }

  ChromeFileSystemAccessPermissionContext* context() {
    return FileSystemAccessPermissionContextFactory::GetForProfile(
        browser()->GetProfile());
  }
};

IN_PROC_BROWSER_TEST_F(MoltDataFolderBrowserTest, NoSiteMayOpenOrWriteIt) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  const base::FilePath home = temp.GetPath().AppendASCII("home");
  base::ScopedPathOverride home_override(base::DIR_HOME, home,
                                         /*is_absolute=*/true,
                                         /*create=*/true);
  // The list is built once, from the home folder of the time.
  context()->ResetBlockPathsForTesting();

  const base::FilePath molt = home.AppendASCII(".moltbrowser");
  const base::FilePath automations = molt.AppendASCII("automations");
  const base::FilePath workflow = automations.AppendASCII("a.molt");
  EXPECT_EQ(Confirm(molt, HandleType::kDirectory, AccessTrigger::kOpen),
            SensitiveEntryResult::kAbort);
  EXPECT_EQ(Confirm(automations, HandleType::kDirectory, AccessTrigger::kOpen),
            SensitiveEntryResult::kAbort);
  EXPECT_EQ(Confirm(workflow, HandleType::kFile, AccessTrigger::kSave),
            SensitiveEntryResult::kAbort);
  EXPECT_EQ(
      Confirm(workflow, HandleType::kFile, AccessTrigger::kProgrammaticWrite),
      SensitiveEntryResult::kAbort);
  EXPECT_EQ(Confirm(molt.AppendASCII("settings.json"), HandleType::kFile,
                    AccessTrigger::kOpen),
            SensitiveEntryResult::kAbort);
  // The home folder's other children are still the user's to pick.
  EXPECT_EQ(Confirm(home.AppendASCII("moltbrowser-notes"),
                    HandleType::kDirectory, AccessTrigger::kOpen),
            SensitiveEntryResult::kAllowed);
}

// The workflow record (workflow_record.h), as the browser configures it at
// startup, for a test's own MoltBrowser data folder.
class MoltWorkflowRecordBrowserTest : public InProcessBrowserTest {
 protected:
  // In place before the browser starts (the fixture is made first).
  molt_ai::ScopedMoltHomeDirForTesting molt_home_;
};

IN_PROC_BROWSER_TEST_F(MoltWorkflowRecordBrowserTest,
                       TheWorkflowRecordIsInTheUserDataDir) {
  using molt_ai::automation::AutomationStorage;
  // A storage call on the shared storage's sequence (the scheduler's
  // service made one at startup too): the record is loaded and written.
  base::test::TestFuture<bool> prepared;
  molt_ai::automation::PostSharedChange(
      AutomationStorage::SharedDirectory(),
      base::BindOnce([](AutomationStorage& storage) {
        storage.PrepareRecord();
        return true;
      }),
      prepared.GetCallback());
  ASSERT_TRUE(prepared.Get());

  base::ScopedAllowBlockingForTesting allow_blocking;
  base::FilePath user_data_dir;
  ASSERT_TRUE(base::PathService::Get(chrome::DIR_USER_DATA, &user_data_dir));
  const base::FilePath record =
      user_data_dir.Append(molt_ai::automation::kWorkflowRecordFileName);
  EXPECT_TRUE(base::PathExists(record));
  EXPECT_TRUE(base::PathExists(
      molt_ai::automation::WorkflowRecord::MarkerFileFor(record)));
  // Nothing of it in MoltBrowser's data folder.
  base::FileEnumerator in_home(molt_home_.path(), /*recursive=*/true,
                               base::FileEnumerator::FILES);
  for (base::FilePath path = in_home.Next(); !path.empty();
       path = in_home.Next()) {
    EXPECT_EQ(path.BaseName().value().find(
                  molt_ai::automation::kWorkflowRecordFileName),
              base::FilePath::StringType::npos)
        << path;
  }
}

IN_PROC_BROWSER_TEST_F(MoltWorkflowRecordBrowserTest,
                       AWorkflowPlantedAfterStartupNeedsReview) {
  using molt_ai::automation::AutomationStorage;
  using molt_ai::automation::Script;
  {
    base::ScopedAllowBlockingForTesting allow_blocking;
    const base::FilePath dir = AutomationStorage::SharedDirectory();
    ASSERT_TRUE(base::CreateDirectory(dir));
    Script planted;
    planted.id = "planted";
    planted.name = "Planted";
    planted.security.trust = molt_ai::automation::TrustLevel::TRUSTED;
    planted.trigger.type = molt_ai::automation::TriggerType::CRON;
    planted.trigger.expression = "* * * * *";
    molt_ai::automation::Step step;
    step.type = molt_ai::automation::StepType::NAVIGATE;
    step.target = "https://example.com/";
    planted.steps.push_back(std::move(step));
    ASSERT_TRUE(base::WriteFile(dir.AppendASCII("planted.molt"),
                                planted.ToJSONString()));
  }
  base::test::TestFuture<std::optional<Script>> loaded;
  molt_ai::automation::PostSharedRead(
      AutomationStorage::SharedDirectory(),
      base::BindOnce(
          [](AutomationStorage& storage) { return storage.Load("planted"); }),
      loaded.GetCallback());
  std::optional<Script> script = loaded.Take();
  ASSERT_TRUE(script);
  EXPECT_TRUE(script->security.changed_outside);
  EXPECT_TRUE(script->security.sites_unconfirmed);
  EXPECT_EQ(script->security.trust, molt_ai::automation::TrustLevel::CASUAL);
}

}  // namespace
