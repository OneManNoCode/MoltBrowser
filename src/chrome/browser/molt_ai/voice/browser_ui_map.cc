// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/browser_ui_map.h"

#include <algorithm>
#include <utility>

#include "chrome/browser/molt_ai/voice/spoken_text.h"

namespace molt_ai {
namespace voice {

namespace {

bool Pressable(BrowserUiRole role) {
  switch (role) {
    case BrowserUiRole::kButton:
    case BrowserUiRole::kPopUpButton:
    case BrowserUiRole::kToggleButton:
    case BrowserUiRole::kMenuItem:
    case BrowserUiRole::kMenuItemCheckBox:
    case BrowserUiRole::kMenuItemRadio:
    case BrowserUiRole::kTab:
    case BrowserUiRole::kLink:
    case BrowserUiRole::kCheckBox:
      return true;
    case BrowserUiRole::kOther:
    case BrowserUiRole::kTextField:
    case BrowserUiRole::kGroup:
      return false;
  }
  return false;
}

int RegionOrder(BrowserUiRegion region) {
  switch (region) {
    case BrowserUiRegion::kMenu:
      return 0;
    case BrowserUiRegion::kToolbar:
      return 1;
    case BrowserUiRegion::kTabStrip:
      return 2;
    case BrowserUiRegion::kBookmarksBar:
      return 3;
    case BrowserUiRegion::kSidePanel:
      return 4;
    case BrowserUiRegion::kOther:
      return 5;
  }
  return 5;
}

}  // namespace

BrowserUiNode::BrowserUiNode() = default;
BrowserUiNode::~BrowserUiNode() = default;
BrowserUiNode::BrowserUiNode(const BrowserUiNode&) = default;
BrowserUiNode& BrowserUiNode::operator=(const BrowserUiNode&) = default;
BrowserUiNode::BrowserUiNode(BrowserUiNode&&) = default;
BrowserUiNode& BrowserUiNode::operator=(BrowserUiNode&&) = default;

BrowserUiItem::BrowserUiItem() = default;
BrowserUiItem::~BrowserUiItem() = default;
BrowserUiItem::BrowserUiItem(const BrowserUiItem&) = default;
BrowserUiItem& BrowserUiItem::operator=(const BrowserUiItem&) = default;
BrowserUiItem::BrowserUiItem(BrowserUiItem&&) = default;
BrowserUiItem& BrowserUiItem::operator=(BrowserUiItem&&) = default;

BrowserUiMap::BrowserUiMap() = default;
BrowserUiMap::~BrowserUiMap() = default;
BrowserUiMap::BrowserUiMap(const BrowserUiMap&) = default;
BrowserUiMap& BrowserUiMap::operator=(const BrowserUiMap&) = default;
BrowserUiMap::BrowserUiMap(BrowserUiMap&&) = default;
BrowserUiMap& BrowserUiMap::operator=(BrowserUiMap&&) = default;

const std::vector<std::string_view>& ProtectedBrowserUiClasses() {
  static const std::vector<std::string_view>* const classes =
      new std::vector<std::string_view>{
          // Permission prompts, their chips and the page info's
          // permissions: answered by their own spoken prompt.
          "Permission",
          // A download's warning (the bubble, its rows, the shelf).
          "DownloadBubble",
          "DownloadItem",
          "DownloadShelf",
          "DownloadDanger",
          // The password manager's bubble.
          "PasswordBubble",
          "PasswordSave",
          "ManagePasswords",
          // Voice mode's own button: "turn off voice mode" asks first.
          "VoiceToolbarButton",
      };
  return *classes;
}

bool IsProtectedBrowserUiClass(std::string_view class_name) {
  for (std::string_view part : ProtectedBrowserUiClasses()) {
    if (class_name.find(part) != std::string_view::npos) {
      return true;
    }
  }
  return false;
}

BrowserUiMap BuildBrowserUiMap(const std::vector<BrowserUiNode>& nodes) {
  BrowserUiMap map;
  const size_t count = std::min(nodes.size(), kMaxBrowserUiNodes);
  // Per node: left out (protected, hidden), and inside something pressable
  // (a tab's close button: the tab is the item).
  std::vector<char> protected_node(count, 0);
  std::vector<char> hidden(count, 0);
  std::vector<char> inside_pressable(count, 0);
  struct Found {
    size_t node;
    int order;
  };
  std::vector<Found> found;
  for (size_t i = 0; i < count; ++i) {
    const BrowserUiNode& node = nodes[i];
    const int32_t parent = node.parent;
    const bool has_parent = parent >= 0 && static_cast<size_t>(parent) < i;
    const size_t p = has_parent ? static_cast<size_t>(parent) : 0;
    protected_node[i] = IsProtectedBrowserUiClass(node.class_name) ||
                        (has_parent && protected_node[p]);
    hidden[i] = !node.visible || (has_parent && hidden[p]);
    inside_pressable[i] =
        has_parent &&
        (inside_pressable[p] || (Pressable(nodes[p].role) &&
                                 nodes[p].region != BrowserUiRegion::kMenu));
    if (protected_node[i]) {
      ++map.protected_nodes;
      continue;
    }
    if (hidden[i] || !Pressable(node.role) || inside_pressable[i]) {
      continue;
    }
    if (node.region == BrowserUiRegion::kMenu) {
      map.menu_open = true;
    }
    found.push_back({i, RegionOrder(node.region)});
  }
  // While a menu shows, only its items: a press anywhere else would close
  // it first.
  if (map.menu_open) {
    std::erase_if(found, [&](const Found& f) {
      return nodes[f.node].region != BrowserUiRegion::kMenu;
    });
  }
  std::stable_sort(
      found.begin(), found.end(),
      [](const Found& a, const Found& b) { return a.order < b.order; });
  for (const Found& f : found) {
    if (map.items.size() >= kMaxBrowserUiItems) {
      break;
    }
    const BrowserUiNode& node = nodes[f.node];
    std::string name =
        CleanSpokenText(node.name.empty() ? node.description : node.name,
                        kMaxBrowserUiNameBytes);
    if (name.empty()) {
      continue;
    }
    BrowserUiItem item;
    item.number = static_cast<int>(map.items.size()) + 1;
    item.id = node.id;
    item.region = node.region;
    item.role = node.role;
    item.name = std::move(name);
    item.enabled = node.enabled;
    item.checked = node.checked;
    item.has_submenu = node.has_submenu;
    map.items.push_back(std::move(item));
  }
  return map;
}

}  // namespace voice
}  // namespace molt_ai
