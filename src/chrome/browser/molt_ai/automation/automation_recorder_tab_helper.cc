// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/automation_recorder_tab_helper.h"

#include <ctime>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "chrome/browser/molt_ai/automation/automation_scheduler_factory.h"
#include "chrome/browser/molt_ai/automation/automation_scheduler_service.h"
#include "chrome/browser/molt_ai/automation/automation_storage.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/scoped_tabbed_browser_displayer.h"
#include "chrome/browser/ui/singleton_tabs.h"
#include "content/public/browser/web_contents.h"
#include "url/gurl.h"

namespace molt_ai {
namespace automation {

AutomationRecorderTabHelper::AutomationRecorderTabHelper(
    content::WebContents* contents)
    : content::WebContentsUserData<AutomationRecorderTabHelper>(*contents) {}

AutomationRecorderTabHelper::~AutomationRecorderTabHelper() = default;

void AutomationRecorderTabHelper::Toggle(Profile* profile) {
  content::WebContents* wc = &GetWebContents();
  if (!rec_) {
    rec_ = std::make_unique<AutomationRecorder>(wc);
  }
  if (!rec_->is_recording()) {
    LOG(INFO) << "[MoltAutomation] start recording on tab";
    rec_->Start(base::DoNothing());
    return;
  }

  // Stop + save.
  Script s = rec_->Stop();
  // Auto-assign id if empty.
  if (s.id.empty()) {
    s.id = "rec_" + base::NumberToString(static_cast<int64_t>(
                          std::time(nullptr)));
  }
  s.name = "Recorded " + s.id;
  LOG(INFO) << "[MoltAutomation] stop recording, captured "
            << static_cast<int>(s.steps.size()) << " steps; saving id=" << s.id;

  // Save through the scheduler service's storage when possible so we
  // share an instance with the scheduler. Falls back to a new
  // AutomationStorage if not available.
  AutomationStorage* storage = nullptr;
  std::unique_ptr<AutomationStorage> owned;
  if (profile) {
    if (auto* svc =
            AutomationSchedulerServiceFactory::GetForProfile(profile)) {
      storage = svc->storage();
    }
  }
  if (!storage) {
    owned = std::make_unique<AutomationStorage>();
    owned->EnsureDirectory();
    storage = owned.get();
  }
  storage->Save(s);

  // Open the manager UI so the user can rename/edit/run.
  if (profile) {
    Browser* b = chrome::FindLastActiveWithProfile(profile);
    if (b) {
      ShowSingletonTab(b, GURL("molt://ai-automation/"));
    }
  }
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(AutomationRecorderTabHelper);

}  // namespace automation
}  // namespace molt_ai
