// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/side_panel/ai_chat_side_panel_coordinator.h"

#include <memory>

#include "chrome/browser/molt_ai/side_panel/ai_chat_side_panel_web_view.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window/public/browser_window_features.h"
#include "chrome/browser/ui/side_panel/side_panel_entry.h"
#include "chrome/browser/ui/side_panel/side_panel_registry.h"
#include "chrome/browser/ui/side_panel/side_panel_ui.h"
#include "ui/views/view.h"

AiChatSidePanelCoordinator::AiChatSidePanelCoordinator(Browser* browser)
    : browser_(browser), profile_(browser->profile()) {}

AiChatSidePanelCoordinator::~AiChatSidePanelCoordinator() = default;

void AiChatSidePanelCoordinator::CreateAndRegisterEntry(
    SidePanelRegistry* global_registry) {
  global_registry->Register(std::make_unique<SidePanelEntry>(
      SidePanelEntry::Key(SidePanelEntry::Id::kMoltAiChat),
      base::BindRepeating(&AiChatSidePanelCoordinator::CreateAiChatWebView,
                          base::Unretained(this)),
      /*default_content_width_callback=*/base::NullCallback()));
}

bool AiChatSidePanelCoordinator::Show() {
  SidePanelUI* side_panel_ui =
      browser_->GetFeatures().side_panel_ui();
  if (!side_panel_ui) {
    return false;
  }
  side_panel_ui->Show(SidePanelEntry::Id::kMoltAiChat);
  return true;
}

std::unique_ptr<views::View> AiChatSidePanelCoordinator::CreateAiChatWebView(
    SidePanelEntryScope& scope) {
  return std::make_unique<AiChatSidePanelWebView>(profile_);
}
