// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The page map (docs/design/TOUCHLESS_BROWSING.md §5.3, §6.3): any page as a
// numbered list of the things a voice user can act on, the lists they can
// choose from (search results, videos, products, rows), and the headings,
// landmarks, title, focus and media a screen reader would report.
//
// The input is a tree of PageNodes, which ax_page_source.h builds from
// Chromium's accessibility trees (one per frame, joined into one page). The
// map works only from that structure, never from page scripts.
//
// Everything in the input comes from web pages and is untrusted: the
// structure may be huge, deep, cyclic or shared (a child listed twice), and
// every string may be hostile. BuildPageMap() visits at most
// PageMapLimits::max_nodes nodes, at most max_depth deep, and does at most
// max_work units of work in all; every string it outputs went through
// SpeechSafeText(), and the value of a protected or credential field is
// never output.
//
// Numbering. Every actionable item that isn't hidden gets a number, in
// reading (document) order, whether it is on screen or not, so numbers stay
// the same while the page is unchanged, however it is scrolled. Items off
// screen are marked (PageItem::on_screen); an overlay draws numbers for the
// ones on screen, and acting on one off screen scrolls it into view first.
// A page that changes (more results loaded, a menu opened) is mapped again,
// and the numbers after the change can move.
//
// Std-only; unit-tested in molt_ai_unittests (page_map_unittest.cc) and
// fuzzed by molt_page_map_fuzzer.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_PAGE_MAP_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_PAGE_MAP_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace molt_ai {
namespace voice {

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

// Where a node lives: the index of its tree (the frame's tree, as the
// source numbers them; voice_page_controller.h keeps an index per frame for
// the whole voice session, never reused) and the node's id in that tree.
// Opaque to the map. A handle names the same node for as long as its frame's
// document lives; the controller refuses a handle whose node is gone.
struct PageNodeHandle {
  int32_t tree = -1;
  int32_t node = 0;

  bool valid() const { return tree >= 0; }
  friend bool operator==(const PageNodeHandle&,
                         const PageNodeHandle&) = default;
};

// The roles the map tells apart; ax_page_source.h maps Chromium's roles to
// these (everything else is kGeneric).
enum class PageRole : uint8_t {
  kGeneric,      // containers, custom elements, anything else
  kRootWebArea,  // a document: the page, or a frame's
  kStaticText,
  kImage,
  kLink,
  kButton,     // also toggle, menu and pop-up buttons
  kTextField,  // single-line text fields and editable roots
  kSearchBox,
  kTextArea,  // multi-line text fields
  kComboBox,  // an editable combo box
  kSelect,    // a <select> (collapsed) or a list box button
  kCheckBox,
  kRadio,
  kSwitch,
  kSlider,    // also spin buttons
  kOption,    // list box and select options
  kMenuItem,  // also menu item check boxes and radios
  kTab,
  kTreeItem,
  kListBox,
  kList,
  kListItem,
  kFeed,
  kArticle,
  kTable,
  kGrid,  // also tree grids
  kRow,
  kCell,
  kColumnHeader,
  kRowHeader,
  kHeading,
  kVideo,
  kAudio,
  kMain,
  kNavigation,
  kBanner,
  kContentInfo,
  kComplementary,
  kSearch,  // the search landmark
  kForm,
  kRegion,
  kDialog,
  kMenu,
  kIframe,  // the host of a child frame's tree
  kParagraph,
  kTime,
};

// State bits of a PageNode.
enum PageState : uint32_t {
  kStateFocusable = 1u << 0,
  kStateFocused = 1u << 1,
  kStateEditable = 1u << 2,
  kStateDisabled = 1u << 3,
  kStateReadOnly = 1u << 4,
  kStateChecked = 1u << 5,
  kStateMixed = 1u << 6,  // checked "mixed"
  kStateExpanded = 1u << 7,
  kStateCollapsed = 1u << 8,
  kStateInvisible = 1u << 9,  // hidden by style; not shown, not numbered
  kStateIgnored = 1u << 10,   // in the tree for structure only
  kStateOffscreen = 1u << 11,
  kStateProtected = 1u << 12,  // a password field
  kStateRequired = 1u << 13,
  kStateSelected = 1u << 14,
  kStateMultiline = 1u << 15,
  kStateHasDefaultAction = 1u << 16,  // the node itself can be "clicked"
};

// Page coordinates in CSS pixels of the top-level page (0, 0 is the top
// left of the viewport), as the overlay needs them. Empty (0 by 0) when
// unknown.
struct PageRect {
  float x = 0;
  float y = 0;
  float width = 0;
  float height = 0;

  bool empty() const { return !(width > 0) || !(height > 0); }
  // The area, capped, and 0 for an empty or non-finite rectangle.
  double Area() const;
};

struct PageNode {
  PageNode();
  ~PageNode();
  PageNode(const PageNode&);
  PageNode& operator=(const PageNode&);
  PageNode(PageNode&&);
  PageNode& operator=(PageNode&&);

