// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// AiChatSidePanelCoordinator — manages the AI Chat side panel entry
// Registered as kMoltAiChat in the SidePanelRegistry.

#ifndef CHROME_BROWSER_MOLT_AI_SIDE_PANEL_AI_CHAT_SIDE_PANEL_COORDINATOR_H_
#define CHROME_BROWSER_MOLT_AI_SIDE_PANEL_AI_CHAT_SIDE_PANEL_COORDINATOR_H_

#include <memory>

#include "base/memory/raw_ptr.h"

class Browser;
class Profile;
class SidePanelRegistry;
class SidePanelEntryScope;

namespace views {
class View;
}  // namespace views

// AiChatSidePanelCoordinator handles the creation and lifecycle of the
// AI chat side panel view. It registers a SidePanelEntry with the
// global SidePanelRegistry so the AI chat appears in the side panel
// dropdown alongside Bookmarks, History, etc.
class AiChatSidePanelCoordinator {
 public:
  explicit AiChatSidePanelCoordinator(Browser* browser);
  AiChatSidePanelCoordinator(const AiChatSidePanelCoordinator&) = delete;
  AiChatSidePanelCoordinator& operator=(const AiChatSidePanelCoordinator&) =
      delete;
  ~AiChatSidePanelCoordinator();

  // Register the AI Chat entry in the global side panel registry.
  void CreateAndRegisterEntry(SidePanelRegistry* global_registry);

  // Show the AI chat side panel.
  bool Show();

 private:
  // Factory method called by SidePanelEntry to create the view on demand.
  std::unique_ptr<views::View> CreateAiChatWebView(
      SidePanelEntryScope& scope);

  const raw_ptr<Browser> browser_;
  const raw_ptr<Profile> profile_;
};

#endif  // CHROME_BROWSER_MOLT_AI_SIDE_PANEL_AI_CHAT_SIDE_PANEL_COORDINATOR_H_
