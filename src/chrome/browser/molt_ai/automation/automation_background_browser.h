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

#include <map>
#include <string>

#include "chrome/browser/molt_ai/automation/automation_script.h"

class Profile;

namespace molt_ai {
namespace automation {

// Launches |script| in a background browser window. Returns immediately;
// the run continues asynchronously. When |minimize| is true the window
// is minimized; otherwise it is shown so the user can watch.
// |start_index| (default 0) lets callers pick up at a specific step —
// used by the manager UI's "Retry from failed step" button.
// |variable_overrides| (default empty) are applied on top of the script's
// saved default_variables AFTER it is reloaded from disk — this is how the
// studio's "Run with these values" passes one-off {{variable}} values without
// mutating the saved workflow. The scheduler passes none (uses saved defaults).
void RunScriptInBackgroundBrowser(
    Profile* profile,
    const Script& script,
    bool minimize,
    size_t start_index = 0,
    const std::map<std::string, std::string>& variable_overrides = {});

}  // namespace automation
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_BACKGROUND_BROWSER_H_
