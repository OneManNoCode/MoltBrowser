// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/side_panel/side_panel_helper.h"

#include "chrome/browser/history_clusters/history_clusters_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_actions.h"
#include "chrome/browser/ui/browser_window/public/browser_window_features.h"
#include "chrome/browser/ui/side_panel/side_panel_registry.h"
#include "chrome/browser/ui/side_panel/side_panel_ui.h"
#include "chrome/browser/ui/ui_features.h"
#include "chrome/browser/ui/views/side_panel/bookmarks/bookmarks_side_panel_coordinator.h"
#include "chrome/browser/ui/views/side_panel/comments/comments_side_panel_coordinator.h"
#include "chrome/browser/ui/views/side_panel/history/history_side_panel_coordinator.h"
#include "chrome/browser/ui/views/side_panel/history_clusters/history_clusters_side_panel_coordinator.h"
#include "chrome/browser/ui/views/side_panel/reading_list/reading_list_side_panel_coordinator.h"
#include "chrome/browser/molt_ai/side_panel/agent_side_panel_web_view.h"
#include "chrome/browser/molt_ai/side_panel/ai_chat_side_panel_web_view.h"
#include "chrome/browser/ui/side_panel/side_panel_entry.h"
#include "chrome/browser/ui/webui_browser/webui_browser.h"
#include "ui/views/view.h"
#include "components/history_clusters/core/features.h"
#include "components/history_clusters/core/history_clusters_service.h"
#include "ui/actions/actions.h"

// static
void SidePanelHelper::PopulateGlobalEntries(
    Browser* browser,
    SidePanelRegistry* window_registry) {
  // Add reading list.
  browser->browser_window_features()
      ->reading_list_side_panel_coordinator()
      ->CreateAndRegisterEntry(window_registry);

  // Add bookmarks.
  browser->browser_window_features()
      ->bookmarks_side_panel_coordinator()
      ->CreateAndRegisterEntry(window_registry);

  if (webui_browser::IsWebUIBrowserEnabled()) {
    // TODO(webium): Consider supporting additional side panels beyond reading
    // list and bookmarks.
    return;
  }

  // Add history clusters.
  if (HistoryClustersSidePanelCoordinator::IsSupported(browser->profile()) &&
      !HistorySidePanelCoordinator::IsSupported()) {
    browser->GetFeatures()
        .history_clusters_side_panel_coordinator()
        ->CreateAndRegisterEntry(window_registry);
  }

  // Add history.
  if (HistorySidePanelCoordinator::IsSupported()) {
    browser->browser_window_features()
        ->history_side_panel_coordinator()
        ->CreateAndRegisterEntry(window_registry);
  }

  // Add comments.
  if (CommentsSidePanelCoordinator::IsSupported()) {
    browser->browser_window_features()
        ->comments_side_panel_coordinator()
        ->CreateAndRegisterEntry(window_registry);
  }

  // MoltBrowser: Add AI Chat side panel.
  // Register directly using a lambda that captures the browser pointer.
  // This avoids needing a long-lived coordinator instance.
  auto molt_ai_entry = std::make_unique<SidePanelEntry>(
      SidePanelEntry::Key(SidePanelEntry::Id::kMoltAiChat),
      base::BindRepeating(
          [](Browser* browser, SidePanelEntryScope& scope)
              -> std::unique_ptr<views::View> {
            // Pass the Browser* so the chat view can observe the
            // TabStripModel and inject the active tab's URL + title
            // into the chat WebUI for grounding.
            return std::make_unique<AiChatSidePanelWebView>(browser);
          },
          base::Unretained(browser)),
      // MoltBrowser: the redesigned chat UI is laid out for a 480px
      // default (upstream default is 360). Still user-resizable; a
      // user-dragged width persists per entry id and wins over this.
      /*default_content_width_callback=*/base::BindRepeating(
          []() { return 480; }));
  // MoltBrowser: the AI Chat page renders its OWN in-page header row
  // (title + overflow menu + close control), so suppress the entire native
  // side-panel header for this entry — that blanks the "AI Chat" title and
  // removes the native close X / pin. Scoped to this entry only; every other
  // side panel keeps its native header.
  molt_ai_entry->set_should_show_header(false);
  // MoltBrowser: the toolbar already has a dedicated "🤖 AI mode" button
  // (toolbar_view.cc) that toggles this panel, so suppress the DUPLICATE
  // ephemeral side-panel action button Chromium otherwise shows in the pinned
  // toolbar container while the panel is open — there should be exactly one
  // AI-mode control.
  molt_ai_entry->set_should_show_ephemerally_in_toolbar(false);
  window_registry->Register(std::move(molt_ai_entry));

  // MoltBrowser: Add the Agent-mode automation studio side panel. It is a
  // second kContent entry, so it shares the single content side panel with the
  // AI chat entry — opening Agent mode auto-swaps out the AI chat panel and
  // vice-versa (exactly the desired mutual exclusivity). Same header/ephemeral
  // suppression as the chat panel: the page draws its own header and the
  // toolbar "Agent mode" button is the one control.
  auto molt_agent_entry = std::make_unique<SidePanelEntry>(
      SidePanelEntry::Key(SidePanelEntry::Id::kMoltAgent),
      base::BindRepeating(
          [](Browser* browser, SidePanelEntryScope& scope)
              -> std::unique_ptr<views::View> {
            return std::make_unique<AgentSidePanelWebView>(browser);
          },
          base::Unretained(browser)),
      /*default_content_width_callback=*/base::BindRepeating(
          []() { return 480; }));
  molt_agent_entry->set_should_show_header(false);
  molt_agent_entry->set_should_show_ephemerally_in_toolbar(false);
  window_registry->Register(std::move(molt_agent_entry));
}

// static
actions::ActionItem* SidePanelHelper::GetActionItem(
    Browser* browser,
    SidePanelEntryKey entry_key) {
  BrowserActions* const browser_actions = browser->browser_actions();
  if (entry_key.id() == SidePanelEntryId::kExtension) {
    std::optional<actions::ActionId> extension_action_id =
        actions::ActionIdMap::StringToActionId(entry_key.ToString());
    CHECK(extension_action_id.has_value());
    actions::ActionItem* const action_item =
        actions::ActionManager::Get().FindAction(
            extension_action_id.value(), browser_actions->root_action_item());
    CHECK(action_item);
    return action_item;
  }

  std::optional<actions::ActionId> action_id =
      SidePanelEntryIdToActionId(entry_key.id());
  CHECK(action_id.has_value());
  return actions::ActionManager::Get().FindAction(
      action_id.value(), browser_actions->root_action_item());
}
