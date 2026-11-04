// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/views/molt_ai/voice/voice_browser_ui_views.h"

#include <string>
#include <string_view>
#include <utility>

#include "base/no_destructor.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "ui/accessibility/ax_action_data.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/accessibility/ax_node_data.h"
#include "ui/base/base_window.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/controls/menu/menu_controller.h"
#include "ui/views/controls/menu/menu_item_view.h"
#include "ui/views/controls/menu/submenu_view.h"
#include "ui/views/view.h"
#include "ui/views/widget/widget.h"

namespace molt_ai {
namespace voice {

namespace {

// The regions read, by the class of the view that holds them (their
// ancestors are the window's frame and layout views, never pressed).
struct RegionClass {
  std::string_view class_name;
  BrowserUiRegion region;
};
constexpr RegionClass kRegions[] = {
    {"ToolbarView", BrowserUiRegion::kToolbar},
    {"TabStripRegionView", BrowserUiRegion::kTabStrip},
    {"BookmarkBarView", BrowserUiRegion::kBookmarksBar},
    {"SidePanel", BrowserUiRegion::kSidePanel},
};

// Views walked at most below a region's root, and how deep.
constexpr int kMaxDepth = 64;

BrowserUiRole RoleFor(ax::mojom::Role role) {
  switch (role) {
    case ax::mojom::Role::kButton:
      return BrowserUiRole::kButton;
    case ax::mojom::Role::kPopUpButton:
      return BrowserUiRole::kPopUpButton;
    case ax::mojom::Role::kToggleButton:
      return BrowserUiRole::kToggleButton;
    case ax::mojom::Role::kMenuItem:
      return BrowserUiRole::kMenuItem;
    case ax::mojom::Role::kMenuItemCheckBox:
      return BrowserUiRole::kMenuItemCheckBox;
    case ax::mojom::Role::kMenuItemRadio:
      return BrowserUiRole::kMenuItemRadio;
    case ax::mojom::Role::kTab:
      return BrowserUiRole::kTab;
    case ax::mojom::Role::kLink:
      return BrowserUiRole::kLink;
    case ax::mojom::Role::kCheckBox:
      return BrowserUiRole::kCheckBox;
    case ax::mojom::Role::kTextField:
    case ax::mojom::Role::kTextFieldWithComboBox:
      return BrowserUiRole::kTextField;
    case ax::mojom::Role::kGroup:
    case ax::mojom::Role::kToolbar:
    case ax::mojom::Role::kTabList:
      return BrowserUiRole::kGroup;
    default:
      return BrowserUiRole::kOther;
  }
}

std::string ClassOf(const views::View* view) {
  return std::string(view->GetClassMetaData()->type_name());
}

// |view| as a node of |region|, under node |parent|.
BrowserUiNode NodeFor(views::View* view,
                      int32_t parent,
                      BrowserUiRegion region) {
  BrowserUiNode node;
  ui::AXNodeData data;
  views::ViewAccessibility& accessibility = view->GetViewAccessibility();
  accessibility.GetAccessibleNodeData(&data);
  node.id = accessibility.GetUniqueId().value();
  node.parent = parent;
  node.region = region;
  node.role = RoleFor(data.role);
  node.name = data.GetStringAttribute(ax::mojom::StringAttribute::kName);
  node.description =
      data.GetStringAttribute(ax::mojom::StringAttribute::kDescription);
  node.class_name = ClassOf(view);
  node.visible =
      view->IsDrawn() && !data.HasState(ax::mojom::State::kInvisible);
  node.enabled = view->GetEnabled() &&
                 data.GetRestriction() != ax::mojom::Restriction::kDisabled;
  node.checked = data.GetCheckedState() == ax::mojom::CheckedState::kTrue;
  node.has_submenu = data.GetHasPopup() != ax::mojom::HasPopup::kFalse;
  return node;
}

// Everything under |view| (it included), parents first.
void Walk(views::View* view,
          int32_t parent,
          BrowserUiRegion region,
          int depth,
          std::vector<BrowserUiNode>& nodes,
          std::vector<views::View*>& views_out) {
  if (nodes.size() >= kMaxBrowserUiNodes || depth > kMaxDepth) {
    return;
  }
  const int32_t index = static_cast<int32_t>(nodes.size());
  nodes.push_back(NodeFor(view, parent, region));
  views_out.push_back(view);
  for (views::View* child : view->children()) {
    Walk(child, index, region, depth + 1, nodes, views_out);
  }
}

// Finds the regions' roots under |view|.
void FindRegions(views::View* view,
                 int depth,
                 std::vector<BrowserUiNode>& nodes,
                 std::vector<views::View*>& views_out) {
  if (depth > kMaxDepth || nodes.size() >= kMaxBrowserUiNodes) {
    return;
  }
  const std::string class_name = ClassOf(view);
  for (const RegionClass& region : kRegions) {
    if (class_name == region.class_name) {
      Walk(view, -1, region.region, 0, nodes, views_out);
      return;
    }
  }
  for (views::View* child : view->children()) {
    FindRegions(child, depth + 1, nodes, views_out);
  }
}

views::Widget* WidgetOf(BrowserWindowInterface* browser) {
  ui::BaseWindow* window = browser ? browser->GetWindow() : nullptr;
  return window ? views::Widget::GetWidgetForNativeWindow(
                      window->GetNativeWindow())
                : nullptr;
}

// The menu that shows over |widget|'s window: the submenu open deepest.
views::SubmenuView* ShowingSubmenu(views::Widget* widget) {
  views::MenuController* controller =
      views::MenuController::GetActiveInstance();
  if (!controller || !widget || controller->owner() != widget) {
    return nullptr;
  }
  views::MenuItemView* item = controller->GetSelectedMenuItem();
  if (!item) {
    return nullptr;
  }
  if (item->HasSubmenu() && item->GetSubmenu()->IsShowing()) {
    return item->GetSubmenu();
  }
  views::MenuItemView* parent = item->GetParentMenuItem();
  if (parent && parent->HasSubmenu() && parent->GetSubmenu()->IsShowing()) {
    return parent->GetSubmenu();
  }
  return nullptr;
}

// The nodes of |browser|'s window and the views they are of.
void CollectWithViews(BrowserWindowInterface* browser,
                      std::vector<BrowserUiNode>& nodes,
                      std::vector<views::View*>& views_out) {
  views::Widget* widget = WidgetOf(browser);
  if (!widget) {
    return;
  }
  // The menu first: its items, each a node of its own.
  if (views::SubmenuView* submenu = ShowingSubmenu(widget)) {
    for (views::MenuItemView* item : submenu->GetMenuItems()) {
      if (nodes.size() >= kMaxBrowserUiNodes) {
        break;
      }
      nodes.push_back(NodeFor(item, -1, BrowserUiRegion::kMenu));
      views_out.push_back(item);
    }
  }
  if (views::View* root = widget->GetRootView()) {
    FindRegions(root, 0, nodes, views_out);
  }
}

}  // namespace

// static
void VoiceBrowserUiViews::EnsureInstalled() {
  static base::NoDestructor<VoiceBrowserUiViews> ui;
  if (VoiceBrowserUi::Get() != ui.get()) {
    VoiceBrowserUi::Install(ui.get());
  }
}

VoiceBrowserUiViews::VoiceBrowserUiViews() = default;
VoiceBrowserUiViews::~VoiceBrowserUiViews() = default;

std::vector<BrowserUiNode> VoiceBrowserUiViews::Collect(
    BrowserWindowInterface* browser) {
  std::vector<BrowserUiNode> nodes;
  std::vector<views::View*> views_out;
  CollectWithViews(browser, nodes, views_out);
  return nodes;
}

bool VoiceBrowserUiViews::Press(BrowserWindowInterface* browser, int32_t id) {
  std::vector<BrowserUiNode> nodes;
  std::vector<views::View*> views_out;
  CollectWithViews(browser, nodes, views_out);
  for (size_t i = 0; i < nodes.size(); ++i) {
    if (nodes[i].id != id) {
      continue;
    }
    // Never a protected view, nor one inside one; only what shows and
    // can be pressed now.
    for (int32_t at = static_cast<int32_t>(i); at >= 0;
         at = nodes[static_cast<size_t>(at)].parent) {
      const BrowserUiNode& node = nodes[static_cast<size_t>(at)];
      if (IsProtectedBrowserUiClass(node.class_name) || !node.visible) {
        return false;
      }
      if (node.parent >= at) {
        break;
      }
    }
    if (!nodes[i].enabled) {
      return false;
    }
    ui::AXActionData action;
    action.action = ax::mojom::Action::kDoDefault;
    return views_out[i]->HandleAccessibleAction(action);
  }
  return false;
}

bool VoiceBrowserUiViews::MenuOpen(BrowserWindowInterface* browser) {
  return ShowingSubmenu(WidgetOf(browser)) != nullptr;
}

void VoiceBrowserUiViews::CloseMenu(BrowserWindowInterface* browser) {
  views::MenuController* controller =
      views::MenuController::GetActiveInstance();
  views::Widget* widget = WidgetOf(browser);
  if (controller && widget && controller->owner() == widget) {
    controller->Cancel(views::MenuController::ExitType::kAll);
  }
}

}  // namespace voice
}  // namespace molt_ai