  PageNodeHandle handle;
  PageRole role = PageRole::kGeneric;
  std::string name;
  std::string value;
  std::string description;
  std::string placeholder;
  uint32_t states = 0;
  PageRect bounds;
  // A link's target (raw page data).
  std::string url;
  int heading_level = 0;
  // ARIA / list position: 1-based, 0 when unknown.
  int pos_in_set = 0;
  int set_size = 0;
  // Lower case: "div", "a", "ytd-video-renderer"; "" for nodes the
  // accessibility tree marks ignored (Blink leaves the tag out for those).
  std::string html_tag;
  // An <input>'s type attribute, and its autocomplete attribute.
  std::string input_type;
  std::string autocomplete;
  // Indices of the children in PageTree::nodes, in order.
  std::vector<uint32_t> children;

  bool Has(uint32_t state) const { return (states & state) != 0; }
};

struct PageTree {
  PageTree();
  ~PageTree();
  PageTree(const PageTree&);
  PageTree& operator=(const PageTree&);
  PageTree(PageTree&&);
  PageTree& operator=(PageTree&&);

  // nodes[0] is the root: the top-level document. A child index that is out
  // of range, or names a node already reached, is skipped.
  std::vector<PageNode> nodes;
  // The page's title (the top-level document's).
  std::string title;
  // The focused node, if any.
  PageNodeHandle focus;
};

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

enum class ItemKind : uint8_t {
  kLink,
  kButton,
  kTextField,
  kCheckBox,
  kRadio,
  kSwitch,
  kSelect,
  kOption,
  kMenuItem,
  kTab,
  kTreeItem,
  kSlider,
  kMedia,      // a <video> or <audio>
  kClickable,  // anything else with a default action ("div onclick")
};

// "link", "button", "text field", ...: a stable lower-case name.
std::string_view ItemKindName(ItemKind kind);

// What kind of secret a field holds (agent_safety's FieldKind, plus fields
// whose label names a secret). Such a field's value is never in the map,
// and the controller never writes one.
enum class CredentialKind : uint8_t {
  kNone,
  kPassword,
  kPaymentCard,
  kOneTimeCode,
};

std::string_view CredentialKindName(CredentialKind kind);

// The credential kind of a text field: protected (password) state, its
// input type and autocomplete tokens as agent_safety::ClassifyField() reads
// them, and a label, placeholder or name that names a secret ("password",
// "PIN", "CVV", "security code", "one-time code", "card number").
CredentialKind ClassifyCredential(const PageNode& node);

struct PageItem {
  PageItem();
  ~PageItem();
  PageItem(const PageItem&);
  PageItem& operator=(const PageItem&);
  PageItem(PageItem&&);
  PageItem& operator=(PageItem&&);

  int number = 0;  // 1-based
  PageNodeHandle handle;
  ItemKind kind = ItemKind::kClickable;
  std::string label;
  // The field's value or the control's current choice; never a credential
  // field's (see |filled|).
  std::string value;
  PageRect bounds;
  bool on_screen = true;
  bool focused = false;
  bool disabled = false;
  bool checked = false;
  bool expanded = false;
  bool collapsed = false;
  bool required = false;
  // A text field the controller may write (SetText): editable, not read
  // only or disabled, and not a credential field.
  bool writable = false;
  CredentialKind credential = CredentialKind::kNone;
  // A text field with a value, for credential fields whose value is left
  // out ("password field, filled").
  bool filled = false;
  // A link's target (raw page data: check it with agent_safety before
  // navigating to it).
  std::string url;
  // The list and entry (indices into PageMap::lists and its entries) whose
  // primary action this is; -1 otherwise.
  int list = -1;
  int entry = -1;
};

// A count shown with a list entry: "2.1B views" is {2100000000, "views"}.
struct EntryCount {
  int64_t value = 0;
  std::string unit;
  std::string text;  // as shown
};

// One entry of a list: one search result, video, product or row.
// Converts directly to the voice matcher's candidate (position, on-screen
// number, kind, title and fields).
struct PageListEntry {
  PageListEntry();
  ~PageListEntry();
  PageListEntry(const PageListEntry&);
  PageListEntry& operator=(const PageListEntry&);
  PageListEntry(PageListEntry&&);
  PageListEntry& operator=(PageListEntry&&);

  int position = 0;  // 1-based, in the list
  // The entry's primary action (its main link): its item number, kind and
  // node. An entry with nothing to act on (a table row of text) has number
  // 0, and then |kind| and |handle| mean nothing.
  int number = 0;
  ItemKind kind = ItemKind::kLink;
  PageNodeHandle handle;
  // The entry's own node (the row, list item or repeated container).
  PageNodeHandle node;
  std::string title;
  // The author, channel or site, and its link's item number (0 if it is
  // plain text).
  std::string channel;
  int channel_number = 0;
  // A duration ("6:07" is 367 s); -1 when none.
  int duration_seconds = -1;
  std::string duration;  // as shown
  std::string date;      // as shown: "15 years ago", "Jan 5, 2024"
  std::vector<EntryCount> counts;
  // The entry's other text, deduplicated, in order.
  std::vector<std::string> texts;
  PageRect bounds;
  bool on_screen = true;
};

enum class ListKind : uint8_t {
  kList,      // role list (<ul>, <ol>) or list box
  kFeed,      // role feed (articles)
  kTable,     // rows of a table or grid
  kRepeated,  // sibling subtrees of similar shape
};

std::string_view ListKindName(ListKind kind);

struct PageList {
  PageList();
  ~PageList();
  PageList(const PageList&);
  PageList& operator=(const PageList&);
  PageList(PageList&&);
  PageList& operator=(PageList&&);

