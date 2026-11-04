// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The browser's own buttons and menus, numbered for voice mode
// (docs/design/TOUCHLESS_BROWSING.md §6.3, "The browser UI too"; Phase 0's
// "the browser UI's own tree in the page map"): "show the browser's
// buttons" reads the toolbar, the tab strip, the bookmarks bar and the side
// panel's controls; "open the menu" reads the app menu; then a number or a
// name ("click Settings", "History") presses one, as a screen reader's
// default action does.
//
// The browser's side (voice_browser_ui.h) walks the window's views and
// the open menu, as their accessibility data gives them, into a flat list
// of BrowserUiNode; BuildBrowserUiMap() decides what is numbered:
//
//   - Only what can be pressed (buttons, menu items, tabs, links, check
//     boxes, toggles), visible, with a name, and not inside another such
//     item (a tab's close button: the tab is the item); in reading order,
//     the regions in a fixed order (the toolbar, the tab strip, the
//     bookmarks bar, the side panel). While a menu shows, only its items.
//   - Never what has its own spoken prompt or another protection: a
//     permission prompt or its chip, a download's warning, the password
//     bubble, and voice mode's own Voice button ("turn off voice mode"
//     asks first; a press here would not). These are left out with
//     everything inside them (kProtectedClasses), whatever their names say.
//   - Names are the browser's own words (a tab's title and a bookmark's
//     name are the page's and the user's: cleaned for speech); at most
//     kMaxBrowserUiItems items.
//
// Pure and std-only: unit-tested from stored trees in the voice harness and
// molt_ai_unittests.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_BROWSER_UI_MAP_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_BROWSER_UI_MAP_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace molt_ai {
namespace voice {

// Items numbered at most, nodes read at most, and a name's length in bytes.
inline constexpr size_t kMaxBrowserUiItems = 200;
inline constexpr size_t kMaxBrowserUiNodes = 4000;
inline constexpr size_t kMaxBrowserUiNameBytes = 200;

// Where a node is in the window.
enum class BrowserUiRegion : uint8_t {
  kMenu,  // the menu showing (the app menu, a submenu, a context menu)
  kToolbar,
  kTabStrip,
  kBookmarksBar,
  kSidePanel,
  kOther,
};

// The accessibility roles the map tells apart (ax::mojom::Role's, read by
// the browser's side).
enum class BrowserUiRole : uint8_t {
  kOther,
  kButton,
  kPopUpButton,  // opens a menu (the app menu button, "more")
  kToggleButton,
  kMenuItem,
  kMenuItemCheckBox,
  kMenuItemRadio,
  kTab,
  kLink,
  kCheckBox,
  kTextField,
  kGroup,
};

struct BrowserUiNode {
  BrowserUiNode();
  ~BrowserUiNode();
  BrowserUiNode(const BrowserUiNode&);
  BrowserUiNode& operator=(const BrowserUiNode&);
  BrowserUiNode(BrowserUiNode&&);
  BrowserUiNode& operator=(BrowserUiNode&&);

  // The view's accessibility id, unique while it lives; how a press finds
  // it again.
  int32_t id = 0;
  // The index of its parent in the list, -1 for a region's root. Parents
  // come before their children.
  int32_t parent = -1;
  BrowserUiRegion region = BrowserUiRegion::kOther;
  BrowserUiRole role = BrowserUiRole::kOther;
  // Its accessible name, and its description (a tooltip) when the name is
  // empty.
  std::string name;
  std::string description;
  // The view's class ("ToolbarButton", "MenuItemView").
  std::string class_name;
  bool visible = true;
  bool enabled = true;
  // Pressed or checked (a toggle, a checked menu item), and whether it
  // opens a submenu.
  bool checked = false;
  bool has_submenu = false;
};

struct BrowserUiItem {
  BrowserUiItem();
  ~BrowserUiItem();
  BrowserUiItem(const BrowserUiItem&);
  BrowserUiItem& operator=(const BrowserUiItem&);
  BrowserUiItem(BrowserUiItem&&);
  BrowserUiItem& operator=(BrowserUiItem&&);

  int number = 0;  // 1-based, as read
  int32_t id = 0;
  BrowserUiRegion region = BrowserUiRegion::kOther;
  BrowserUiRole role = BrowserUiRole::kOther;
  // Speech-safe.
  std::string name;
  bool enabled = true;
  bool checked = false;
  bool has_submenu = false;
};

struct BrowserUiMap {
  BrowserUiMap();
  ~BrowserUiMap();
  BrowserUiMap(const BrowserUiMap&);
  BrowserUiMap& operator=(const BrowserUiMap&);
  BrowserUiMap(BrowserUiMap&&);
  BrowserUiMap& operator=(BrowserUiMap&&);

  std::vector<BrowserUiItem> items;
  // A menu is showing: its items come first.
  bool menu_open = false;
  // Nodes left out because they were protected, for the log.
  size_t protected_nodes = 0;
};

// The classes whose views (and everything inside them) are never pressed
// by voice: the permission prompt and its chip, a download's warning, the
// password bubble, voice mode's own button.
const std::vector<std::string_view>& ProtectedBrowserUiClasses();

// Whether a view of |class_name| is protected (ProtectedBrowserUiClasses(),
// matched as a part of the name).
bool IsProtectedBrowserUiClass(std::string_view class_name);

// The map of |nodes| (BrowserUiNode's order: parents first; past
// kMaxBrowserUiNodes the rest are not read).
BrowserUiMap BuildBrowserUiMap(const std::vector<BrowserUiNode>& nodes);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_BROWSER_UI_MAP_H_
