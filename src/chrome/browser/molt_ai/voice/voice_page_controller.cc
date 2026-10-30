// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_page_controller.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/time/time.h"
#include "chrome/browser/molt_ai/security/activation_guard.h"
#include "chrome/browser/molt_ai/security/agent_safety.h"
#include "chrome/browser/molt_ai/voice/ax_page_source.h"
#include "chrome/browser/molt_ai/voice/page_map_text.h"
#include "chrome/browser/molt_ai/voice/site_skills.h"
#include "components/input/native_web_keyboard_event.h"
#include "content/public/browser/browser_accessibility_state.h"
#include "content/public/browser/media_session.h"
#include "content/public/browser/page.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/scoped_accessibility_mode.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "ui/accessibility/ax_action_data.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/accessibility/ax_location_and_scroll_updates.h"
#include "ui/accessibility/ax_node.h"
#include "ui/accessibility/ax_node_data.h"
#include "ui/accessibility/ax_tree.h"
#include "ui/accessibility/ax_tree_data.h"
#include "ui/accessibility/ax_tree_update.h"
#include "ui/accessibility/ax_updates_and_events.h"
#include "ui/events/keycodes/dom/dom_code.h"
#include "ui/events/keycodes/dom/dom_key.h"
#include "ui/events/keycodes/dom/keycode_converter.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/geometry/point.h"
#include "url/gurl.h"

// Why this accessibility mode (AccessibilityMode()), from Blink at 156
// (third_party/blink/renderer/modules/accessibility/ax_object.cc):
//   - kWebContents builds the tree: role, name, description, value,
//     states (focusable, editable, invisible, protected, expanded, required),
//     bounds, link targets and input types (AXObject::Serialize() and
//     SerializeUnignoredAttributes()).
//   - kExtendedProperties adds what the map needs on top: the checked state
//     (SerializeScreenReaderAttributes()), placeholders
//     (SerializeNameAndDescriptionAttributes()), heading levels and list
//     positions (SerializeUnignoredAttributes()), HTML tags
//     (SerializeHTMLTagAndClass()), autocomplete and the default action
//     (SerializeOtherScreenReaderAttributes()).
// Left out:
//   - kNativeAPIs: no BrowserAccessibilityManager or platform objects are
//     made (RenderFrameHostImpl::GetOrCreateBrowserAccessibilityManager()),
//     so voice mode costs no browser-side platform tree and doesn't present
//     the page to OS assistive technology as if one were running.
//   - kInlineTextBoxes: a node per line of text, for caret navigation; much
//     of the tree's size and its serialization cost.
//   - kHTML: every HTML attribute copied into the tree.
//   - kScreenReader: keeps content-visibility-hidden subtrees (unlaid out)
//     in the tree (AXObjectCacheImpl's DetermineAXObjectType()); without it
//     they are pruned, as the map wants (off screen, not rendered).
//   - kLabelImages, kHTMLMetadata, kPDFPrinting, kAnnotateMainNode.

