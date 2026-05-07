// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Helper that launches a Script in a background Browser window:
//   - Creates a fresh Browser of type TYPE_POPUP attached to the
//     user's main profile (so cookies/login state persist).
//   - Adds a single tab and points it at the script's first NAVIGATE
//     step's URL, then attaches an AutomationRunner.
//   - When |minimize| is true, the window is minimized to the dock so
//     the user can verify activity but it stays out of the way.
//   - When the script finishes (success or failure), the window
//     auto-closes.

#ifndef CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_BACKGROUND_BROWSER_H_
#define CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_BACKGROUND_BROWSER_H_

#include "chrome/browser/molt_ai/automation/automation_script.h"

class Profile;

namespace molt_ai {
namespace automation {

// Launches |script| in a background browser window. Returns immediately;
// the run continues asynchronously. When |minimize| is true the window
// is minimized; otherwise it is shown so the user can watch.
void RunScriptInBackgroundBrowser(Profile* profile,
                                  const Script& script,
                                  bool minimize);

}  // namespace automation
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_BACKGROUND_BROWSER_H_
