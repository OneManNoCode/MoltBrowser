// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The browser window's own buttons and menus, as voice mode reads and
// presses them (browser_ui_map.h; docs/design/TOUCHLESS_BROWSING.md §6.3,
// "The browser UI too"). This is the API voice mode calls; no views code.
// The views side (chrome/browser/ui/views/molt_ai/voice/
// voice_browser_ui_views.h) implements it, and installs itself when a
// window gets its Voice button. Without it, voice mode says it can't find
// the browser's buttons.
//
//   - Collect(): the toolbar, the tab strip, the bookmarks bar and the side
//     panel of |browser|'s window, and the menu that shows, as their
//     accessibility data gives them (role, name, states, the view's class).
//     Bubbles (a permission prompt, the download bubble, the password
//     bubble) are windows of their own, never walked.
//   - Press(): the view with that accessibility id, found again under the
//     same roots, as assistive technology's default action does (a menu
//     item accepted, a submenu opened); never a protected one
//     (IsProtectedBrowserUiClass(), checked again here, ancestors too).
//
// UI thread.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_BROWSER_UI_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_BROWSER_UI_H_

#include <cstdint>
#include <vector>

#include "chrome/browser/molt_ai/voice/browser_ui_map.h"

class BrowserWindowInterface;

namespace molt_ai {
namespace voice {

class VoiceBrowserUi {
 public:
  // The installed implementation, or null.
  static VoiceBrowserUi* Get();
  // |ui| lives for the rest of the process (not owned).
  static void Install(VoiceBrowserUi* ui);

  virtual std::vector<BrowserUiNode> Collect(
      BrowserWindowInterface* browser) = 0;
  // False when no such view is there now, or it is protected.
  virtual bool Press(BrowserWindowInterface* browser, int32_t id) = 0;
  // A menu shows over |browser|'s window.
  virtual bool MenuOpen(BrowserWindowInterface* browser) = 0;
  virtual void CloseMenu(BrowserWindowInterface* browser) = 0;

 protected:
  virtual ~VoiceBrowserUi() = default;
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_BROWSER_UI_H_
