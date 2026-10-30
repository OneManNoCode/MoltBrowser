// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/ax_page_source.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/accessibility/ax_node.h"
#include "ui/accessibility/ax_node_data.h"
#include "ui/accessibility/ax_tree.h"
#include "ui/accessibility/ax_tree_data.h"
#include "ui/accessibility/ax_tree_id.h"
#include "ui/accessibility/ax_tree_update.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/geometry/vector2d_f.h"

namespace molt_ai {
namespace voice {

namespace {

using ax::mojom::Role;

// At most |max_bytes| of |s|, cut before a character that would be split.
std::string Prefix(const std::string& s, size_t max_bytes) {
  if (s.size() <= max_bytes) {
    return s;
  }
  size_t end = max_bytes;
  while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) {
    --end;
  }
  return s.substr(0, end);
}

std::string CappedString(const ui::AXNodeData& data,
                         ax::mojom::StringAttribute attribute,
                         size_t max_bytes) {
  return Prefix(data.GetStringAttribute(attribute), max_bytes);
}

// A position as the page map takes it; non-finite values read as 0.
float Finite(float v) {
  return std::isfinite(v) ? v : 0;
}

constexpr uint32_t kNoParent = 0xFFFFFFFFu;

// An element's id and class attributes are cut to this many bytes.
constexpr size_t kMaxIdClassBytes = 256;

// AXTree::GetTreeBounds() measures a node from its offset container, that
// container from its own, and so on (the root stands in for an id that
// names no node) until a node is its own container
// (AXTree::RelativeToTreeBoundsInternal()). The ids come from the renderer,
// and the tree takes them as they are: containers that lead back to each
// other would keep that walk going forever, and a very long chain would
// make every node's bounds slow. A node is measured only when its chain
// ends within |max_hops| (Blink's containers are the node's ancestors, so a
// node of the walk has no longer one); each node's chain is followed once.
class OffsetChains {
 public:
  OffsetChains(const ui::AXTree& tree, size_t max_hops)
      : tree_(&tree), max_hops_(max_hops) {}
  OffsetChains(const OffsetChains&) = delete;
  OffsetChains& operator=(const OffsetChains&) = delete;

  bool Ends(const ui::AXNode& start) {
    std::vector<const ui::AXNode*> path;
    const ui::AXNode* node = &start;
    // The hops from the node after the path's last to the chain's end;
    // kLoops if the chain loops. -1: the path's last node ends it.
    int64_t after = -1;
    while (true) {
      auto [it, added] = state_.try_emplace(node->id(), kWalking);
      if (!added) {
        // Back to a node of this walk (a loop), or one already known.
        after = it->second == kWalking ? kLoops : it->second;
        break;
      }
      path.push_back(node);
      const ui::AXNode* next =
          tree_->GetFromId(node->data().relative_bounds.offset_container_id);
      if (!next) {
        next = tree_->root();
      }
      if (!next || next == node) {
        break;
      }
      node = next;
    }
    for (size_t i = path.size(); i-- > 0;) {
      after = after == kLoops ? kLoops : after + 1;
      state_[path[i]->id()] = after;
    }
    const int64_t hops = state_[start.id()];
    return hops >= 0 && static_cast<size_t>(hops) <= max_hops_;
  }

 private:
  static constexpr int64_t kWalking = -2;
  static constexpr int64_t kLoops = -3;

