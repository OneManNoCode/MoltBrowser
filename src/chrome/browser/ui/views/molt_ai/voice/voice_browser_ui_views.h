// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The views side of voice mode's view of the browser's own buttons and
// menus (chrome/browser/molt_ai/voice/voice_browser_ui.h): the browser
// window's toolbar, tab strip, bookmarks bar and side panel, and the menu
// that shows, read as their accessibility data gives them, and pressed by
// their accessibility id with the default action assistive technology
// uses (View::HandleAccessibleAction(), which a menu item takes as its
// own: accepted, or its submenu opened).
//
// Installed once, when a window gets its Voice button (voice_toolbar_button.h).

#ifndef CHROME_BROWSER_UI_VIEWS_MOLT_AI_VOICE_VOICE_BROWSER_UI_VIEWS_H_
#define CHROME_BROWSER_UI_VIEWS_MOLT_AI_VOICE_VOICE_BROWSER_UI_VIEWS_H_

#include <cstdint>
#include <vector>

#include "chrome/browser/molt_ai/voice/voice_browser_ui.h"

namespace molt_ai {
namespace voice {

class VoiceBrowserUiViews : public VoiceBrowserUi {
 public:
  // Installs the process's one instance (VoiceBrowserUi::Install()), once.
  static void EnsureInstalled();

  VoiceBrowserUiViews();
  VoiceBrowserUiViews(const VoiceBrowserUiViews&) = delete;
  VoiceBrowserUiViews& operator=(const VoiceBrowserUiViews&) = delete;
  ~VoiceBrowserUiViews() override;

  // VoiceBrowserUi:
  std::vector<BrowserUiNode> Collect(BrowserWindowInterface* browser) override;
  bool Press(BrowserWindowInterface* browser, int32_t id) override;
  bool MenuOpen(BrowserWindowInterface* browser) override;
  void CloseMenu(BrowserWindowInterface* browser) override;
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_UI_VIEWS_MOLT_AI_VOICE_VOICE_BROWSER_UI_VIEWS_H_