namespace molt_ai {
namespace voice {

namespace {

// Whether every frame above |frame|, up to the top-level document (through
// a fenced frame's outer document), has |frame|'s origin.
bool AncestorsHaveOrigin(const content::RenderFrameHost& frame) {
  const url::Origin& origin = frame.GetLastCommittedOrigin();
  for (const content::RenderFrameHost* parent =
           frame.GetParentOrOuterDocument();
       parent; parent = parent->GetParentOrOuterDocument()) {
    if (!parent->GetLastCommittedOrigin().IsSameOriginWith(origin)) {
      return false;
    }
  }
  return true;
}

// How long Submit() waits for the field to report the focus.
constexpr base::TimeDelta kSubmitFocusTimeout = base::Seconds(3);
// How far Submit() looks for the form and its submit input.
constexpr int kMaxFormAncestors = 64;
constexpr size_t kMaxFormNodes = 5000;

bool IsTextFieldRole(PageRole role) {
  return role == PageRole::kTextField || role == PageRole::kSearchBox ||
         role == PageRole::kTextArea || role == PageRole::kComboBox;
}

std::string LowerAscii(const std::string& s) {
  std::string out = s.substr(0, 16);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

// The submit input (<input type=submit> or type=image) of |field|'s form,
// shown and enabled; null if the field is in no form or the form has none.
// A <button> doesn't say its type in the tree, so it is never taken for
// one: Enter submits through the form's own default button instead.
const ui::AXNode* FindSubmitInput(const ui::AXNode& field) {
  const ui::AXNode* form = field.parent();
  for (int i = 0; form && i < kMaxFormAncestors; ++i, form = form->parent()) {
    if (form->GetRole() == ax::mojom::Role::kForm) {
      break;
    }
  }
  if (!form || form->GetRole() != ax::mojom::Role::kForm) {
    return nullptr;
  }
  std::vector<const ui::AXNode*> stack = {form};
  size_t visited = 0;
  while (!stack.empty() && visited++ < kMaxFormNodes) {
    const ui::AXNode* node = stack.back();
    stack.pop_back();
    const ui::AXNodeData& data = node->data();
    const std::string type = LowerAscii(
        data.GetStringAttribute(ax::mojom::StringAttribute::kInputType));
    if ((type == "submit" || type == "image") &&
        !data.HasState(ax::mojom::State::kInvisible) &&
        data.GetRestriction() != ax::mojom::Restriction::kDisabled) {
      return node;
    }
    const auto& children = node->children();
    for (size_t k = children.size(); k-- > 0;) {
      stack.push_back(children[k].get());
    }
  }
  return nullptr;
}

}  // namespace

WEB_CONTENTS_USER_DATA_KEY_IMPL(VoicePageController);

VoicePageController::HeldTree::HeldTree() = default;
VoicePageController::HeldTree::HeldTree(HeldTree&&) = default;
VoicePageController::HeldTree& VoicePageController::HeldTree::operator=(
    HeldTree&&) = default;
VoicePageController::HeldTree::~HeldTree() = default;

VoicePageController::VoicePageController(content::WebContents* web_contents)
    : content::WebContentsObserver(web_contents),
      content::WebContentsUserData<VoicePageController>(*web_contents) {}

VoicePageController::~VoicePageController() = default;

// static
ui::AXMode VoicePageController::AccessibilityMode() {
  return ui::AXMode(ui::AXMode::kWebContents | ui::AXMode::kExtendedProperties);
}

// static
std::string_view VoicePageController::ActionResultName(ActionResult result) {
  switch (result) {
    case ActionResult::kDone:
      return "done";
    case ActionResult::kNotActive:
      return "not active";
    case ActionResult::kStale:
      return "stale";
    case ActionResult::kNotAllowed:
      return "not allowed";
    case ActionResult::kDisabled:
      return "disabled";
    case ActionResult::kNotEditable:
      return "not editable";
    case ActionResult::kCredentialField:
      return "credential field";
    case ActionResult::kTooLong:
      return "too long";
    case ActionResult::kNotMedia:
      return "not media";
    case ActionResult::kNoMediaSession:
      return "no media session";
    case ActionResult::kFocusNotConfirmed:
      return "focus not confirmed";
    case ActionResult::kCancelled:
      return "cancelled";
    case ActionResult::kNoSuchOption:
      return "no such option";
  }
  return "unknown";
}

bool VoicePageController::held_by(Holder holder) const {
  switch (holder) {
    case Holder::kVoiceMode:
      return voice_mode_hold_;
    case Holder::kWebAgent:
      return web_agent_holds_ > 0;
  }
  return false;
}

void VoicePageController::Start(Holder holder) {
  if (!web_contents()) {
    return;
  }
  switch (holder) {
    case Holder::kVoiceMode:
      voice_mode_hold_ = true;
      break;
    case Holder::kWebAgent:
      ++web_agent_holds_;
      break;
  }
  if (active()) {
    return;  // another holder took the mode already
  }
  // If another client already asked for the trees, the renderers are
  // mid-stream: ask them for whole trees again, as Reading Mode does.
  const bool had_trees =
      web_contents()->GetAccessibilityMode().has_mode(ui::AXMode::kWebContents);
  mode_ =
      content::BrowserAccessibilityState::GetInstance()
          ->CreateScopedModeForWebContents(web_contents(), AccessibilityMode());
  resets_this_page_ = 0;
  if (had_trees) {
    web_contents()->ResetAccessibility();
  }
}

void VoicePageController::Stop(Holder holder) {
  switch (holder) {
    case Holder::kVoiceMode:
      voice_mode_hold_ = false;
      // Submit() is voice mode's: its wait ends with voice mode's hold.
      FinishSubmit(ActionResult::kCancelled);
      break;
    case Holder::kWebAgent:
      if (web_agent_holds_ > 0) {
        --web_agent_holds_;
      }
      break;
  }
  if (voice_mode_hold_ || web_agent_holds_ > 0) {
    return;  // another holder keeps the mode
  }
  FinishSubmit(ActionResult::kCancelled);
  mode_.reset();
  trees_.clear();
  tree_ids_.clear();
}

base::CallbackListSubscription VoicePageController::AddTreesUpdatedCallback(
    base::RepeatingClosure callback) {
  return trees_updated_.Add(std::move(callback));
}

void VoicePageController::SetTreeCapsForTesting(size_t max_trees,
                                                size_t max_frame_tree_nodes) {
  max_trees_ = max_trees;
  max_frame_tree_nodes_ = max_frame_tree_nodes;
}

void VoicePageController::SetTreesUpdatedCallbackForTesting(
    base::RepeatingClosure callback) {
  trees_updated_for_testing_ = std::move(callback);
}

content::RenderFrameHost* VoicePageController::FrameFor(
    const ui::AXTreeID& tree_id) {
  if (tree_id == ui::AXTreeIDUnknown()) {
    return nullptr;
  }
  content::RenderFrameHost* frame =
      content::RenderFrameHost::FromAXTreeID(tree_id);
  if (!frame ||
      content::WebContents::FromRenderFrameHost(frame) != web_contents() ||
      !frame->IsActive() || !frame->GetPage().IsPrimary()) {
    return nullptr;
  }
  return frame;
}

size_t VoicePageController::FrameTreeNodes() {
  const ui::AXTreeID main_id =
      web_contents()->GetPrimaryMainFrame()->GetAXTreeID();
  size_t nodes = 0;
  for (const auto& [tree_id, held] : trees_) {
    if (tree_id != main_id) {
      nodes += static_cast<size_t>(held.tree->size());
    }
  }
  return nodes;
}

void VoicePageController::DropTree(const ui::AXTreeID& tree_id) {
  auto it = trees_.find(tree_id);
  if (it == trees_.end()) {
    return;
  }
  const int32_t index = it->second.index;
  tree_ids_.erase(index);
  trees_.erase(it);
  if (pending_submit_ && pending_submit_->field.tree == index) {
    FinishSubmit(ActionResult::kStale);
  }
}

void VoicePageController::DropTreesNotInPrimaryPage() {
  std::vector<ui::AXTreeID> gone;
  for (const auto& [tree_id, held] : trees_) {
    if (!FrameFor(tree_id)) {
      gone.push_back(tree_id);
    }
  }
  for (const ui::AXTreeID& tree_id : gone) {
    DropTree(tree_id);
  }
}

void VoicePageController::AccessibilityEventReceived(
    const ui::AXUpdatesAndEvents& details) {
  if (!active()) {
    return;
  }
  const ui::AXTreeID& tree_id = details.ax_tree_id;
  content::RenderFrameHost* frame = FrameFor(tree_id);
  if (!frame) {
    return;  // another page (a cached or prerendered one) or a gone frame
  }
  // The top-level document's tree is always kept: the frames it embeds
  // can't take its place.
  const bool main_frame = frame->IsInPrimaryMainFrame();
  auto it = trees_.find(tree_id);
  if (it == trees_.end()) {
    if (trees_.size() >= max_trees_ && !main_frame) {
      ++stats_.trees_over_cap;
      return;
    }
    HeldTree held;
    held.index = next_tree_index_++;
    held.tree = std::make_unique<ui::AXTree>();
    it = trees_.emplace(tree_id, std::move(held)).first;
    tree_ids_[it->second.index] = tree_id;
    ++stats_.trees;
  }

  bool changed = false;
  bool need_reset = false;
  for (const ui::AXTreeUpdate& update : details.updates) {
    ui::AXTree& tree = *it->second.tree;
    // The renderer validated the node ids (RenderFrameHostImpl::
    // HandleAXEvents()). What a tree can't take here is skipped before
    // Unserialize() sees it: in builds that fail fast on accessibility
    // errors (DCHECK or not official, ax_common.h) it would crash the
    // browser. An empty tree needs a whole tree (its root first); tree data
    // alone waits for it (the browser sends some when the focus moves, as
    // when this tab becomes active again); tree data must name this tree.
    if (!TreeCanTakeUpdate(tree, update)) {
      if (!update.nodes.empty()) {
        ++stats_.dropped_updates;
        need_reset = true;  // ask for a whole tree
      }
      continue;
    }
    if (update.has_tree_data && update.tree_data.tree_id != tree_id) {
      ++stats_.dropped_updates;
      continue;
    }
    if (!tree.Unserialize(update) ||
        static_cast<size_t>(tree.size()) > kMaxTreeNodes) {
      ++stats_.unserialize_failures;
      DropTree(tree_id);
      need_reset = true;
      changed = false;
      break;
    }
    // The frames' trees together are capped too, so a page that embeds
    // many large frames can't make the browser hold more.
    if (!main_frame && FrameTreeNodes() > max_frame_tree_nodes_) {
      ++stats_.trees_over_cap;
      DropTree(tree_id);
      changed = false;
      break;
    }
    changed = true;
  }
  if (need_reset && resets_this_page_ < kMaxResetsPerPage) {
    ++resets_this_page_;
    ++stats_.resets;
    web_contents()->ResetAccessibility();
  }
  if (changed) {
    MaybeSendPendingEnter();
    if (trees_updated_for_testing_) {
      trees_updated_for_testing_.Run();
    }
    trees_updated_.Notify();
  }
}

// Blink sends where nodes moved, and scroll offsets, apart from the tree
// updates (AXObjectCacheImpl::SerializeLocationChanges()); without them a
// node's bounds would stay where the page was before it scrolled. Applied
// as BrowserAccessibilityManager::OnLocationChanges() does; node ids the
// tree doesn't have are skipped (content checked the ids are valid ones).
void VoicePageController::AccessibilityLocationChangesReceived(
    const ui::AXTreeID& tree_id,
    ui::AXLocationAndScrollUpdates& details) {
  if (!active() || !FrameFor(tree_id)) {
    return;
  }
  auto it = trees_.find(tree_id);
  if (it == trees_.end() || !it->second.tree->root()) {
    return;
  }
  ui::AXTree& tree = *it->second.tree;
  bool changed = false;
  for (const ui::AXScrollChange& change : details.scroll_changes) {
    if (ui::AXNode* node = tree.GetFromId(change.id)) {
      node->SetScrollInfo(change.scroll_x, change.scroll_y);
      changed = true;
    }
  }
  for (const ui::AXLocationChange& change : details.location_changes) {
    if (ui::AXNode* node = tree.GetFromId(change.id)) {
      node->SetLocation(change.new_location.offset_container_id,
                        change.new_location.bounds,
                        change.new_location.transform.get());
      changed = true;
    }
  }
  if (changed) {
    trees_updated_.Notify();
  }
}

void VoicePageController::RenderFrameDeleted(
    content::RenderFrameHost* render_frame_host) {
  DropTree(render_frame_host->GetAXTreeID());
}

void VoicePageController::PrimaryPageChanged(content::Page& page) {
  DropTreesNotInPrimaryPage();
  resets_this_page_ = 0;
  skill_missing_logged_ = 0;
}

void VoicePageController::WebContentsDestroyed() {
  voice_mode_hold_ = false;
  web_agent_holds_ = 0;
  Stop(Holder::kVoiceMode);
}

bool VoicePageController::HasPageTree() {
  if (!active()) {
    return false;
  }
  DropTreesNotInPrimaryPage();
  auto it = trees_.find(web_contents()->GetPrimaryMainFrame()->GetAXTreeID());
  return it != trees_.end() && it->second.tree->root();
}

PageTree VoicePageController::JoinedTree(size_t max_nodes,
                                         size_t max_depth,
                                         size_t max_string_bytes) {
  if (!HasPageTree()) {
    return PageTree();
  }
  const HeldTree& main =
      trees_.find(web_contents()->GetPrimaryMainFrame()->GetAXTreeID())->second;
  AXPageSourceLimits source_limits;
  source_limits.max_nodes = max_nodes;
  source_limits.max_depth = max_depth;
  source_limits.max_trees = kMaxTrees;
  source_limits.max_string_bytes = max_string_bytes;
  // Only this tab's primary page's frames are here (the line above dropped
  // the rest), so a child tree id can only join a frame of this page.
  return BuildPageTree(
      AXFrameTree{main.index, main.tree.get()},
      [this](const ui::AXTreeID& tree_id) {
        auto it = trees_.find(tree_id);
        return it == trees_.end()
                   ? AXFrameTree()
                   : AXFrameTree{it->second.index, it->second.tree.get()};
      },
      source_limits);
}

PageTree VoicePageController::BuildTree(size_t max_string_bytes) {
  const PageMapLimits limits;
  return JoinedTree(limits.max_nodes, limits.max_depth,
                    std::min(max_string_bytes, kMaxReadingStringBytes));
}

PageMap VoicePageController::BuildMap(const PageMapLimits& limits) {
  const PageTree page =
      JoinedTree(limits.max_nodes, limits.max_depth,
                 AXPageSourceLimits().max_string_bytes);
  if (page.nodes.empty()) {
    return PageMap();
  }
  // The site's skill (site_skills.h), on its http(s) pages only.
  const GURL& url = web_contents()->GetPrimaryMainFrame()->GetLastCommittedURL();
  const SiteSkill* skill =
      url.SchemeIsHTTPOrHTTPS() ? BuiltInSkillForHost(url.host()) : nullptr;
  if (!skill) {
    return BuildPageMap(page, limits);
  }
  SkillReport report;
  PageMap map = BuildPageMapWithSkills(page, limits, skill, /*hint=*/nullptr,
                                       url.path(), &report);
  // A skill that no longer finds what it expects here (the site changed):
  // the generic map stands in, and the log says so, once for this page.
  const uint32_t unlogged = report.missing & ~skill_missing_logged_;
  if (unlogged) {
    skill_missing_logged_ |= unlogged;
    std::string parts;
    for (uint32_t bit = kPartSearch; bit <= kPartContent; bit <<= 1) {
      if (unlogged & bit) {
        parts += std::string(parts.empty() ? "" : ", ") +
                 std::string(SkillPartName(static_cast<SkillPartBit>(bit)));
      }
    }
    LOG(WARNING) << "[MoltVoice] site skill " << report.skill
                 << " found no " << parts << " on " << url.host()
                 << url.path() << "; the generic page map stands in";
  }
  return map;
}

std::optional<VoicePageController::Target> VoicePageController::Resolve(
    const PageNodeHandle& handle,
    ActionResult* error) {
  if (!active()) {
    *error = ActionResult::kNotActive;
    return std::nullopt;
  }
  *error = ActionResult::kStale;
  auto id = tree_ids_.find(handle.tree);
  if (id == tree_ids_.end()) {
    return std::nullopt;
  }
  auto held = trees_.find(id->second);
  if (held == trees_.end()) {
    return std::nullopt;
  }
  content::RenderFrameHost* frame = FrameFor(id->second);
  if (!frame) {
    return std::nullopt;
  }
  const ui::AXNode* node = held->second.tree->GetFromId(handle.node);
  if (!node) {
    return std::nullopt;
  }
  // Only http(s) documents, as the agents act on (agent_safety.h).
  if (!agent_safety::IsAllowedNavigationURL(frame->GetLastCommittedURL())) {
    *error = ActionResult::kNotAllowed;
    return std::nullopt;
  }
  Target target;
  target.frame = frame;
  target.tree = held->second.tree.get();
  target.node = node;
  target.tree_id = id->second;
  target.tree_index = handle.tree;
  *error = ActionResult::kDone;
  return target;
}

VoicePageController::ActionResult VoicePageController::Perform(
    const Target& target,
    ui::AXActionData data,
    activation::Intent intent) {
  data.target_tree_id = target.tree_id;
  data.target_node_id = target.node->id();
  // Before the action reaches the page: what its activation unlocks is the
  // guard's to decide from now (activation_policy.h). Every action is
  // noted, also those Blink gives no activation for at 156 (set value,
  // scrolls), so a change there is still covered.
  ActivationGuard::NoteAction(web_contents(), intent);
  target.frame->AccessibilityPerformAction(data);
  return ActionResult::kDone;
}

VoicePageController::TargetFacts::TargetFacts() = default;
VoicePageController::TargetFacts::~TargetFacts() = default;
VoicePageController::TargetFacts::TargetFacts(const TargetFacts&) = default;
VoicePageController::TargetFacts& VoicePageController::TargetFacts::operator=(
    const TargetFacts&) = default;
VoicePageController::TargetFacts::TargetFacts(TargetFacts&&) = default;
VoicePageController::TargetFacts& VoicePageController::TargetFacts::operator=(
    TargetFacts&&) = default;

std::optional<VoicePageController::TargetFacts> VoicePageController::Inspect(
    const PageNodeHandle& target,
    ActionResult* error) {
  std::optional<Target> found = Resolve(target, error);
  if (!found) {
    return std::nullopt;
  }
  TargetFacts facts;
  const size_t max_string_bytes = AXPageSourceLimits().max_string_bytes;
  const ui::AXNode* node = found->node;
  for (size_t depth = 0; node && depth <= kMaxInspectAncestors;
       ++depth, node = node->parent()) {
    facts.chain.push_back(
        PageNodeFromData(node->data(), target.tree, max_string_bytes));
    if (facts.form_index < 0 && depth > 0 &&
        node->GetRole() == ax::mojom::Role::kForm) {
      facts.form_index = static_cast<int>(depth);
    }
  }
  facts.origin = found->frame->GetLastCommittedOrigin();
  facts.main_frame = found->frame->IsInPrimaryMainFrame();
  facts.framed_by_own_origin = AncestorsHaveOrigin(*found->frame);
  return facts;
}

std::optional<url::Origin> VoicePageController::OriginOfTree(
    int32_t tree_index) {
  auto id = tree_ids_.find(tree_index);
  if (id == tree_ids_.end()) {
    return std::nullopt;
  }
  content::RenderFrameHost* frame = FrameFor(id->second);
  if (!frame) {
    return std::nullopt;
  }
  return frame->GetLastCommittedOrigin();
}

bool VoicePageController::FramedByItsOwnOrigin(int32_t tree_index) {
  auto id = tree_ids_.find(tree_index);
  if (id == tree_ids_.end()) {
    return false;
  }
  content::RenderFrameHost* frame = FrameFor(id->second);
  return frame && AncestorsHaveOrigin(*frame);
}

namespace {

// The enabled options under |select|, in tree order: at most |max| of them,
// in a walk of at most kMaxFormNodes nodes.
std::vector<const ui::AXNode*> OptionNodes(const ui::AXNode& select,
                                           size_t max) {
  std::vector<const ui::AXNode*> options;
  std::vector<const ui::AXNode*> stack;
  const auto& top = select.children();
  for (size_t k = top.size(); k-- > 0;) {
    stack.push_back(top[k].get());
  }
  size_t visited = 0;
  while (!stack.empty() && visited++ < kMaxFormNodes && options.size() < max) {
    const ui::AXNode* node = stack.back();
    stack.pop_back();
    if (PageRoleFor(node->data()) == PageRole::kOption) {
      if (node->data().GetRestriction() != ax::mojom::Restriction::kDisabled) {
        options.push_back(node);
      }
      continue;  // an option's own children are its text
    }
    const auto& children = node->children();
    for (size_t k = children.size(); k-- > 0;) {
      stack.push_back(children[k].get());
    }
  }
  return options;
}

bool IsSelectRole(PageRole role) {
  return role == PageRole::kSelect || role == PageRole::kListBox;
}

}  // namespace

std::vector<std::string> VoicePageController::OptionsOf(
    const PageNodeHandle& select) {
  ActionResult error;
  std::optional<Target> found = Resolve(select, &error);
  std::vector<std::string> names;
  if (!found || !IsSelectRole(PageRoleFor(found->node->data()))) {
    return names;
  }
  for (const ui::AXNode* option : OptionNodes(*found->node, kMaxOptions)) {
    std::string name = SpeechSafeText(
        option->data().GetStringAttribute(ax::mojom::StringAttribute::kName),
        kMaxOptionChars);
    if (!name.empty()) {
      names.push_back(std::move(name));
    }
  }
  return names;
}

VoicePageController::ActionResult VoicePageController::SelectOption(
    const PageNodeHandle& select,
    std::string_view option) {
  ActionResult error;
  std::optional<Target> found = Resolve(select, &error);
  if (!found) {
    return error;
  }
  if (found->node->data().GetRestriction() ==
      ax::mojom::Restriction::kDisabled) {
    return ActionResult::kDisabled;
  }
  if (!IsSelectRole(PageRoleFor(found->node->data()))) {
    return ActionResult::kNotEditable;
  }
  // The same options, named the same way, as OptionsOf() lists.
  for (const ui::AXNode* node : OptionNodes(*found->node, kMaxOptions)) {
    const std::string name = SpeechSafeText(
        node->data().GetStringAttribute(ax::mojom::StringAttribute::kName),
        kMaxOptionChars);
    if (name.empty() || name != option) {
      continue;
    }
    Target target = *found;
    target.node = node;
    ui::AXActionData data;
    data.action = ax::mojom::Action::kDoDefault;
    return Perform(target, data);
  }
  return ActionResult::kNoSuchOption;
}

VoicePageController::ActionResult VoicePageController::Press(
    const PageNodeHandle& target,
    activation::Intent intent) {
  ActionResult error;
  std::optional<Target> found = Resolve(target, &error);
  if (!found) {
    return error;
  }
  if (found->node->data().GetRestriction() ==
      ax::mojom::Restriction::kDisabled) {
    return ActionResult::kDisabled;
  }
  // Blink's AXObject::OnNativeClickAction(): user activation, then the
  // element's click (pointer and mouse events and click), as for OS
  // assistive technology.
  ui::AXActionData data;
  data.action = ax::mojom::Action::kDoDefault;
  return Perform(*found, data, intent);
}

VoicePageController::ActionResult VoicePageController::Focus(
    const PageNodeHandle& target) {
  ActionResult error;
  std::optional<Target> found = Resolve(target, &error);
  if (!found) {
    return error;
  }
  ui::AXActionData data;
  data.action = ax::mojom::Action::kFocus;
  return Perform(*found, data);
}

VoicePageController::ActionResult VoicePageController::ScrollIntoView(
    const PageNodeHandle& target) {
  ActionResult error;
  std::optional<Target> found = Resolve(target, &error);
  if (!found) {
    return error;
  }
  ui::AXActionData data;
  data.action = ax::mojom::Action::kScrollToMakeVisible;
  data.horizontal_scroll_alignment =
      ax::mojom::ScrollAlignment::kScrollAlignmentCenter;
  data.vertical_scroll_alignment =
      ax::mojom::ScrollAlignment::kScrollAlignmentCenter;
  data.scroll_behavior = ax::mojom::ScrollBehavior::kDoNotScrollIfVisible;
  return Perform(*found, data);
}

VoicePageController::ActionResult VoicePageController::SetText(
    const PageNodeHandle& field,
    std::string_view text) {
  ActionResult error;
  std::optional<Target> found = Resolve(field, &error);
  if (!found) {
    return error;
  }
  // The field as it is now, not as the map saw it.
  const PageNode node = PageNodeFromData(found->node->data(), field.tree,
                                         AXPageSourceLimits().max_string_bytes);
  if (node.Has(kStateProtected) ||
      ClassifyCredential(node) != CredentialKind::kNone) {
    return ActionResult::kCredentialField;
  }
  if (!IsTextFieldRole(node.role) || !node.Has(kStateEditable) ||
      node.Has(kStateReadOnly) || node.Has(kStateDisabled)) {
    return ActionResult::kNotEditable;
  }
  std::optional<std::string> typed =
      TypedText(text, node.role == PageRole::kTextArea, kMaxSetTextBytes);
  if (!typed) {
    return ActionResult::kTooLong;
  }
  // Blink's AXNodeObject::OnNativeSetValueAction(): the value, then the
  // input and change events a page listens to.
  ui::AXActionData data;
  data.action = ax::mojom::Action::kSetValue;
  data.value = std::move(*typed);
  return Perform(*found, data);
}

void VoicePageController::Submit(const PageNodeHandle& field,
                                 SubmitCallback done) {
  FinishSubmit(ActionResult::kCancelled);  // one at a time
  ActionResult error;
  std::optional<Target> found = Resolve(field, &error);
  if (!found) {
    std::move(done).Run(error);
    return;
  }
  const PageRole role = PageRoleFor(found->node->data());
  if (role == PageRole::kButton) {
    std::move(done).Run(Press(field));
    return;
  }
  if (!IsTextFieldRole(role)) {
    std::move(done).Run(ActionResult::kNotEditable);
    return;
  }
  if (const ui::AXNode* submit = FindSubmitInput(*found->node)) {
    std::move(done).Run(Press(PageNodeHandle{field.tree, submit->id()}));
    return;
  }
  // Enter, as the keyboard sends it, to the focused field. The key events
  // travel to the renderer apart from accessibility actions, so the field
  // must report the focus first.
  pending_submit_ = PendingSubmit{field, std::move(done)};
  if (found->tree->data().focus_id != found->node->id()) {
    const ActionResult focus = Focus(field);
    if (focus != ActionResult::kDone) {
      FinishSubmit(focus);
      return;
    }
    submit_timer_.Start(FROM_HERE, kSubmitFocusTimeout,
                        base::BindOnce(&VoicePageController::OnSubmitTimeout,
                                       weak_factory_.GetWeakPtr()));
    return;
  }
  MaybeSendPendingEnter();
}

void VoicePageController::MaybeSendPendingEnter() {
  if (!pending_submit_) {
    return;
  }
  ActionResult error;
  std::optional<Target> found = Resolve(pending_submit_->field, &error);
  if (!found) {
    FinishSubmit(error);
    return;
  }
  if (found->tree->data().focus_id != found->node->id()) {
    return;  // not yet
  }
  SendEnter(found->frame);
  FinishSubmit(ActionResult::kDone);
}

void VoicePageController::OnSubmitTimeout() {
  FinishSubmit(ActionResult::kFocusNotConfirmed);
}

void VoicePageController::FinishSubmit(ActionResult result) {
  submit_timer_.Stop();
  if (!pending_submit_) {
    return;
  }
  SubmitCallback done = std::move(pending_submit_->done);
  pending_submit_.reset();
  std::move(done).Run(result);
}

void VoicePageController::SendEnter(content::RenderFrameHost* frame) {
  content::RenderWidgetHost* widget = frame->GetRenderWidgetHost();
  if (!widget) {
    return;
  }
  // A key press gives the page user activation as an action does. It comes
  // back as the tab's input, which is the browser's here, not the user's.
  ActivationGuard::NoteAction(web_contents(), activation::Intent::kNone);
  if (ActivationGuard* guard =
          ActivationGuard::FromWebContents(web_contents())) {
    guard->ExpectOwnKeyDown(ui::VKEY_RETURN);
  }
  for (blink::WebInputEvent::Type type :
       {blink::WebInputEvent::Type::kRawKeyDown,
        blink::WebInputEvent::Type::kChar,
        blink::WebInputEvent::Type::kKeyUp}) {
    input::NativeWebKeyboardEvent event(
        type, blink::WebInputEvent::kNoModifiers, base::TimeTicks::Now());
    event.dom_key = ui::DomKey::ENTER;
    event.dom_code = static_cast<int>(ui::DomCode::ENTER);
    event.native_key_code =
        ui::KeycodeConverter::DomCodeToNativeKeycode(ui::DomCode::ENTER);
    event.windows_key_code = ui::VKEY_RETURN;
    event.is_system_key = false;
    if (type != blink::WebInputEvent::Type::kKeyUp) {
      event.text[0] = ui::VKEY_RETURN;
      event.unmodified_text[0] = ui::VKEY_RETURN;
    }
    widget->ForwardKeyboardEvent(event);
  }
}

VoicePageController::ActionResult VoicePageController::PlayMedia(
    const PageNodeHandle& media) {
  ActionResult error;
  std::optional<Target> found = Resolve(media, &error);
  if (!found) {
    return error;
  }
  const PageRole role = PageRoleFor(found->node->data());
  if (role != PageRole::kVideo && role != PageRole::kAudio) {
    return ActionResult::kNotMedia;
  }
  ui::AXActionData data;
  data.action = ax::mojom::Action::kDoDefault;
  // The user asked for it to play: the player may go full screen.
  return Perform(*found, data, activation::Intent::kPlayMedia);
}

VoicePageController::ActionResult VoicePageController::PauseMedia() {
  if (!active()) {
    return ActionResult::kNotActive;
  }
  content::MediaSession* session =
      content::MediaSession::GetIfExists(web_contents());
  if (!session) {
    return ActionResult::kNoMediaSession;
  }
  session->Suspend(content::MediaSession::SuspendType::kUI);
  return ActionResult::kDone;
}

VoicePageController::ActionResult VoicePageController::ResumeMedia() {
  if (!active()) {
    return ActionResult::kNotActive;
  }
  content::MediaSession* session =
      content::MediaSession::GetIfExists(web_contents());
  if (!session) {
    return ActionResult::kNoMediaSession;
  }
  // MediaSessionImpl::Resume(kUI) asks each player to play as triggered
  // by the user (HTMLMediaElement::RequestPlay()): with sound, without the
  // autoplay check. Players join the session once they have played.
  session->Resume(content::MediaSession::SuspendType::kUI);
  return ActionResult::kDone;
}

VoicePageController::ActionResult VoicePageController::Increment(
    const PageNodeHandle& target) {
  return Step(target, ax::mojom::Action::kIncrement);
}

VoicePageController::ActionResult VoicePageController::Decrement(
    const PageNodeHandle& target) {
  return Step(target, ax::mojom::Action::kDecrement);
}

VoicePageController::ActionResult VoicePageController::Step(
    const PageNodeHandle& target,
    ax::mojom::Action action) {
  ActionResult error;
  std::optional<Target> found = Resolve(target, &error);
  if (!found) {
    return error;
  }
  if (found->node->data().GetRestriction() ==
      ax::mojom::Restriction::kDisabled) {
    return ActionResult::kDisabled;
  }
  const PageRole role = PageRoleFor(found->node->data());
  if (role != PageRole::kSlider) {
    return ActionResult::kNotEditable;
  }
  ui::AXActionData data;
  data.action = action;
  return Perform(*found, data);
}

VoicePageController::ActionResult VoicePageController::ScrollPage(
    PageScrollAction action) {
  if (!HasPageTree()) {
    return active() ? ActionResult::kStale : ActionResult::kNotActive;
  }
  const HeldTree& main =
      trees_.find(web_contents()->GetPrimaryMainFrame()->GetAXTreeID())->second;
  ActionResult error;
  std::optional<Target> found =
      Resolve(PageNodeHandle{main.index, main.tree->root()->id()}, &error);
  if (!found) {
    return error;
  }
  ui::AXActionData data;
  // Far enough past any page's end; Blink clamps it to the scroller's.
  constexpr int kFarEnd = 100000000;
  switch (action) {
    case PageScrollAction::kDown:
      data.action = ax::mojom::Action::kScrollDown;
      break;
    case PageScrollAction::kUp:
      data.action = ax::mojom::Action::kScrollUp;
      break;
    case PageScrollAction::kLeft:
      data.action = ax::mojom::Action::kScrollLeft;
      break;
    case PageScrollAction::kRight:
      data.action = ax::mojom::Action::kScrollRight;
      break;
    case PageScrollAction::kTop:
      data.action = ax::mojom::Action::kSetScrollOffset;
      data.target_point = gfx::Point(0, 0);
      break;
    case PageScrollAction::kBottom:
      data.action = ax::mojom::Action::kSetScrollOffset;
      data.target_point = gfx::Point(0, kFarEnd);
      break;
  }
  return Perform(*found, data);
}

}  // namespace voice
}  // namespace molt_ai
