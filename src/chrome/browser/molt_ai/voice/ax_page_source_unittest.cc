// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/ax_page_source.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/accessibility/ax_node_data.h"
#include "ui/accessibility/ax_tree.h"
#include "ui/accessibility/ax_tree_data.h"
#include "ui/accessibility/ax_tree_id.h"
#include "ui/accessibility/ax_tree_update.h"
#include "ui/gfx/geometry/rect_f.h"

namespace molt_ai {
namespace voice {
namespace {

using ax::mojom::Role;
using ax::mojom::State;
using ax::mojom::StringAttribute;

ui::AXNodeData Node(int id,
                    Role role,
                    const std::string& name = std::string(),
                    std::vector<int32_t> children = {}) {
  ui::AXNodeData data;
  data.id = id;
  data.role = role;
  if (!name.empty()) {
    data.AddStringAttribute(StringAttribute::kName, name);
  }
  data.child_ids = std::move(children);
  data.relative_bounds.bounds = gfx::RectF(0, 0, 800, 600);
  return data;
}

// Holds frames' trees as the controller does, and looks them up.
class Frames {
 public:
  ui::AXTree* Add(int32_t index,
                  const ui::AXTreeID& id,
                  std::vector<ui::AXNodeData> nodes,
                  const std::string& title = std::string()) {
    ui::AXTreeUpdate update;
    update.has_tree_data = true;
    update.tree_data.tree_id = id;
    update.tree_data.title = title;
    update.root_id = nodes.front().id;
    update.nodes = std::move(nodes);
    auto tree = std::make_unique<ui::AXTree>(update);
    ui::AXTree* raw = tree.get();
    trees_[id] = {index, std::move(tree)};
    return raw;
  }

  AXFrameTree Get(const ui::AXTreeID& id) const {
    auto it = trees_.find(id);
    if (it == trees_.end()) {
      return AXFrameTree();
    }
    return AXFrameTree{it->second.first, it->second.second.get()};
  }

  PageTree Build(const ui::AXTreeID& main,
                 const AXPageSourceLimits& limits = AXPageSourceLimits()) {
    return BuildPageTree(
        Get(main), [this](const ui::AXTreeID& id) { return Get(id); }, limits);
  }

