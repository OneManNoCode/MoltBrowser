// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/molt_ai/side_panel/ai_chat_side_panel_coordinator.h"

#include <memory>

#include "chrome/browser/molt_ai/side_panel/ai_chat_side_panel_web_view.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "ui/views/view.h"

// TODO(AiChat): Add these includes once SidePanelEntry registration is
// implemented:
//   #include "chrome/browser/ui/side_panel/side_panel_entry.h"
//   #include "chrome/browser/ui/side_panel/side_panel_registry.h"

AiChatSidePanelCoordinator::AiChatSidePanelCoordinator(Browser* browser)
    : browser_(browser), profile_(browser->profile()) {}

AiChatSidePanelCoordinator::~AiChatSidePanelCoordinator() = default;

std::unique_ptr<views::View> AiChatSidePanelCoordinator::CreateAiChatView() {
  return std::make_unique<AiChatSidePanelWebView>(profile_);
}

// TODO(AiChat): Implement SidePanelEntry registration once a dedicated
// SidePanelEntryId is available. The implementation should follow this pattern:
//
// void AiChatSidePanelCoordinator::CreateAndRegisterEntry(
//     SidePanelRegistry* global_registry) {
//   global_registry->Register(std::make_unique<SidePanelEntry>(
//       SidePanelEntry::Key(SidePanelEntry::Id::kMoltAiChat),
//       base::BindRepeating(&CreateAiChatWebView, browser_),
//       /*default_content_width_callback=*/base::NullCallback()));
// }
