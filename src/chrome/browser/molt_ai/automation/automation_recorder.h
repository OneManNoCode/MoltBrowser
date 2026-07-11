// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// AutomationRecorder — captures user actions in a tab and emits Step
// objects suitable for the AutomationRunner to replay.
//
// Usage:
//   1. AutomationRecorder rec(web_contents);
//   2. rec.Start() — injects automation_recorder.js, listens for events.
//   3. User performs the workflow (clicks, typing, scrolling, navigation).
//   4. rec.Stop() — returns a Script with collected steps + auto-derived
//      domain whitelist. Caller then prompts the user for a name and
//      saves via AutomationStorage::Save.

#ifndef CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_RECORDER_H_
#define CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_RECORDER_H_

#include <set>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "content/public/browser/web_contents_observer.h"

namespace content {
class NavigationHandle;
class WebContents;
}  // namespace content

namespace molt_ai {
namespace automation {

// Fired every time a step is captured so the recording overlay can show
// the running count.
using StepCapturedCallback =
    base::RepeatingCallback<void(const Step& s, int total)>;

class AutomationRecorder : public content::WebContentsObserver {
 public:
  explicit AutomationRecorder(content::WebContents* contents);
  ~AutomationRecorder() override;

  AutomationRecorder(const AutomationRecorder&) = delete;
  AutomationRecorder& operator=(const AutomationRecorder&) = delete;

  // Begin capturing. Re-injects the recorder JS into the active frame so
  // the overlay survives navigations within the recording session.
  void Start(StepCapturedCallback on_step_captured);

  // Stop capturing and return the assembled Script (id is empty — caller
  // must assign one).
  Script Stop();

  bool is_recording() const { return is_recording_; }
  int captured_count() const { return static_cast<int>(steps_.size()); }

  // content::WebContentsObserver — re-inject after navigation.
  void DocumentOnLoadCompletedInPrimaryMainFrame() override;
  // Record browser-initiated navigations (typed URL / bookmark / history) as
  // NAVIGATE steps, so the omnibox URL the user types becomes a recorded step.
  void DidFinishNavigation(
      content::NavigationHandle* navigation_handle) override;

  // Hook for the WebUI / IPC layer: receive a step JSON dict that the
  // injected recorder JS posted via chrome.send.
  void OnStepFromInjectedJS(const base::DictValue& step_json);

 private:
  void InjectRecorderJS();
  void DeduplicateAndAppend(Step step);

  // Capture a record-time thumbnail of the just-clicked element (the tab is
  // visible while recording) and stash it as a data: URI in steps_[index].extra
  // ("ref_shot"), so the editor can show what each step was recorded against.
  // Best-effort + async; a missed capture just leaves the step without a thumb.
  void CaptureElementThumb(int index, int x, int y, int w, int h);
  void OnThumbReady(int index, std::string data_uri);
  // Polls the injected JS queue and routes each entry to
  // OnStepFromInjectedJS. Runs every 400ms while recording.
  void PollPageQueue();

  bool is_recording_ = false;
  std::vector<Step> steps_;
  std::set<std::string> seen_hosts_;
  StepCapturedCallback on_step_captured_;
  base::RepeatingTimer poll_timer_;

  base::WeakPtrFactory<AutomationRecorder> weak_factory_{this};
};

}  // namespace automation
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_RECORDER_H_