 private:
  std::map<ui::AXTreeID, std::pair<int32_t, std::unique_ptr<ui::AXTree>>>
      trees_;
};

TEST(AXPageSourceTest, ConvertsRolesStatesAndStrings) {
  ui::AXNodeData link = Node(3, Role::kLink, "Watch");
  link.AddStringAttribute(StringAttribute::kUrl, "https://a.example/watch");
  link.AddState(State::kFocusable);
  link.SetDefaultActionVerb(ax::mojom::DefaultActionVerb::kJump);

  ui::AXNodeData field = Node(4, Role::kTextField, "Password");
  field.AddState(State::kProtected);
  field.AddState(State::kEditable);
  field.AddStringAttribute(StringAttribute::kInputType, "password");
  field.AddStringAttribute(StringAttribute::kAutoComplete, "current-password");
  field.AddStringAttribute(StringAttribute::kPlaceholder, "Your password");
  field.SetValue("hunter2");

  ui::AXNodeData check = Node(5, Role::kCheckBox, "Remember me");
  check.SetCheckedState(ax::mojom::CheckedState::kTrue);
  check.SetRestriction(ax::mojom::Restriction::kDisabled);

  ui::AXNodeData heading = Node(6, Role::kHeading, "Results");
  heading.AddIntAttribute(ax::mojom::IntAttribute::kHierarchicalLevel, 3);

  ui::AXNodeData ignored = Node(7, Role::kGenericContainer);
  ignored.AddState(State::kIgnored);

  ui::AXNodeData area = Node(8, Role::kTextField, "Comment");
  area.AddState(State::kMultiline);
  area.AddState(State::kEditable);

  ui::AXNodeData editable = Node(9, Role::kGenericContainer, "Reply");
  editable.AddState(State::kEditable);
  editable.AddBoolAttribute(ax::mojom::BoolAttribute::kNonAtomicTextFieldRoot,
                            true);

  ui::AXNodeData clicky = Node(10, Role::kGenericContainer);
  clicky.SetDefaultActionVerb(ax::mojom::DefaultActionVerb::kClickAncestor);

  EXPECT_EQ(PageRoleFor(Node(1, Role::kRootWebArea)), PageRole::kRootWebArea);
  EXPECT_EQ(PageRoleFor(Node(1, Role::kTextFieldWithComboBox)),
            PageRole::kComboBox);
  EXPECT_EQ(PageRoleFor(Node(1, Role::kComboBoxSelect)), PageRole::kSelect);
  EXPECT_EQ(PageRoleFor(Node(1, Role::kToggleButton)), PageRole::kButton);
  EXPECT_EQ(PageRoleFor(Node(1, Role::kListBoxOption)), PageRole::kOption);
  EXPECT_EQ(PageRoleFor(Node(1, Role::kFooter)), PageRole::kContentInfo);
  EXPECT_EQ(PageRoleFor(Node(1, Role::kLayoutTable)), PageRole::kGeneric);
  EXPECT_EQ(PageRoleFor(area), PageRole::kTextArea);
  EXPECT_EQ(PageRoleFor(editable), PageRole::kTextField);

  const PageNode l = PageNodeFromData(link, 2, 1024);
  EXPECT_EQ(l.handle, (PageNodeHandle{2, 3}));
  EXPECT_EQ(l.role, PageRole::kLink);
  EXPECT_EQ(l.name, "Watch");
  EXPECT_EQ(l.url, "https://a.example/watch");
  EXPECT_TRUE(l.Has(kStateFocusable));
  EXPECT_TRUE(l.Has(kStateHasDefaultAction));

  const PageNode f = PageNodeFromData(field, 0, 1024);
  EXPECT_TRUE(f.Has(kStateProtected));
  EXPECT_TRUE(f.Has(kStateEditable));
  EXPECT_EQ(f.input_type, "password");
  EXPECT_EQ(f.autocomplete, "current-password");
  EXPECT_EQ(f.placeholder, "Your password");
  EXPECT_EQ(ClassifyCredential(f), CredentialKind::kPassword);

  const PageNode c = PageNodeFromData(check, 0, 1024);
  EXPECT_TRUE(c.Has(kStateChecked));
  EXPECT_TRUE(c.Has(kStateDisabled));
  EXPECT_FALSE(c.Has(kStateHasDefaultAction));

  EXPECT_EQ(PageNodeFromData(heading, 0, 1024).heading_level, 3);
  EXPECT_TRUE(PageNodeFromData(ignored, 0, 1024).Has(kStateIgnored));
  // A click that goes to an ancestor isn't the node's own action.
  EXPECT_FALSE(PageNodeFromData(clicky, 0, 1024).Has(kStateHasDefaultAction));
}

TEST(AXPageSourceTest, CutsStringsAtACharacterBoundary) {
  // "é" is two bytes: a cut at 3 bytes keeps "aé", at 2 keeps "a".
  const PageNode three =
      PageNodeFromData(Node(1, Role::kButton, "a\xC3\xA9z"), 0, 3);
  EXPECT_EQ(three.name, "a\xC3\xA9");
  const PageNode two =
      PageNodeFromData(Node(1, Role::kButton, "a\xC3\xA9z"), 0, 2);
  EXPECT_EQ(two.name, "a");
}

TEST(AXPageSourceTest, JoinsChildFramesUnderTheirHosts) {
  Frames frames;
  const ui::AXTreeID main_id = ui::AXTreeID::CreateNewAXTreeID();
  const ui::AXTreeID child_id = ui::AXTreeID::CreateNewAXTreeID();
  ui::AXNodeData iframe = Node(3, Role::kIframe, "Widget");
  iframe.AddChildTreeId(child_id);
  iframe.relative_bounds.bounds = gfx::RectF(100, 200, 300, 150);
  ui::AXNodeData after = Node(4, Role::kButton, "After frame");
  after.relative_bounds.bounds = gfx::RectF(10, 400, 80, 20);
  frames.Add(0, main_id,
             {Node(1, Role::kRootWebArea, "Host", {2, 3, 4}),
              Node(2, Role::kLink, "Home"), iframe, after},
             "Host page");
  ui::AXNodeData like = Node(2, Role::kButton, "Like");
  like.relative_bounds.bounds = gfx::RectF(5, 6, 40, 20);
  frames.Add(7, child_id, {Node(1, Role::kRootWebArea, "Widget", {2}), like});

  const PageTree page = frames.Build(main_id);
  EXPECT_EQ(page.title, "Host page");
  std::vector<std::pair<std::string, int32_t>> order;
  for (const PageNode& node : page.nodes) {
    order.emplace_back(node.name, node.handle.tree);
  }
  // Nodes are listed in walk order: the frame's after its host.
  EXPECT_EQ(order,
            (std::vector<std::pair<std::string, int32_t>>{{"Host", 0},
                                                          {"Home", 0},
                                                          {"Widget", 0},
                                                          {"Widget", 7},
                                                          {"Like", 7},
                                                          {"After frame", 0}}));
  // The frame's root is the host's child; its bounds are offset by the
  // host's position.
  ASSERT_EQ(page.nodes[2].children.size(), 1u);
  EXPECT_EQ(page.nodes[page.nodes[2].children[0]].handle,
            (PageNodeHandle{7, 1}));
  EXPECT_FLOAT_EQ(page.nodes[4].bounds.x, 105);
  EXPECT_FLOAT_EQ(page.nodes[4].bounds.y, 206);

  const PageMap map = BuildPageMap(page);
  ASSERT_EQ(map.items.size(), 3u);
  EXPECT_EQ(map.items[0].label, "Home");
  EXPECT_EQ(map.items[1].label, "Like");
  EXPECT_EQ(map.items[1].handle, (PageNodeHandle{7, 2}));
  EXPECT_EQ(map.items[2].label, "After frame");
}

TEST(AXPageSourceTest, AFrameIsJoinedOnceAndUnknownFramesAreSkipped) {
  Frames frames;
  const ui::AXTreeID main_id = ui::AXTreeID::CreateNewAXTreeID();
  const ui::AXTreeID child_id = ui::AXTreeID::CreateNewAXTreeID();
  ui::AXNodeData first = Node(2, Role::kIframe);
  first.AddChildTreeId(child_id);
  ui::AXNodeData second = Node(3, Role::kIframe);
  second.AddChildTreeId(child_id);  // the same frame claimed twice
  ui::AXNodeData unknown = Node(4, Role::kIframe);
  unknown.AddChildTreeId(ui::AXTreeID::CreateNewAXTreeID());
  ui::AXNodeData itself = Node(5, Role::kIframe);
  itself.AddChildTreeId(main_id);  // a cycle
  frames.Add(0, main_id,
             {Node(1, Role::kRootWebArea, "", {2, 3, 4, 5}), first, second,
              unknown, itself});
  ui::AXNodeData back = Node(2, Role::kIframe);
  back.AddChildTreeId(main_id);  // back up to the page
  frames.Add(1, child_id, {Node(1, Role::kRootWebArea, "Child", {2}), back});

  const PageTree page = frames.Build(main_id);
  int child_roots = 0;
  for (const PageNode& node : page.nodes) {
    child_roots += node.name == "Child" ? 1 : 0;
  }
  EXPECT_EQ(child_roots, 1);
  EXPECT_EQ(page.nodes.size(), 7u);  // 5 in the page, 2 in the frame
}

TEST(AXPageSourceTest, CapsNodesAndDepth) {
  Frames frames;
  const ui::AXTreeID main_id = ui::AXTreeID::CreateNewAXTreeID();
  std::vector<ui::AXNodeData> nodes;
  // A chain 400 deep, then 100 siblings at the bottom.
  for (int i = 1; i <= 400; ++i) {
    nodes.push_back(Node(
        i, i == 1 ? Role::kRootWebArea : Role::kGenericContainer, "", {i + 1}));
  }
  std::vector<int32_t> leaves;
  for (int i = 0; i < 100; ++i) {
    leaves.push_back(402 + i);
  }
  nodes.push_back(Node(401, Role::kGenericContainer, "", leaves));
  for (int32_t id : leaves) {
    nodes.push_back(Node(id, Role::kButton, "B"));
  }
  frames.Add(0, main_id, std::move(nodes));

  const PageTree deep = frames.Build(main_id);
  EXPECT_EQ(deep.nodes.size(), AXPageSourceLimits().max_depth + 1);

  AXPageSourceLimits limits;
  limits.max_depth = 1000;
  limits.max_nodes = 450;
  const PageTree capped = frames.Build(main_id, limits);
  EXPECT_EQ(capped.nodes.size(), 450u);
  // A prefix of the walk: the chain, then the first leaves.
  EXPECT_EQ(capped.nodes.back().handle.node, 450);
}

TEST(AXPageSourceTest, TakesTheFocusOfTheFocusedFrame) {
  Frames frames;
  const ui::AXTreeID main_id = ui::AXTreeID::CreateNewAXTreeID();
  const ui::AXTreeID child_id = ui::AXTreeID::CreateNewAXTreeID();
  ui::AXNodeData iframe = Node(2, Role::kIframe);
  iframe.AddChildTreeId(child_id);

  ui::AXTreeUpdate main;
  main.has_tree_data = true;
  main.tree_data.tree_id = main_id;
  main.tree_data.focused_tree_id = child_id;
  main.tree_data.focus_id = 1;
  main.root_id = 1;
  main.nodes = {Node(1, Role::kRootWebArea, "", {2}), iframe};
  ui::AXTree main_tree(main);

  ui::AXTreeUpdate child;
  child.has_tree_data = true;
  child.tree_data.tree_id = child_id;
  child.tree_data.focus_id = 2;
  child.root_id = 1;
  ui::AXNodeData field = Node(2, Role::kTextField, "Search");
  field.AddState(State::kFocusable);
  child.nodes = {Node(1, Role::kRootWebArea, "", {2}), field};
  ui::AXTree child_tree(child);

  const PageTree page =
      BuildPageTree(AXFrameTree{0, &main_tree}, [&](const ui::AXTreeID& id) {
        return id == child_id ? AXFrameTree{3, &child_tree} : AXFrameTree();
      });
  EXPECT_EQ(page.focus, (PageNodeHandle{3, 2}));
  const PageMap map = BuildPageMap(page);
  ASSERT_EQ(map.items.size(), 1u);
  EXPECT_TRUE(map.items[0].focused);
  EXPECT_EQ(map.focused_item, 1);
}

TEST(AXPageSourceTest, NoTreeNoPage) {
  ui::AXTree empty;
  const PageTree page =
      BuildPageTree(AXFrameTree{0, &empty},
                    [](const ui::AXTreeID&) { return AXFrameTree(); });
  EXPECT_TRUE(page.nodes.empty());
  const PageTree none = BuildPageTree(
      AXFrameTree(), [](const ui::AXTreeID&) { return AXFrameTree(); });
  EXPECT_TRUE(none.nodes.empty());
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