  raw_ptr<const ui::AXTree> tree_;
  const size_t max_hops_;
  // Per node: kWalking, kLoops, or the hops from it to the chain's end.
  std::unordered_map<ui::AXNodeID, int64_t> state_;
};

// One level of the walk: a node, the frame it is in, and how far its
// children have been taken.
struct Frame {
  raw_ptr<const ui::AXNode> node = nullptr;
  raw_ptr<const ui::AXTree> tree = nullptr;
  int32_t tree_index = -1;
  uint32_t page_index = 0;  // in PageTree::nodes
  uint32_t depth = 0;
  gfx::Vector2dF offset;  // the frame's position in the page
  bool frame_offscreen = false;
  // The nearest ancestor (in the node's own tree) with a width or a height:
  // where GetTreeBounds() takes a node's size from when it has none.
  raw_ptr<const ui::AXNode> sized_ancestor = nullptr;
  size_t next_child = 0;
  bool child_tree_done = false;
};

}  // namespace

bool TreeCanTakeUpdate(const ui::AXTree& tree, const ui::AXTreeUpdate& update) {
  if (tree.root()) {
    return true;
  }
  return update.root_id != ui::kInvalidAXNodeID && !update.nodes.empty() &&
         update.nodes[0].id == update.root_id;
}

PageRole PageRoleFor(const ui::AXNodeData& data) {
  if (data.GetBoolAttribute(
          ax::mojom::BoolAttribute::kNonAtomicTextFieldRoot) &&
      data.HasState(ax::mojom::State::kEditable)) {
    return data.HasState(ax::mojom::State::kMultiline) ? PageRole::kTextArea
                                                       : PageRole::kTextField;
  }
  switch (data.role) {
    case Role::kRootWebArea:
      return PageRole::kRootWebArea;
    case Role::kStaticText:
      return PageRole::kStaticText;
    case Role::kImage:
    case Role::kSvgRoot:
    case Role::kGraphicsDocument:
      return PageRole::kImage;
    case Role::kLink:
    case Role::kDocBackLink:
    case Role::kDocBiblioRef:
    case Role::kDocGlossRef:
    case Role::kDocNoteRef:
      return PageRole::kLink;
    case Role::kButton:
    case Role::kToggleButton:
    case Role::kPopUpButton:
    case Role::kComboBoxMenuButton:
    case Role::kDisclosureTriangle:
      return PageRole::kButton;
    case Role::kTextField:
      return data.HasState(ax::mojom::State::kMultiline) ? PageRole::kTextArea
                                                         : PageRole::kTextField;
    case Role::kSearchBox:
      return PageRole::kSearchBox;
    case Role::kTextFieldWithComboBox:
      return PageRole::kComboBox;
    case Role::kComboBoxSelect:
      return PageRole::kSelect;
    case Role::kCheckBox:
      return PageRole::kCheckBox;
    case Role::kRadioButton:
      return PageRole::kRadio;
    case Role::kSwitch:
      return PageRole::kSwitch;
    case Role::kSlider:
    case Role::kSpinButton:
      return PageRole::kSlider;
    case Role::kListBoxOption:
    case Role::kMenuListOption:
      return PageRole::kOption;
    case Role::kMenuItem:
    case Role::kMenuItemCheckBox:
    case Role::kMenuItemRadio:
      return PageRole::kMenuItem;
    case Role::kTab:
      return PageRole::kTab;
    case Role::kTreeItem:
      return PageRole::kTreeItem;
    case Role::kListBox:
      return PageRole::kListBox;
    case Role::kList:
      return PageRole::kList;
    case Role::kListItem:
      return PageRole::kListItem;
    case Role::kFeed:
      return PageRole::kFeed;
    case Role::kArticle:
      return PageRole::kArticle;
    case Role::kTable:
      return PageRole::kTable;
    case Role::kGrid:
    case Role::kTreeGrid:
    case Role::kListGrid:
      return PageRole::kGrid;
    case Role::kRow:
      return PageRole::kRow;
    case Role::kCell:
      return PageRole::kCell;
    case Role::kColumnHeader:
      return PageRole::kColumnHeader;
    case Role::kRowHeader:
      return PageRole::kRowHeader;
    case Role::kHeading:
      return PageRole::kHeading;
    case Role::kVideo:
      return PageRole::kVideo;
    case Role::kAudio:
      return PageRole::kAudio;
    case Role::kMain:
      return PageRole::kMain;
    case Role::kNavigation:
    case Role::kDocToc:
      return PageRole::kNavigation;
    case Role::kBanner:
    case Role::kHeader:
      return PageRole::kBanner;
    case Role::kContentInfo:
    case Role::kFooter:
      return PageRole::kContentInfo;
    case Role::kComplementary:
      return PageRole::kComplementary;
    case Role::kSearch:
      return PageRole::kSearch;
    case Role::kForm:
      return PageRole::kForm;
    case Role::kRegion:
      return PageRole::kRegion;
    case Role::kDialog:
    case Role::kAlertDialog:
      return PageRole::kDialog;
    case Role::kMenu:
    case Role::kMenuBar:
    case Role::kMenuListPopup:
      return PageRole::kMenu;
    case Role::kIframe:
    case Role::kIframePresentational:
      return PageRole::kIframe;
    case Role::kParagraph:
      return PageRole::kParagraph;
    case Role::kTime:
      return PageRole::kTime;
    default:
      return PageRole::kGeneric;
  }
}

PageNode PageNodeFromData(const ui::AXNodeData& data,
                          int32_t tree_index,
                          size_t max_string_bytes) {
  using ax::mojom::State;
  using ax::mojom::StringAttribute;
  PageNode node;
  node.handle = {tree_index, data.id};
  node.role = PageRoleFor(data);

  uint32_t states = 0;
  auto set = [&](bool on, uint32_t bit) {
    if (on) {
      states |= bit;
    }
  };
  set(data.HasState(State::kFocusable), kStateFocusable);
  set(data.HasState(State::kEditable), kStateEditable);
  set(data.GetRestriction() == ax::mojom::Restriction::kDisabled,
      kStateDisabled);
  set(data.GetRestriction() == ax::mojom::Restriction::kReadOnly,
      kStateReadOnly);
  set(data.GetCheckedState() == ax::mojom::CheckedState::kTrue, kStateChecked);
  set(data.GetCheckedState() == ax::mojom::CheckedState::kMixed, kStateMixed);
  set(data.HasState(State::kExpanded), kStateExpanded);
  set(data.HasState(State::kCollapsed), kStateCollapsed);
  set(data.HasState(State::kInvisible), kStateInvisible);
  set(data.IsIgnored() || data.role == Role::kNone, kStateIgnored);
  set(data.HasState(State::kProtected), kStateProtected);
  set(data.HasState(State::kRequired), kStateRequired);
  set(data.GetBoolAttribute(ax::mojom::BoolAttribute::kSelected),
      kStateSelected);
  set(data.HasState(State::kMultiline), kStateMultiline);
  const ax::mojom::DefaultActionVerb verb = data.GetDefaultActionVerb();
  set(verb != ax::mojom::DefaultActionVerb::kNone &&
          verb != ax::mojom::DefaultActionVerb::kClickAncestor,
      kStateHasDefaultAction);
  node.states = states;

  node.name = CappedString(data, StringAttribute::kName, max_string_bytes);
  node.value = CappedString(data, StringAttribute::kValue, max_string_bytes);
  node.description =
      CappedString(data, StringAttribute::kDescription, max_string_bytes);
  node.placeholder =
      CappedString(data, StringAttribute::kPlaceholder, max_string_bytes);
  node.url = CappedString(data, StringAttribute::kUrl, max_string_bytes);
  node.html_tag =
      CappedString(data, StringAttribute::kHtmlTag, max_string_bytes);
  node.input_type =
      CappedString(data, StringAttribute::kInputType, max_string_bytes);
  node.autocomplete =
      CappedString(data, StringAttribute::kAutoComplete, max_string_bytes);
  // What a site skill may name (site_skills.h); short.
  const size_t attribute_bytes = std::min(max_string_bytes, kMaxIdClassBytes);
  node.html_id = CappedString(data, StringAttribute::kHtmlId, attribute_bytes);
  node.class_name =
      CappedString(data, StringAttribute::kClassName, attribute_bytes);
  if (node.role == PageRole::kHeading) {
    node.heading_level =
        data.GetIntAttribute(ax::mojom::IntAttribute::kHierarchicalLevel);
  }
  node.pos_in_set = data.GetIntAttribute(ax::mojom::IntAttribute::kPosInSet);
  node.set_size = data.GetIntAttribute(ax::mojom::IntAttribute::kSetSize);
  return node;
}

PageTree BuildPageTree(const AXFrameTree& main,
                       AXTreeLookup lookup,
                       const AXPageSourceLimits& limits) {
  PageTree page;
  if (!main.tree || !main.tree->root() || limits.max_nodes == 0) {
    return page;
  }
  page.title = Prefix(main.tree->data().title, limits.max_string_bytes);

  // The focused frame (the browser names it in the top-level tree's data)
  // and its focused node.
  AXFrameTree focus_frame = main;
  const ui::AXTreeID& focused_tree = main.tree->data().focused_tree_id;
  if (focused_tree != ui::AXTreeIDUnknown() &&
      focused_tree != main.tree->GetAXTreeID()) {
    const AXFrameTree found = lookup(focused_tree);
    if (found.tree) {
      focus_frame = found;
    }
  }
  const ui::AXNodeID focus_id = focus_frame.tree->data().focus_id;

  // The selection: the focused frame's, else the top-level document's.
  const std::array<const AXFrameTree*, 2> selection_frames = {&focus_frame,
                                                              &main};
  for (const AXFrameTree* frame : selection_frames) {
    const ui::AXTreeData& data = frame->tree->data();
    if (data.sel_anchor_object_id == ui::kInvalidAXNodeID ||
        data.sel_focus_object_id == ui::kInvalidAXNodeID) {
      continue;
    }
    page.selection.anchor = {frame->index, data.sel_anchor_object_id};
    page.selection.anchor_offset = data.sel_anchor_offset;
    page.selection.focus = {frame->index, data.sel_focus_object_id};
    page.selection.focus_offset = data.sel_focus_offset;
    page.selection.backward = data.sel_is_backward;
    break;
  }

  // Each tree's offset containers, checked as its nodes are measured.
  std::map<int32_t, OffsetChains> chains;
  // Whether GetTreeBounds(|ax_node|) ends: it follows the node's chain of
  // offset containers; when the node has no size, its children's too; and
  // when what it found has none, its nearest ancestor with a size's.
  auto measurable = [&](const ui::AXNode* ax_node, const Frame& where) {
    OffsetChains& tree_chains =
        chains.try_emplace(where.tree_index, *where.tree, limits.max_depth + 2)
            .first->second;
    if (!tree_chains.Ends(*ax_node)) {
      return false;
    }
    if (ax_node->data().relative_bounds.bounds.IsEmpty()) {
      for (const auto& child : ax_node->children()) {
        if (!tree_chains.Ends(*child)) {
          return false;
        }
      }
    }
    return !where.sized_ancestor || tree_chains.Ends(*where.sized_ancestor);
  };

  // Adds |ax_node| under |parent| (kNoParent for the root); returns its
  // index in |page|.
  auto add = [&](const ui::AXNode* ax_node, const Frame& where,
                 uint32_t parent) {
    const ui::AXNodeData& data = ax_node->data();
    PageNode node =
        PageNodeFromData(data, where.tree_index, limits.max_string_bytes);
    bool offscreen = false;
    if (measurable(ax_node, where)) {
      gfx::RectF bounds = where.tree->GetTreeBounds(ax_node, &offscreen,
                                                    /*clip_bounds=*/false);
      bounds.Offset(where.offset);
      node.bounds = {Finite(bounds.x()), Finite(bounds.y()),
                     Finite(bounds.width()), Finite(bounds.height())};
    } else {
      offscreen = true;  // nowhere to show it: no bounds
    }
    if (offscreen || where.frame_offscreen) {
      node.states |= kStateOffscreen;
    }
    if (where.tree_index == focus_frame.index && data.id == focus_id) {
      node.states |= kStateFocused;
      page.focus = node.handle;
    }
    const uint32_t index = static_cast<uint32_t>(page.nodes.size());
    page.nodes.push_back(std::move(node));
    if (parent != kNoParent) {
      page.nodes[parent].children.push_back(index);
    }
    return index;
  };

  // A pre-order walk with one stack entry per level (each walks its
  // children lazily), so the stack never holds more than max_depth
  // entries, and the nodes taken are exactly the first max_nodes, of which
  // at most max_frame_nodes in child frames.
  std::vector<int32_t> trees_seen = {main.index};
  size_t frame_nodes = 0;
  std::vector<Frame> stack;
  Frame root;
  root.node = main.tree->root();
  root.tree = main.tree;
  root.tree_index = main.index;
  root.page_index = add(main.tree->root(), root, kNoParent);
  stack.push_back(root);
  while (!stack.empty() && page.nodes.size() < limits.max_nodes) {
    Frame& top = stack.back();
    const bool in_frame = top.tree_index != main.index;
    if (top.depth >= limits.max_depth ||
        (in_frame && frame_nodes >= limits.max_frame_nodes)) {
      stack.pop_back();
      continue;
    }
    const auto& children = top.node->children();
    if (top.next_child < children.size()) {
      Frame child = top;  // the same frame, one level down
      child.node = children[top.next_child++].get();
      child.depth = top.depth + 1;
      child.next_child = 0;
      child.child_tree_done = false;
      const gfx::RectF& parent_bounds = top.node->data().relative_bounds.bounds;
      if (parent_bounds.width() > 0 || parent_bounds.height() > 0) {
        child.sized_ancestor = top.node;
      }
      child.page_index = add(child.node, child, top.page_index);
      frame_nodes += in_frame ? 1 : 0;
      stack.push_back(child);
      continue;
    }
    // After a host's own children (Blink gives an iframe none), the frame
    // it hosts, at the host's position: only a frame whose parent, as the
    // browser records it, is the host's frame.
    if (!top.child_tree_done) {
      top.child_tree_done = true;
      std::optional<ui::AXTreeID> child_tree =
          top.node->data().GetChildTreeID();
      if (child_tree && trees_seen.size() < limits.max_trees &&
          frame_nodes < limits.max_frame_nodes) {
        const AXFrameTree frame = lookup(*child_tree);
        if (frame.tree && frame.tree->root() &&
            frame.tree->data().parent_tree_id == top.tree->GetAXTreeID() &&
            std::find(trees_seen.begin(), trees_seen.end(), frame.index) ==
                trees_seen.end()) {
          trees_seen.push_back(frame.index);
          const PageNode& host = page.nodes[top.page_index];
          Frame inner;
          inner.node = frame.tree->root();
          inner.tree = frame.tree;
          inner.tree_index = frame.index;
          inner.depth = top.depth + 1;
          inner.offset = gfx::Vector2dF(host.bounds.x, host.bounds.y);
          inner.frame_offscreen = host.Has(kStateOffscreen);
          const uint32_t host_index = top.page_index;
          inner.page_index = add(inner.node, inner, host_index);
          ++frame_nodes;
          stack.push_back(inner);
          continue;
        }
      }
    }
    stack.pop_back();
  }
  return page;
}

}  // namespace voice
}  // namespace molt_ai
