// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MOLT_AI_SIDE_PANEL_AI_CHAT_SIDE_PANEL_COORDINATOR_H_
#define CHROME_BROWSER_MOLT_AI_SIDE_PANEL_AI_CHAT_SIDE_PANEL_COORDINATOR_H_

#include <memory>

#include "base/memory/raw_ptr.h"

class Browser;
class Profile;

namespace views {
class View;
}  // namespace views

// AiChatSidePanelCoordinator handles the creation and lifecycle of the
// AI chat side panel view. It serves as the bridge between the browser
// and the AI chat interface displayed in the side panel.
//
// TODO(AiChat): Register a proper SidePanelEntry with the SidePanelRegistry
// once a dedicated SidePanelEntryId (e.g., kMoltAiChat) is added to the
// SIDE_PANEL_ENTRY_IDS macro in side_panel_entry_id.h. This requires
// corresponding entries in actions.xml and histograms.xml.
class AiChatSidePanelCoordinator {
 public:
  explicit AiChatSidePanelCoordinator(Browser* browser);
  AiChatSidePanelCoordinator(const AiChatSidePanelCoordinator&) = delete;
  AiChatSidePanelCoordinator& operator=(const AiChatSidePanelCoordinator&) =
      delete;
  ~AiChatSidePanelCoordinator();

  // Creates the AI chat side panel view. The caller takes ownership of the
  // returned view.
  //
  // TODO(AiChat): Once SidePanelEntry registration is implemented, this
  // will be called from the SidePanelEntry's CreateContentCallback instead
  // of being invoked directly.
  std::unique_ptr<views::View> CreateAiChatView();

  // TODO(AiChat): Add the following method once SidePanelEntry registration
  // is ready:
  //   void CreateAndRegisterEntry(SidePanelRegistry* global_registry);

 private:
  const raw_ptr<Browser> browser_;
  const raw_ptr<Profile> profile_;
};

#endif  // CHROME_BROWSER_MOLT_AI_SIDE_PANEL_AI_CHAT_SIDE_PANEL_COORDINATOR_H_
