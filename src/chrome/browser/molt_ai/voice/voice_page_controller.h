// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Voice mode's hold on one tab's page (docs/design/TOUCHLESS_BROWSING.md
// §6.3, §6.5): it keeps the tab's accessibility trees, builds the page map
// from them on request, and acts on the page as assistive technology does,
// through accessibility actions.
//
//   - Mode: while voice mode is on for the tab, it holds a scoped
//     accessibility mode on the tab only (AccessibilityMode()). The
//     renderers then send their trees and every change to them.
//   - Trees: it keeps one ui::AXTree per frame of the tab's primary page,
//     from AccessibilityEventReceived(), in the browser process as platform
//     accessibility does. The updates come from renderers and are
//     untrusted: an update a tree can't take drops that tree (counted), and
//     a few resets per page ask the renderers for their trees again. The
//     top-level document's tree is always kept; the child frames' trees are
//     capped in number and in nodes together.
//   - Map: BuildMap() joins the frames' trees (ax_page_source.h) and maps
//     them (page_map.h). A PageNodeHandle names a frame by an index this
//     controller gives each tree once per voice session, never reused, and
//     a node by its id in that tree.
//   - Actions: press (the default action), focus, scroll into view, set
//     text and submit, and media play and pause. Every action finds its
//     target again: the frame must still be this tab's, in its primary
//     page, an http(s) document, and the node must still be in the tree the
//     frame has now; a stale handle is refused (map again). Text is never
//     written into a protected or credential field or a field that isn't
//     editable.
//   - Activation: Blink takes these actions (and Submit's Enter key) as the
//     user's own interaction, so the page gets user activation from them.
//     Every action is noted with the tab's ActivationGuard
//     (molt_ai/security/activation_guard.h), which keeps what the page
//     does with that activation to what the user asked for, or asks first.
//   - Holders: voice mode and the web agent (agents/web_agent.h) each hold
//     the controller (Start(Holder)); the mode stays while either does, so
//     an agent run on a tab in voice mode leaves the page as voice mode
//     had it, and a run on another tab releases its hold when it ends.
//     Stop() of the last holder releases the mode and drops the trees.
//
// The page map and these actions are what voice mode and the web agent act
// through: the numbers overlay draws PageItem::bounds, voice mode's steps
// and the agent's actions (docs/design/AGENT_ACTIONS.md §2.10) name
// PageNodeHandles, and the agent checks what an action would touch with
// Inspect() first.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_PAGE_CONTROLLER_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_PAGE_CONTROLLER_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/callback_list.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "chrome/browser/molt_ai/security/activation_policy.h"
#include "chrome/browser/molt_ai/voice/page_map.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "ui/accessibility/ax_enums.mojom-forward.h"
#include "ui/accessibility/ax_mode.h"
#include "ui/accessibility/ax_tree_id.h"
#include "url/origin.h"

namespace content {
class RenderFrameHost;
class ScopedAccessibilityMode;
}  // namespace content

namespace ui {
class AXNode;
class AXTree;
struct AXActionData;
struct AXLocationAndScrollUpdates;
struct AXUpdatesAndEvents;
}  // namespace ui

