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

#include "base/files/file_path.h"
#include "base/files/scoped_temp_dir.h"
#include "base/path_service.h"
#include "base/test/scoped_path_override.h"
#include "base/test/test_future.h"
#include "base/threading/thread_restrictions.h"
#include "chrome/browser/file_system_access/chrome_file_system_access_permission_context.h"
#include "chrome/browser/file_system_access/file_system_access_permission_context_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
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

}  // namespace