  ListKind kind = ListKind::kRepeated;
  // The container's name, or the nearest heading before it.
  std::string label;
  PageNodeHandle container;
  std::vector<PageListEntry> entries;
  // How prominent the list is (see BuildPageMap()); lists are sorted by it,
  // highest first, so lists[0] is "the results".
  double prominence = 0;
  bool in_main = false;
  bool in_navigation = false;  // navigation, banner, footer, aside or menu
};

struct PageHeading {
  int level = 2;
  std::string text;
  PageNodeHandle handle;
  bool on_screen = true;
};

enum class LandmarkKind : uint8_t {
  kMain,
  kNavigation,
  kBanner,
  kContentInfo,
  kComplementary,
  kSearch,
  kForm,
  kRegion,
};

std::string_view LandmarkKindName(LandmarkKind kind);

struct PageLandmark {
  LandmarkKind kind = LandmarkKind::kMain;
  std::string label;
  PageNodeHandle handle;
};

struct PageMedia {
  bool video = true;
  std::string label;
  PageNodeHandle handle;
  int number = 0;  // its item number
  PageRect bounds;
  bool on_screen = true;
};

struct PageMapStats {
  size_t nodes_visited = 0;
  // Caps that cut the map short.
  bool node_cap = false;
  bool depth_cap = false;
  bool work_cap = false;
  bool item_cap = false;
  // Child indices that were out of range or named a node already reached.
  size_t bad_links = 0;
};

struct PageMap {
  PageMap();
  ~PageMap();
  PageMap(const PageMap&);
  PageMap& operator=(const PageMap&);
  PageMap(PageMap&&);
  PageMap& operator=(PageMap&&);

  std::string title;
  std::vector<PageItem> items;  // items[i].number == i + 1
  std::vector<PageList> lists;  // most prominent first
  std::vector<PageHeading> headings;
  std::vector<PageLandmark> landmarks;
  std::vector<PageMedia> media;
  // The focused item's number, 0 if the focus isn't on an item.
  int focused_item = 0;
  // A credential field is on the page.
  bool has_credential_fields = false;
  PageMapStats stats;

  // The item numbered |number|, or null.
  const PageItem* ItemByNumber(int number) const;
  // The item for |handle|, or null.
  const PageItem* ItemForHandle(const PageNodeHandle& handle) const;
};

// Caps on the work BuildPageMap() does and on what it outputs.
struct PageMapLimits {
  size_t max_nodes = 20000;
  size_t max_depth = 256;
  // Units: a node visited, a child link followed, a node looked at while
  // reading an entry, labelling an item or comparing siblings.
  size_t max_work = 1000000;
  size_t max_items = 2000;
  size_t max_lists = 32;
  size_t max_entries_per_list = 200;
  size_t max_headings = 200;
  size_t max_landmarks = 50;
  size_t max_media = 50;
  // Lists with fewer entries are left out: semantic lists (and tables) need
  // 2, repeated structures need 3.
  size_t min_semantic_entries = 2;
  size_t min_repeated_entries = 3;
};

// Caps on the text BuildPageMap() outputs, in code points.
inline constexpr size_t kMaxPageTitleChars = 160;
inline constexpr size_t kMaxItemLabelChars = 120;
inline constexpr size_t kMaxItemValueChars = 120;
inline constexpr size_t kMaxEntryTitleChars = 160;
inline constexpr size_t kMaxEntryFieldChars = 80;
inline constexpr size_t kMaxEntryTexts = 6;
inline constexpr size_t kMaxEntryCounts = 3;
inline constexpr size_t kMaxHeadingChars = 120;
inline constexpr size_t kMaxLinkUrlBytes = 2048;

// Builds the map of |tree|.
//
// Lists are found three ways: semantic lists (role list, list box, feed,
// table and grid, whose entries are list items, options, articles and
// rows), and repeated structures: a container whose children (at least
// min_repeated_entries of them, not inline text) have subtrees of similar
// shape and each hold a link, button or other action. Each entry gets a
// primary action (the link in or around its heading, else its most
// descriptive link), a title (that link's visible text or the heading's;
// not a long aria-label), a channel (a link to a channel, author or user
// page, or another short link or the site name in the result's link) and
// typed fields (duration, counts, date), and its other text.
//
// Lists are ranked by prominence: more and larger entries, entries with a
// title, fields and a heading, inside the main landmark, count for a list;
// being in navigation, a banner, footer, aside or menu, or entries that each
// hold many actions (a list of sections) count against it.
PageMap BuildPageMap(const PageTree& tree,
                     const PageMapLimits& limits = PageMapLimits());

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_PAGE_MAP_H_
