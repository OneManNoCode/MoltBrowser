// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Per-tab attachment that owns an AutomationRecorder and binds its
// lifecycle to a content::WebContents. Used by the toolbar Record
// button to start/stop recording on the currently active tab.

#ifndef CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_RECORDER_TAB_HELPER_H_
#define CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_RECORDER_TAB_HELPER_H_

#include <memory>

#include "chrome/browser/molt_ai/automation/automation_recorder.h"
#include "content/public/browser/web_contents_user_data.h"

namespace content {
class WebContents;
}  // namespace content

class Profile;

namespace molt_ai {
namespace automation {

class AutomationRecorderTabHelper
    : public content::WebContentsUserData<AutomationRecorderTabHelper> {
 public:
  ~AutomationRecorderTabHelper() override;

  // Toggle: if not recording, start. If recording, stop, save the script
  // to disk via AutomationStorage, and open molt://ai-automation/ so the
  // user can name + edit it.
  void Toggle(Profile* profile);

  bool is_recording() const { return rec_ && rec_->is_recording(); }

  // ---- Global recording state (used by the toolbar Record button to
  // render Start vs. Stop label). Only one tab can be recording at a
  // time so the toolbar can show an unambiguous indicator regardless of
  // which tab is active. ----
  static bool IsAnyRecording();
  static content::WebContents* GetActivelyRecordingContents();

 private:
  friend class content::WebContentsUserData<AutomationRecorderTabHelper>;
  explicit AutomationRecorderTabHelper(content::WebContents* contents);

  std::unique_ptr<AutomationRecorder> rec_;

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

}  // namespace automation
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_RECORDER_TAB_HELPER_H_