namespace molt_ai {
namespace voice {

class VoicePageController
    : public content::WebContentsObserver,
      public content::WebContentsUserData<VoicePageController> {
 public:
  // The accessibility mode voice mode holds on its tab: kWebContents (the
  // tree: roles, names, values, states, bounds, link targets, input types)
  // and kExtendedProperties (checked state, placeholders, heading levels,
  // list positions, HTML tags, autocomplete and default actions). Not
  // kNativeAPIs (no platform accessibility objects; the OS is not told an
  // assistive technology runs), kInlineTextBoxes, kHTML or kScreenReader
  // (see the .cc).
  static ui::AXMode AccessibilityMode();

  // Caps on what the controller keeps.
  static constexpr size_t kMaxTrees = 64;
  static constexpr size_t kMaxTreeNodes = 200000;
  // The nodes of the child frames' trees together (the top-level document's
  // tree has kMaxTreeNodes of its own).
  static constexpr size_t kMaxFrameTreeNodes = 200000;
  static constexpr int kMaxResetsPerPage = 3;
  // Text written by SetText(), in bytes.
  static constexpr size_t kMaxSetTextBytes = 8192;
  // Inspect(): the ancestors read above the node.
  static constexpr size_t kMaxInspectAncestors = 32;
  // OptionsOf(): the options listed, and their names, in code points.
  static constexpr size_t kMaxOptions = 30;
  static constexpr size_t kMaxOptionChars = 80;

  // Who holds the controller (Start()): the mode is held while either does.
  enum class Holder {
    kVoiceMode,  // voice mode on this tab (voice_controller.h)
    kWebAgent,   // a web agent run on this tab (agents/web_agent.h)
  };

  enum class ActionResult {
    kDone,               // sent to the page
    kNotActive,          // voice mode is off for this tab
    kStale,              // the node or its frame is gone: map again
    kNotAllowed,         // the frame isn't an http(s) document
    kDisabled,           // the control is disabled
    kNotEditable,        // not a text field that can be written
    kCredentialField,    // a password, card or one-time code field
    kTooLong,            // more text than kMaxSetTextBytes
    kNotMedia,           // not a <video> or <audio>
    kNoMediaSession,     // nothing has played in this tab
    kFocusNotConfirmed,  // Submit: the field didn't get the focus in time
    kCancelled,          // Submit: replaced by another, or voice mode ended
    kNoSuchOption,       // SelectOption: the select has no such option
  };
  static std::string_view ActionResultName(ActionResult result);

  struct Stats {
    size_t trees = 0;
    size_t unserialize_failures = 0;
    size_t dropped_updates = 0;
    size_t resets = 0;
    size_t trees_over_cap = 0;
  };

  VoicePageController(const VoicePageController&) = delete;
  VoicePageController& operator=(const VoicePageController&) = delete;
  ~VoicePageController() override;

  // |holder| starts or stops holding the controller (voice mode on and off
  // for this tab, or a web agent run). The accessibility mode is taken by
  // the first holder and released, with the trees, by the last. A web
  // agent's holds are counted (two runs may share a tab); voice mode's is
  // one.
  void Start(Holder holder = Holder::kVoiceMode);
  void Stop(Holder holder = Holder::kVoiceMode);
  bool active() const { return mode_ != nullptr; }
  bool held_by(Holder holder) const;

  // Whether the top-level document's tree is here (the map is empty until
  // it is).
  bool HasPageTree();
  // The page map of the trees held now.
  PageMap BuildMap(const PageMapLimits& limits = PageMapLimits());

  // What an action on |target| would touch now, read from the tree its
  // frame has now (the web agent's checks before it acts,
  // agents/agent_page_map.h).
  struct TargetFacts {
    TargetFacts();
    ~TargetFacts();
    TargetFacts(const TargetFacts&);
    TargetFacts& operator=(const TargetFacts&);
    TargetFacts(TargetFacts&&);
    TargetFacts& operator=(TargetFacts&&);

    // The node, then its ancestors, nearest first, at most
    // kMaxInspectAncestors of them, in its frame's tree: each as
    // PageNodeFromData() reads it (role, states, names, value, link
    // target, HTML tag, input type, autocomplete; no children or bounds).
    std::vector<PageNode> chain;
    // The index in |chain| of the nearest form; -1 if none was found.
    int form_index = -1;
    // The frame's committed origin, and whether the frame is the top-level
    // document's.
    url::Origin origin;
    bool main_frame = false;
    // Whether every frame above it, up to the top-level document, has its
    // origin (FramedByItsOwnOrigin()).
    bool framed_by_own_origin = false;
  };
  // As every action does, the target is found again first: a stale handle
  // (*|error| kStale) or a frame that isn't an http(s) document
  // (kNotAllowed) gives nullopt.
  std::optional<TargetFacts> Inspect(const PageNodeHandle& target,
                                     ActionResult* error);

  // The committed origin of the frame whose tree has |tree_index|
  // (PageNodeHandle::tree), if it is one of this tab's primary page's.
  std::optional<url::Origin> OriginOfTree(int32_t tree_index);
  // Whether that frame is one of this tab's primary page's and every frame
  // above it, up to the top-level document (through a fenced frame's outer
  // document), has its origin: false for a frame of the page's origin that
  // another site's frame embeds, which chose what it shows.
  bool FramedByItsOwnOrigin(int32_t tree_index);

  // The names of a select's options (a collapsed <select>, whose options
  // the map doesn't number, or a list box), in order: at most kMaxOptions,
  // each cleaned for speech and capped at kMaxOptionChars; disabled ones
  // left out. Empty for anything else, or a stale handle.
  std::vector<std::string> OptionsOf(const PageNodeHandle& select);

  // Chooses the option named |option| (as OptionsOf() gives it) of the
  // select |select|: the option's default action, which Blink carries out
  // as assistive technology's choice (HTMLOptionElement::AccessKeyAction():
  // the select takes it and fires its input and change events), with user
  // activation. kNoSuchOption if it has none by that name.
  ActionResult SelectOption(const PageNodeHandle& select,
                            std::string_view option);

  // Accessibility actions on a node of the last map. |intent| is what the
  // user asked the press for, when it was full screen or playing media (the
  // activation guard lets the page go full screen then).
  ActionResult Press(const PageNodeHandle& target,
                     activation::Intent intent = activation::Intent::kNone);
  ActionResult Focus(const PageNodeHandle& target);
  ActionResult ScrollIntoView(const PageNodeHandle& target);
  ActionResult SetText(const PageNodeHandle& field, std::string_view text);

  // Submits |field|'s form: presses the form's submit input when it has
  // one; otherwise focuses the field, waits (up to a few seconds) until its
  // frame reports the focus there, and sends Enter to it as the keyboard
  // would. |target| may also be a button, which is pressed.
  using SubmitCallback = base::OnceCallback<void(ActionResult)>;
  void Submit(const PageNodeHandle& field, SubmitCallback done);

  // Plays or pauses a <video> or <audio> through its default action: an
  // accessibility click, which gives the page user activation, so the
  // page's player may start with sound. The click goes to the element
  // itself, so it plays what a site's player plays on a click (YouTube's
  // does); a bare <video controls> doesn't play from it (Blink's controls
  // see only clicks on their own buttons): press the controls' play
  // button instead, a button inside the media on the map. Chromium's
  // accessibility media actions (kResumeMedia, kSuspendMedia) do nothing
  // in web pages (RenderAccessibilityImpl::PerformAction()).
  ActionResult PlayMedia(const PageNodeHandle& media);
  // Pauses or resumes what plays in the tab, as the media keys do (the
  // media session).
  ActionResult PauseMedia();
  ActionResult ResumeMedia();

  // Steps a slider or spin button up or down: Blink's increment and
  // decrement actions (AXNodeObject::OnNativeIncrementAction()), which
  // give the page user activation, step a native range input, and send an
  // ARIA slider the arrow key its script listens to.
  ActionResult Increment(const PageNodeHandle& target);
  ActionResult Decrement(const PageNodeHandle& target);

  // Scrolls the top-level document as assistive technology does: kDown,
  // kUp, kLeft and kRight by Blink's step for them (AXObject::Scroll():
  // four fifths of the viewport), kTop and kBottom to either end
  // (kSetScrollOffset, clamped by the page's scroller).
  enum class PageScrollAction { kDown, kUp, kLeft, kRight, kTop, kBottom };
  ActionResult ScrollPage(PageScrollAction action);

  const Stats& stats() const { return stats_; }

  // Runs |callback| after each batch of updates that changed a tree: its
  // nodes, or where they are (location and scroll changes). The voice
  // overlay follows the page with it (chrome/browser/ui/views/molt_ai/voice/
  // voice_overlay.h).
  base::CallbackListSubscription AddTreesUpdatedCallback(
      base::RepeatingClosure callback);

  // Runs |callback| after each batch of tree updates that changed a tree.
  void SetTreesUpdatedCallbackForTesting(base::RepeatingClosure callback);
  // Lower caps than kMaxTrees and kMaxFrameTreeNodes.
  void SetTreeCapsForTesting(size_t max_trees, size_t max_frame_tree_nodes);

 private:
  friend class content::WebContentsUserData<VoicePageController>;
  WEB_CONTENTS_USER_DATA_KEY_DECL();

  explicit VoicePageController(content::WebContents* web_contents);

  struct HeldTree {
    HeldTree();
    HeldTree(HeldTree&&);
    HeldTree& operator=(HeldTree&&);
    ~HeldTree();

    int32_t index = -1;
    std::unique_ptr<ui::AXTree> tree;
  };

  // A node found again for an action.
  struct Target {
    raw_ptr<content::RenderFrameHost> frame = nullptr;
    raw_ptr<ui::AXTree> tree = nullptr;
    raw_ptr<const ui::AXNode> node = nullptr;
    ui::AXTreeID tree_id;
    int32_t tree_index = -1;
  };

  struct PendingSubmit {
    PageNodeHandle field;
    SubmitCallback done;
  };

  // content::WebContentsObserver:
  void AccessibilityEventReceived(
      const ui::AXUpdatesAndEvents& details) override;
  void AccessibilityLocationChangesReceived(
      const ui::AXTreeID& tree_id,
      ui::AXLocationAndScrollUpdates& details) override;
  void RenderFrameDeleted(content::RenderFrameHost* render_frame_host) override;
  void PrimaryPageChanged(content::Page& page) override;
  void WebContentsDestroyed() override;

  // This tab's frame of the primary page with |tree_id|, or null.
  content::RenderFrameHost* FrameFor(const ui::AXTreeID& tree_id);
  // The nodes held of the child frames' trees, together.
  size_t FrameTreeNodes();
  void DropTree(const ui::AXTreeID& tree_id);
  void DropTreesNotInPrimaryPage();
  std::optional<Target> Resolve(const PageNodeHandle& handle,
                                ActionResult* error);
  // Sends |data| to |target|'s frame, noted with the tab's activation
  // guard first.
  ActionResult Perform(const Target& target,
                       ui::AXActionData data,
                       activation::Intent intent = activation::Intent::kNone);
  // Increment() and Decrement().
  ActionResult Step(const PageNodeHandle& target, ax::mojom::Action action);
  void FinishSubmit(ActionResult result);
  void MaybeSendPendingEnter();
  void OnSubmitTimeout();
  void SendEnter(content::RenderFrameHost* frame);

  std::unique_ptr<content::ScopedAccessibilityMode> mode_;
  // Voice mode holds the controller; web agent runs that do.
  bool voice_mode_hold_ = false;
  int web_agent_holds_ = 0;
  std::map<ui::AXTreeID, HeldTree> trees_;
  std::map<int32_t, ui::AXTreeID> tree_ids_;
  int32_t next_tree_index_ = 0;
  int resets_this_page_ = 0;
  size_t max_trees_ = kMaxTrees;
  size_t max_frame_tree_nodes_ = kMaxFrameTreeNodes;
  Stats stats_;
  std::optional<PendingSubmit> pending_submit_;
  base::OneShotTimer submit_timer_;
  base::RepeatingClosure trees_updated_for_testing_;
  base::RepeatingClosureList trees_updated_;
  base::WeakPtrFactory<VoicePageController> weak_factory_{this};
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_PAGE_CONTROLLER_H_
