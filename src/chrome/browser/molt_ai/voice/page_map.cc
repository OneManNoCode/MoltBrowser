// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/page_map.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

#include "chrome/browser/molt_ai/voice/page_map_text.h"
#include "chrome/services/molt_ai/inference/engine/raw_ptr_exclusion.h"

namespace molt_ai {
namespace voice {

namespace {

constexpr uint32_t kNoPos = std::numeric_limits<uint32_t>::max();

// Context inherited from ancestors.
enum Context : uint8_t {
  kInMain = 1 << 0,
  kInNavigation = 1 << 1,  // navigation, banner, footer, aside or menu
};

// How much of a subtree one look reads (nodes), and how deep below its root.
constexpr size_t kMaxLabelScan = 64;
constexpr size_t kMaxEntryScan = 400;
// An entry's fields come from at most this many of its text runs.
constexpr size_t kMaxFieldRuns = 48;
constexpr size_t kMaxSignatureScan = 64;
constexpr uint32_t kMaxSignatureDepth = 8;
// An action is an entry by itself only when its subtree has at least this
// many nodes (itself included): a card, not a bare button.
constexpr uint32_t kMinCardNodes = 4;
// Siblings are clustered against at most this many shapes.
constexpr size_t kMaxClusters = 8;
// Two shapes are alike from this Jaccard similarity on.
constexpr double kSimilarShapes = 0.5;
// The bytes of text gathered before it is cleaned, per code point wanted.
constexpr size_t kGatherBytesPerChar = 6;
// A list's label may come from a heading at most this many siblings back.
constexpr size_t kMaxLabelSiblings = 3;
// Entries holding more actions than this on average are a list of sections.
constexpr double kMaxItemsPerEntry = 12;

struct Visit {
  uint32_t node = 0;         // index into PageTree::nodes
  uint32_t parent = kNoPos;  // position in the order
  uint32_t depth = 0;
  uint8_t context = 0;
};

// Whether |text| has the word "search" (any case) in its first bytes:
// "Search", "Search YouTube", "search…".
bool SaysSearch(std::string_view text) {
  constexpr std::string_view kWord = "search";
  constexpr size_t kMaxScan = 256;
  const std::string_view head = text.substr(0, kMaxScan);
  auto lower = [](char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
  };
  auto letter = [&](char c) {
    const char l = lower(c);
    return l >= 'a' && l <= 'z';
  };
  for (size_t i = 0; i + kWord.size() <= head.size(); ++i) {
    if (i > 0 && letter(head[i - 1])) {
      continue;
    }
    size_t k = 0;
    while (k < kWord.size() && lower(head[i + k]) == kWord[k]) {
      ++k;
    }
    if (k == kWord.size() &&
        (i + k == head.size() || !letter(head[i + k]))) {
      return true;
    }
  }
  return false;
}

// The kind of item a node is by its role alone; nullopt for roles that
// are items only through a default action (kClickable).
std::optional<ItemKind> RoleItemKind(PageRole role) {
  switch (role) {
    case PageRole::kLink:
      return ItemKind::kLink;
    case PageRole::kButton:
      return ItemKind::kButton;
    case PageRole::kTextField:
    case PageRole::kSearchBox:
    case PageRole::kTextArea:
    case PageRole::kComboBox:
      return ItemKind::kTextField;
    case PageRole::kSelect:
      return ItemKind::kSelect;
    case PageRole::kCheckBox:
      return ItemKind::kCheckBox;
    case PageRole::kRadio:
      return ItemKind::kRadio;
    case PageRole::kSwitch:
      return ItemKind::kSwitch;
    case PageRole::kSlider:
      return ItemKind::kSlider;
    case PageRole::kOption:
      return ItemKind::kOption;
    case PageRole::kMenuItem:
      return ItemKind::kMenuItem;
    case PageRole::kTab:
      return ItemKind::kTab;
    case PageRole::kTreeItem:
      return ItemKind::kTreeItem;
    case PageRole::kVideo:
    case PageRole::kAudio:
      return ItemKind::kMedia;
    default:
      return std::nullopt;
  }
}

bool IsListContainer(PageRole role) {
  return role == PageRole::kList || role == PageRole::kListBox ||
         role == PageRole::kFeed || role == PageRole::kTable ||
         role == PageRole::kGrid;
}

bool IsSemanticEntry(PageRole role) {
  return role == PageRole::kListItem || role == PageRole::kOption ||
         role == PageRole::kArticle || role == PageRole::kRow;
}

uint8_t OwnContext(PageRole role) {
  switch (role) {
    case PageRole::kMain:
      return kInMain;
    case PageRole::kNavigation:
    case PageRole::kBanner:
    case PageRole::kContentInfo:
    case PageRole::kComplementary:
    case PageRole::kMenu:
      return kInNavigation;
    default:
      return 0;
  }
}

std::optional<LandmarkKind> LandmarkFor(const PageNode& node) {
  switch (node.role) {
    case PageRole::kMain:
      return LandmarkKind::kMain;
    case PageRole::kNavigation:
      return LandmarkKind::kNavigation;
    case PageRole::kBanner:
      return LandmarkKind::kBanner;
    case PageRole::kContentInfo:
      return LandmarkKind::kContentInfo;
    case PageRole::kComplementary:
      return LandmarkKind::kComplementary;
    case PageRole::kSearch:
      return LandmarkKind::kSearch;
    // A form or region is a landmark only when it has a name.
    case PageRole::kForm:
      return node.name.empty() ? std::nullopt
                               : std::optional(LandmarkKind::kForm);
    case PageRole::kRegion:
      return node.name.empty() ? std::nullopt
                               : std::optional(LandmarkKind::kRegion);
    default:
      return std::nullopt;
  }
}

// ASCII whitespace, as agent_safety splits and trims attributes.
bool IsHtmlSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}

bool IsAsciiAlnum(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9');
}

// Whether |phrase| (lower case) is in |folded| as whole words.
bool HasWords(std::string_view folded, std::string_view phrase) {
  size_t from = 0;
  while (true) {
    const size_t at = folded.find(phrase, from);
    if (at == std::string_view::npos) {
      return false;
    }
    const size_t after = at + phrase.size();
    const bool start_ok = at == 0 || !IsAsciiAlnum(folded[at - 1]);
    const bool end_ok = after == folded.size() || !IsAsciiAlnum(folded[after]);
    if (start_ok && end_ok) {
      return true;
    }
    from = at + 1;
  }
}

CredentialKind CredentialFromLabel(std::string_view text) {
  const std::string folded =
      FoldForCompare(text.substr(0, kMaxSpeechScanBytes));
  for (std::string_view word :
       {"password", "passcode", "passphrase", "pin", "passwort", "kennwort",
        "contraseña", "mot de passe"}) {
    if (HasWords(folded, word)) {
      return CredentialKind::kPassword;
    }
  }
  for (std::string_view word :
       {"cvv", "cvc", "csc", "cvv2", "security code", "card number",
        "credit card", "debit card", "card verification"}) {
    if (HasWords(folded, word)) {
      return CredentialKind::kPaymentCard;
    }
  }
  for (std::string_view word :
       {"otp", "one-time code", "one time code", "one-time password",
        "verification code", "authentication code", "2fa", "two-factor"}) {
    if (HasWords(folded, word)) {
      return CredentialKind::kOneTimeCode;
    }
  }
  return CredentialKind::kNone;
}

std::string LowerAscii(std::string_view s, size_t max_bytes) {
  std::string out(s.substr(0, max_bytes));
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

// Sorted, deduplicated shape tokens of a subtree; Jaccard similarity.
using Shape = std::vector<uint32_t>;

double Similarity(const Shape& a, const Shape& b) {
  if (a.empty() && b.empty()) {
    return 1;
  }
  size_t i = 0;
  size_t j = 0;
  size_t common = 0;
  while (i < a.size() && j < b.size()) {
    if (a[i] == b[j]) {
      ++common;
      ++i;
      ++j;
    } else if (a[i] < b[j]) {
      ++i;
    } else {
      ++j;
    }
  }
  const size_t all = a.size() + b.size() - common;
  return all == 0 ? 0 : static_cast<double>(common) / static_cast<double>(all);
}

bool LooksLikeChannelUrl(std::string_view url) {
  const std::string lower = LowerAscii(url, kMaxLinkUrlBytes);
  for (std::string_view part :
       {"/@", "/channel/", "/c/", "/user/", "/author/", "/profile/"}) {
    if (lower.find(part) != std::string::npos) {
      return true;
    }
  }
  return false;
}

// Text that is a field value rather than a name: a duration, count or date.
bool IsFieldLike(std::string_view text) {
  return ParseDuration(text).has_value() || ParseCount(text).has_value() ||
         LooksLikeDate(text);
}

class Builder {
 public:
  Builder(const PageTree& tree,
          const PageMapLimits& limits,
          const PageMapHints* hints)
      : tree_(&tree), limits_(limits), hints_(hints) {}

  PageMap Build() {
    if (!tree_->nodes.empty()) {
      map_.page_tree = tree_->nodes[0].handle.tree;
    }
    Walk();
    if (order_.empty()) {
      map_.title = SpeechSafeText(tree_->title, kMaxPageTitleChars);
      return std::move(map_);
    }
    IndexSubtrees();
    MakeItems();
    MakeHeadingsLandmarksMedia();
    MakeLists();
    Finish();
    return std::move(map_);
  }

 private:
  const PageNode& NodeAt(uint32_t pos) const {
    return tree_->nodes[order_[pos].node];
  }

  // Spends |units| of the work budget; false once it is used up.
  bool Spend(size_t units) {
    work_ += units;
    if (work_ > limits_.max_work) {
      map_.stats.work_cap = true;
      return false;
    }
    return true;
  }

  bool OutOfWork() const { return work_ > limits_.max_work; }

  // In the top-level document, not in a frame it embeds.
  bool InPage(uint32_t pos) const {
    return NodeAt(pos).handle.tree == map_.page_tree;
  }

  // Visible: shown on the page (ignored nodes are kept for structure, but
  // are not items, headings or entries themselves).
  bool Shown(uint32_t pos) const {
    return !NodeAt(pos).Has(kStateInvisible) && !NodeAt(pos).Has(kStateIgnored);
  }

  // A pre-order walk of the tree from nodes[0], bounded in nodes, depth and
  // work; each node at most once.
  void Walk() {
    const std::vector<PageNode>& nodes = tree_->nodes;
    if (nodes.empty()) {
      return;
    }
    std::vector<uint8_t> seen(nodes.size(), 0);
    struct Pending {
      uint32_t node;
      uint32_t parent;
      uint32_t depth;
    };
    std::vector<Pending> stack;
    stack.push_back({0, kNoPos, 0});
    // The stack never holds more than this (a node may list millions of
    // children; only max_nodes of them can be visited anyway).
    const size_t max_pending = limits_.max_nodes * 2 + 16;
    while (!stack.empty()) {
      const Pending next = stack.back();
      stack.pop_back();
      if (seen[next.node]) {
        ++map_.stats.bad_links;
        continue;
      }
      if (order_.size() >= limits_.max_nodes) {
        map_.stats.node_cap = true;
        break;
      }
      if (!Spend(1)) {
        break;
      }
      seen[next.node] = 1;
      const PageNode& node = nodes[next.node];
      const uint8_t context =
          (next.parent == kNoPos ? 0 : order_[next.parent].context) |
          OwnContext(node.role);
      order_.push_back({next.node, next.parent, next.depth, context});
      const uint32_t pos = static_cast<uint32_t>(order_.size() - 1);
      // Children, pushed last first so they are visited in order.
      for (size_t k = node.children.size(); k-- > 0;) {
        if (!Spend(1)) {
          break;
        }
        const uint32_t child = node.children[k];
        if (child >= nodes.size() || seen[child]) {
          ++map_.stats.bad_links;
          continue;
        }
        if (next.depth + 1 > limits_.max_depth) {
          map_.stats.depth_cap = true;
          continue;
        }
        if (stack.size() >= max_pending) {
          map_.stats.node_cap = true;
          continue;
        }
        stack.push_back({child, pos, next.depth + 1});
      }
    }
    map_.stats.nodes_visited = order_.size();
  }

  // end_[p]: the position just past p's subtree (a pre-order subtree is a
  // contiguous run); children_ in CSR form.
  void IndexSubtrees() {
    const size_t n = order_.size();
    end_.resize(n);
    for (size_t p = 0; p < n; ++p) {
      end_[p] = static_cast<uint32_t>(p + 1);
    }
    for (size_t p = n; p-- > 1;) {
      const uint32_t parent = order_[p].parent;
      end_[parent] = std::max(end_[parent], end_[p]);
    }
    child_start_.assign(n + 1, 0);
    for (size_t p = 1; p < n; ++p) {
      ++child_start_[order_[p].parent + 1];
    }
    for (size_t p = 0; p < n; ++p) {
      child_start_[p + 1] += child_start_[p];
    }
    child_list_.resize(n > 0 ? n - 1 : 0);
    std::vector<uint32_t> fill(child_start_.begin(), child_start_.end() - 1);
    for (size_t p = 1; p < n; ++p) {
      child_list_[fill[order_[p].parent]++] = static_cast<uint32_t>(p);
    }
  }

  size_t ChildCount(uint32_t pos) const {
    return child_start_[pos + 1] - child_start_[pos];
  }
  uint32_t Child(uint32_t pos, size_t k) const {
    return child_list_[child_start_[pos] + k];
  }

  // The text a subtree shows: its static text and image names, in order,
  // from at most |max_nodes| nodes; cleaned and capped at |max_chars|.
  // Nodes inside |skip| (a subtree root, or kNoPos) are left out.
  std::string GatherText(uint32_t pos,
                         size_t max_nodes,
                         size_t max_chars,
                         uint32_t skip = kNoPos) {
    std::string raw;
    const size_t max_bytes = max_chars * kGatherBytesPerChar + 16;
    const uint32_t stop =
        static_cast<uint32_t>(std::min<size_t>(end_[pos], pos + max_nodes));
    for (uint32_t p = pos; p < stop && raw.size() < max_bytes; ++p) {
      if (!Spend(1)) {
        break;
      }
      if (skip != kNoPos && p >= skip && p < end_[skip]) {
        p = end_[skip] - 1;
        continue;
      }
      const PageNode& node = NodeAt(p);
      if (node.Has(kStateInvisible)) {
        continue;
      }
      if (node.role == PageRole::kStaticText ||
          (node.role == PageRole::kImage && p != pos)) {
        if (!node.name.empty()) {
          if (!raw.empty()) {
            raw.push_back(' ');
          }
          raw.append(node.name, 0,
                     std::min(node.name.size(), max_bytes - raw.size() + 1));
        }
      }
    }
    return SpeechSafeText(raw, max_chars);
  }

  // ---- Items -------------------------------------------------------------

  void MakeItems() {
    const size_t n = order_.size();
    kind_.assign(n, -1);
    // Decide kinds bottom-up: a node with a default action is an item only
    // when nothing below it is (else its descendants are numbered).
    std::vector<uint8_t> below(n, 0);
    for (size_t p = n; p-- > 0;) {
      const PageNode& node = NodeAt(static_cast<uint32_t>(p));
      if (Shown(static_cast<uint32_t>(p))) {
        std::optional<ItemKind> kind = RoleItemKind(node.role);
        if (!kind && node.Has(kStateEditable) && node.Has(kStateFocusable) &&
            node.role == PageRole::kGeneric) {
          kind = ItemKind::kTextField;  // an editable root
        }
        if (!kind && node.Has(kStateHasDefaultAction) && !below[p] &&
            node.role != PageRole::kRootWebArea &&
            node.role != PageRole::kStaticText) {
          kind = ItemKind::kClickable;
        }
        if (kind) {
          kind_[p] = static_cast<int8_t>(*kind);
        }
      }
      const uint32_t parent = order_[p].parent;
      if (parent != kNoPos && (below[p] || kind_[p] >= 0)) {
        below[parent] = 1;
      }
    }
    action_below_ = std::move(below);

    item_of_.assign(n, -1);
    size_t frame_items = 0;
    for (uint32_t p = 0; p < n; ++p) {
      if (kind_[p] < 0) {
        continue;
      }
      if (map_.items.size() >= limits_.max_items) {
        map_.stats.item_cap = true;
        break;
      }
      if (OutOfWork()) {
        break;
      }
      // A frame the page embeds numbers only its share.
      if (!InPage(p)) {
        if (frame_items >= limits_.max_frame_items) {
          map_.stats.item_cap = true;
          continue;
        }
        ++frame_items;
      }
      item_of_[p] = static_cast<int32_t>(map_.items.size());
      map_.items.push_back(MakeItem(p, static_cast<ItemKind>(kind_[p])));
    }
    // Items per subtree, for the lists' "actions per entry".
    items_before_.assign(n + 1, 0);
    for (size_t p = 0; p < n; ++p) {
      items_before_[p + 1] = items_before_[p] + (item_of_[p] >= 0 ? 1 : 0);
    }
  }

  PageItem MakeItem(uint32_t pos, ItemKind kind) {
    const PageNode& node = NodeAt(pos);
    PageItem item;
    item.number = static_cast<int>(map_.items.size() + 1);
    item.handle = node.handle;
    item.kind = kind;
    item.bounds = node.bounds;
    item.on_screen = !node.Has(kStateOffscreen);
    item.focused = node.Has(kStateFocused) ||
                   (tree_->focus.valid() && tree_->focus == node.handle);
    item.disabled = node.Has(kStateDisabled);
    item.checked = node.Has(kStateChecked);
    item.expanded = node.Has(kStateExpanded);
    item.collapsed = node.Has(kStateCollapsed);
    item.required = node.Has(kStateRequired);
    if (kind == ItemKind::kLink) {
      item.url = node.url.substr(0, kMaxLinkUrlBytes);
    }

    item.label = SpeechSafeText(node.name, kMaxItemLabelChars);
    const bool text_field = kind == ItemKind::kTextField;
    if (item.label.empty() && text_field) {
      item.label = SpeechSafeText(node.placeholder, kMaxItemLabelChars);
    }
    if (item.label.empty() && !text_field) {
      item.label = GatherText(pos, kMaxLabelScan, kMaxItemLabelChars);
    }
    if (item.label.empty()) {
      item.label = SpeechSafeText(node.description, kMaxItemLabelChars);
    }

    if (text_field) {
      item.credential = ClassifyCredential(node);
      item.filled = !node.value.empty();
      item.writable = node.Has(kStateEditable) && !node.Has(kStateReadOnly) &&
                      !node.Has(kStateDisabled) &&
                      item.credential == CredentialKind::kNone;
      item.search_field = item.credential == CredentialKind::kNone &&
                          (node.role == PageRole::kSearchBox ||
                           FoldForCompare(node.input_type) == "search" ||
                           SaysSearch(node.name) ||
                           SaysSearch(node.placeholder));
    }
    // A protected node's value is never kept, whatever its role.
    if (item.credential == CredentialKind::kNone &&
        !node.Has(kStateProtected) &&
        (text_field || kind == ItemKind::kSelect || kind == ItemKind::kSlider ||
         kind == ItemKind::kOption)) {
      item.value = SpeechSafeText(node.value, kMaxItemValueChars);
    }
    if (item.credential != CredentialKind::kNone) {
      map_.has_credential_fields = true;
    }
    return item;
  }

  // ---- Headings, landmarks, media ----------------------------------------

  void MakeHeadingsLandmarksMedia() {
    std::vector<PageMedia> frame_media;
    for (uint32_t p = 0; p < order_.size(); ++p) {
      if (OutOfWork()) {
        break;
      }
      const PageNode& node = NodeAt(p);
      if (node.role == PageRole::kHeading && Shown(p)) {
        if (map_.headings.size() < limits_.max_headings) {
          PageHeading heading;
          heading.level = node.heading_level >= 1 && node.heading_level <= 6
                              ? node.heading_level
                              : 2;
          heading.text = SpeechSafeText(node.name, kMaxHeadingChars);
          if (heading.text.empty()) {
            heading.text = GatherText(p, kMaxLabelScan, kMaxHeadingChars);
          }
          heading.handle = node.handle;
          heading.on_screen = !node.Has(kStateOffscreen);
          map_.headings.push_back(std::move(heading));
        }
      }
      if (std::optional<LandmarkKind> kind = LandmarkFor(node);
          kind && !node.Has(kStateInvisible) &&
          map_.landmarks.size() < limits_.max_landmarks) {
        PageLandmark landmark;
        landmark.kind = *kind;
        landmark.label = SpeechSafeText(node.name, kMaxItemLabelChars);
        landmark.handle = node.handle;
        map_.landmarks.push_back(std::move(landmark));
      }
      // The page's own media first: "play" never starts a frame's (an
      // ad's) while the page has a player of its own.
      std::vector<PageMedia>& media_list = InPage(p) ? map_.media : frame_media;
      if (item_of_[p] >= 0 &&
          map_.items[item_of_[p]].kind == ItemKind::kMedia &&
          media_list.size() < limits_.max_media) {
        const PageItem& item = map_.items[item_of_[p]];
        PageMedia media;
        media.video = node.role == PageRole::kVideo;
        media.label = item.label;
        media.handle = item.handle;
        media.number = item.number;
        media.bounds = item.bounds;
        media.on_screen = item.on_screen;
        media_list.push_back(std::move(media));
      }
    }
    for (PageMedia& media : frame_media) {
      if (map_.media.size() >= limits_.max_media) {
        break;
      }
      map_.media.push_back(std::move(media));
    }
  }

  // ---- Lists -------------------------------------------------------------

  struct Candidate {
    ListKind kind;
    uint32_t container;
    std::vector<uint32_t> entries;
  };

  // The site's results, as its skill named their entries (PageMapHints):
  // the entries this walk reached, in the top-level document, in order and
  // not inside one another, under their nearest common ancestor.
  std::optional<Candidate> SiteCandidate() {
    if (!hints_ || hints_->result_entries.size() < 2 || order_.empty()) {
      return std::nullopt;
    }
    std::vector<uint32_t> pos_of(tree_->nodes.size(), kNoPos);
    for (uint32_t p = 0; p < order_.size(); ++p) {
      pos_of[order_[p].node] = p;
    }
    Spend(order_.size());
    Candidate c;
    c.kind = ListKind::kRepeated;
    uint32_t after = 0;
    for (uint32_t node : hints_->result_entries) {
      if (c.entries.size() >= limits_.max_entries_per_list || !Spend(1)) {
        break;
      }
      const uint32_t pos = node < pos_of.size() ? pos_of[node] : kNoPos;
      if (pos == kNoPos || pos == 0 || pos < after || !InPage(pos) ||
          NodeAt(pos).Has(kStateInvisible)) {
        continue;
      }
      c.entries.push_back(pos);
      after = end_[pos];
    }
    if (c.entries.size() < 2) {
      return std::nullopt;
    }
    // The nearest ancestor of the first entry that holds the last.
    uint32_t container = order_[c.entries.front()].parent;
    const uint32_t last = c.entries.back();
    while (container != kNoPos && end_[container] <= last) {
      container = order_[container].parent;
    }
    c.container = container == kNoPos ? 0 : container;
    return c;
  }

  // Whether |c| (a list the map found) mostly lists the site's entries, or
  // parts or wrappers of them.
  bool AmongSiteEntries(const Candidate& c, const Candidate& site) const {
    size_t shared = 0;
    for (uint32_t e : c.entries) {
      // The site entry at or before |e|: |e| is inside it, or holds it.
      auto it = std::upper_bound(site.entries.begin(), site.entries.end(), e);
      const bool inside = it != site.entries.begin() && e < end_[*(it - 1)];
      const bool holds = it != site.entries.end() && *it < end_[e];
      shared += inside || holds ? 1 : 0;
    }
    return !c.entries.empty() && 2 * shared >= c.entries.size();
  }

  void MakeLists() {
    // The top-level document's lists are looked for (and read) first, so
    // that the frames it embeds can't spend the work budget before them.
    std::vector<Candidate> candidates;
    // The site's own results come before all of them.
    const std::optional<Candidate> site = SiteCandidate();
    if (site) {
      candidates.push_back(*site);
    }
    for (const bool page_pass : {true, false}) {
      for (uint32_t p = 0; p < order_.size() && !OutOfWork(); ++p) {
        const PageNode& node = NodeAt(p);
        if (InPage(p) != page_pass || node.Has(kStateInvisible)) {
          continue;
        }
        if (IsListContainer(node.role)) {
          Candidate c;
          c.container = p;
          c.kind = node.role == PageRole::kFeed
                       ? ListKind::kFeed
                       : (node.role == PageRole::kTable ||
                                  node.role == PageRole::kGrid
                              ? ListKind::kTable
                              : ListKind::kList);
          c.entries = SemanticEntries(p);
          if (c.entries.size() >= limits_.min_semantic_entries) {
            candidates.push_back(std::move(c));
          }
          continue;
        }
        FindRepeated(p, &candidates);
      }
    }

    for (size_t i = 0; i < candidates.size(); ++i) {
      if (OutOfWork()) {
        break;
      }
      const bool from_site = site && i == 0;
      if (site && !from_site && AmongSiteEntries(candidates[i], *site)) {
        continue;
      }
      map_.lists.push_back(MakeList(candidates[i]));
      if (from_site) {
        PageList& list = map_.lists.back();
        list.from_site = true;
        list.in_navigation = false;
        const std::string label =
            SpeechSafeText(hints_->results_label, kMaxItemLabelChars);
        if (!label.empty()) {
          list.label = label;
        }
      }
    }
    // The site's results first, then the top-level document's lists, then
    // the frames'; each most prominent first, ties in document order
    // (candidates are).
    const int32_t page_tree = map_.page_tree;
    std::stable_sort(map_.lists.begin(), map_.lists.end(),
                     [page_tree](const PageList& a, const PageList& b) {
                       if (a.from_site != b.from_site) {
                         return a.from_site;
                       }
                       const bool a_page = a.container.tree == page_tree;
                       const bool b_page = b.container.tree == page_tree;
                       if (a_page != b_page) {
                         return a_page;
                       }
                       return a.prominence > b.prominence;
                     });
    if (map_.lists.size() > limits_.max_lists) {
      map_.lists.resize(limits_.max_lists);
    }
    for (size_t l = 0; l < map_.lists.size(); ++l) {
      for (size_t e = 0; e < map_.lists[l].entries.size(); ++e) {
        const int number = map_.lists[l].entries[e].number;
        if (number > 0 && map_.items[number - 1].list < 0) {
          map_.items[number - 1].list = static_cast<int>(l);
          map_.items[number - 1].entry = static_cast<int>(e);
        }
      }
    }
  }

  PageRole EntryRoleFor(PageRole container) const {
    switch (container) {
      case PageRole::kListBox:
        return PageRole::kOption;
      case PageRole::kFeed:
        return PageRole::kArticle;
      case PageRole::kTable:
      case PageRole::kGrid:
        return PageRole::kRow;
      default:
        return PageRole::kListItem;
    }
  }

  // The entries of a semantic list: its list items (options, articles,
  // rows) not inside a nested list; a table's header rows left out.
  std::vector<uint32_t> SemanticEntries(uint32_t container) {
    std::vector<uint32_t> entries;
    const PageRole want = EntryRoleFor(NodeAt(container).role);
    for (uint32_t p = container + 1; p < end_[container]; ++p) {
      if (!Spend(1) || entries.size() >= limits_.max_entries_per_list) {
        break;
      }
      const PageNode& node = NodeAt(p);
      if (IsListContainer(node.role)) {
        p = end_[p] - 1;  // a nested list has its own entries
        continue;
      }
      if (node.role != want) {
        continue;
      }
      if (Shown(p) && !(want == PageRole::kRow && IsHeaderRow(p))) {
        entries.push_back(p);
      }
      p = end_[p] - 1;
    }
    return entries;
  }

  bool IsHeaderRow(uint32_t row) const {
    size_t cells = 0;
    for (size_t k = 0; k < ChildCount(row); ++k) {
      const PageRole role = NodeAt(Child(row, k)).role;
      if (role == PageRole::kCell || role == PageRole::kRowHeader) {
        return false;
      }
      if (role == PageRole::kColumnHeader) {
        ++cells;
      }
    }
    return cells > 0;
  }

  // The shape of a subtree: (role, depth) of its first kMaxSignatureScan
  // nodes down to kMaxSignatureDepth, wrappers (generic containers) left
  // out, and how many links, headings and buttons it holds and roughly how
  // big it is.
  Shape ShapeOf(uint32_t root) {
    Shape shape;
    uint32_t links = 0;
    uint32_t headings = 0;
    uint32_t buttons = 0;
    const uint32_t stop = static_cast<uint32_t>(
        std::min<size_t>(end_[root], root + kMaxSignatureScan));
    const uint32_t base = order_[root].depth;
    for (uint32_t p = root; p < stop; ++p) {
      if (!Spend(1)) {
        break;
      }
      const PageNode& node = NodeAt(p);
      if (node.Has(kStateInvisible)) {
        continue;
      }
      links += node.role == PageRole::kLink ? 1 : 0;
      headings += node.role == PageRole::kHeading ? 1 : 0;
      buttons += node.role == PageRole::kButton ? 1 : 0;
      const uint32_t depth = order_[p].depth - base;
      if (node.role == PageRole::kGeneric || depth > kMaxSignatureDepth) {
        continue;
      }
      shape.push_back((static_cast<uint32_t>(node.role) << 8) | (depth << 4));
    }
    shape.push_back(0xFF0000u | std::min(links, 15u));
    shape.push_back(0xFE0000u | std::min(headings, 15u));
    shape.push_back(0xFD0000u | std::min(buttons, 15u));
    size_t size = end_[root] - root;
    uint32_t bucket = 0;
    while (size > 1 && bucket < 16) {
      size >>= 1;
      ++bucket;
    }
    shape.push_back(0xFC0000u | bucket);
    std::sort(shape.begin(), shape.end());
    shape.erase(std::unique(shape.begin(), shape.end()), shape.end());
    return shape;
  }

  // Whether |pos| can be an entry of a repeated structure: a container
  // that holds an action, or an action that holds more than its own text
  // (a card: a link around a heading and details). A bare button or link
  // among others (a toolbar, a form's fields) is not an entry.
  bool CanBeEntry(uint32_t pos) const {
    if (NodeAt(pos).Has(kStateInvisible)) {
      return false;
    }
    if (item_of_[pos] >= 0) {
      return end_[pos] - pos >= kMinCardNodes;
    }
    return action_below_[pos] != 0;
  }

  // A repeated structure under |container|: at least min_repeated_entries
  // children (not inline text) with similar shapes, each holding an
  // action.
  void FindRepeated(uint32_t container, std::vector<Candidate>* out) {
    const size_t count = ChildCount(container);
    if (count < limits_.min_repeated_entries) {
      return;
    }
    const PageNode& node = NodeAt(container);
    if (RoleItemKind(node.role) || node.role == PageRole::kStaticText ||
        node.role == PageRole::kParagraph || node.role == PageRole::kHeading ||
        node.role == PageRole::kRow) {
      return;
    }
    std::vector<uint32_t> members;
    for (size_t k = 0; k < count; ++k) {
      const uint32_t child = Child(container, k);
      const PageNode& c = NodeAt(child);
      if (c.role == PageRole::kStaticText) {
        // Inline content (a paragraph with links) is not a list.
        if (!SpeechSafeText(c.name, 1).empty()) {
          return;
        }
        continue;
      }
      if (IsSemanticEntry(c.role)) {
        return;  // its semantic container lists these
      }
      if (CanBeEntry(child)) {
        members.push_back(child);
      }
    }
    if (members.size() < limits_.min_repeated_entries) {
      return;
    }
    struct Cluster {
      Shape shape;
      std::vector<uint32_t> members;
    };
    std::vector<Cluster> clusters;
    for (uint32_t member : members) {
      if (OutOfWork()) {
        return;
      }
      Shape shape = ShapeOf(member);
      bool placed = false;
      for (Cluster& cluster : clusters) {
        if (Similarity(cluster.shape, shape) >= kSimilarShapes) {
          cluster.members.push_back(member);
          placed = true;
          break;
        }
      }
      if (!placed && clusters.size() < kMaxClusters) {
        clusters.push_back({std::move(shape), {member}});
      }
    }
    for (Cluster& cluster : clusters) {
      if (cluster.members.size() < limits_.min_repeated_entries) {
        continue;
      }
      if (cluster.members.size() > limits_.max_entries_per_list) {
        cluster.members.resize(limits_.max_entries_per_list);
      }
      out->push_back(
          {ListKind::kRepeated, container, std::move(cluster.members)});
    }
  }

  // A list's label: the container's name; else a heading just before the
  // container or one of its two nearest ancestors (a preceding sibling, at
  // most kMaxLabelSiblings back); else the name of a named ancestor within
  // two levels (a "Contents" navigation around its list).
  std::string ListLabel(uint32_t container) {
    std::string label =
        SpeechSafeText(NodeAt(container).name, kMaxItemLabelChars);
    uint32_t at = container;
    for (int level = 0; level < 3 && label.empty(); ++level) {
      const uint32_t parent = order_[at].parent;
      if (parent == kNoPos) {
        break;
      }
      // Siblings before |at|, nearest first (children are in order).
      const auto first = child_list_.begin() + child_start_[parent];
      const auto last = child_list_.begin() + child_start_[parent + 1];
      const size_t index =
          static_cast<size_t>(std::lower_bound(first, last, at) - first);
      for (size_t back = 0; back < kMaxLabelSiblings && back < index; ++back) {
        const uint32_t sibling = Child(parent, index - 1 - back);
        if (NodeAt(sibling).role == PageRole::kHeading && Shown(sibling)) {
          label = SpeechSafeText(NodeAt(sibling).name, kMaxItemLabelChars);
          if (label.empty()) {
            label = GatherText(sibling, kMaxLabelScan, kMaxItemLabelChars);
          }
          break;
        }
      }
      if (label.empty() && level < 2 && !NodeAt(parent).name.empty() &&
          NodeAt(parent).role != PageRole::kRootWebArea) {
        label = SpeechSafeText(NodeAt(parent).name, kMaxItemLabelChars);
      }
      at = parent;
    }
    return label;
  }

  PageList MakeList(const Candidate& c) {
    PageList list;
    list.kind = c.kind;
    const PageNode& container = NodeAt(c.container);
    list.container = container.handle;
    list.label = ListLabel(c.container);
    list.in_main = (order_[c.container].context & kInMain) != 0;
    list.in_navigation = (order_[c.container].context & kInNavigation) != 0;

    double area = 0;
    size_t with_title = 0;
    size_t with_fields = 0;
    size_t with_heading = 0;
    size_t with_action = 0;
    size_t items = 0;
    for (uint32_t e : c.entries) {
      if (OutOfWork()) {
        break;
      }
      bool has_heading = false;
      PageListEntry entry = MakeEntry(e, &has_heading);
      entry.position = static_cast<int>(list.entries.size() + 1);
      area += std::min(entry.bounds.Area(), 1e6);
      with_title += entry.title.empty() ? 0 : 1;
      with_fields +=
          (!entry.channel.empty() || entry.duration_seconds >= 0 ||
           !entry.date.empty() || !entry.counts.empty() || !entry.texts.empty())
              ? 1
              : 0;
      with_heading += has_heading ? 1 : 0;
      with_action += entry.number > 0 ? 1 : 0;
      items += items_before_[end_[e]] - items_before_[e];
      list.entries.push_back(std::move(entry));
    }
    const double n = static_cast<double>(list.entries.size());
    if (n > 0) {
      list.prominence =
          2 * std::log2(1 + n) + 1.5 * std::log2(1 + area / 10000) +
          3 * (static_cast<double>(with_title) / n) +
          2 * (static_cast<double>(with_fields) / n) +
          2 * (static_cast<double>(with_heading) / n) +
          2 * (static_cast<double>(with_action) / n) + (list.in_main ? 3 : 0) -
          (list.in_navigation ? 4 : 0) +
          (list.kind == ListKind::kFeed ? 1 : 0) -
          (static_cast<double>(items) / n > kMaxItemsPerEntry ? 4 : 0);
    }
    return list;
  }

  // ---- One list entry ------------------------------------------------------

  struct Run {
    uint32_t pos;
    uint32_t link;     // the nearest link (or item) around it, or kNoPos
    uint32_t heading;  // the nearest heading around it, or kNoPos
    bool in_control;   // inside a button, field or other non-link control
  };

  // Text runs inside |heading|, whatever link they are in.
  std::string HeadingRunsText(const std::vector<Run>& runs,
                              uint32_t heading,
                              size_t max_chars) const {
    std::string raw;
    for (const Run& run : runs) {
      if (run.heading != heading) {
        continue;
      }
      const std::string& name = NodeAt(run.pos).name;
      if (!raw.empty()) {
        raw.push_back(' ');
      }
      raw.append(name, 0, std::min<size_t>(name.size(), kMaxSpeechScanBytes));
      if (raw.size() > max_chars * kGatherBytesPerChar) {
        break;
      }
    }
    return SpeechSafeText(raw, max_chars);
  }

  PageListEntry MakeEntry(uint32_t root, bool* has_heading) {
    PageListEntry entry;
    const PageNode& root_node = NodeAt(root);
    entry.node = root_node.handle;
    entry.bounds = root_node.bounds;
    entry.on_screen = !root_node.Has(kStateOffscreen);

    // One pass over the entry: its actions, headings and text runs, with
    // the link, heading and control each sits in.
    const uint32_t stop = static_cast<uint32_t>(
        std::min<size_t>(end_[root], root + kMaxEntryScan));
    std::vector<uint32_t> links;    // link items
    std::vector<uint32_t> actions;  // every item
    std::vector<uint32_t> headings;
    std::vector<Run> runs;
    // Per position in [root, stop): the link, heading and control around.
    std::vector<uint32_t> link_of(stop - root, kNoPos);
    std::vector<uint32_t> heading_of(stop - root, kNoPos);
    std::vector<uint8_t> control_of(stop - root, 0);
    for (uint32_t p = root; p < stop; ++p) {
      if (!Spend(1)) {
        break;
      }
      const PageNode& node = NodeAt(p);
      const uint32_t i = p - root;
      if (p != root) {
        const uint32_t parent = order_[p].parent;
        if (parent >= root && parent < stop) {
          link_of[i] = link_of[parent - root];
          heading_of[i] = heading_of[parent - root];
          control_of[i] = control_of[parent - root];
        }
      }
      if (node.Has(kStateInvisible)) {
        continue;
      }
      if (p != root && IsListContainer(node.role)) {
        p = end_[p] - 1;  // a nested list is a list of its own
        continue;
      }
      if (node.role == PageRole::kHeading && !node.Has(kStateIgnored)) {
        headings.push_back(p);
        heading_of[i] = p;
      }
      if (item_of_[p] >= 0) {
        actions.push_back(p);
        const ItemKind kind = map_.items[item_of_[p]].kind;
        if (kind == ItemKind::kLink) {
          links.push_back(p);
          link_of[i] = p;
        } else if (kind == ItemKind::kClickable && link_of[i] == kNoPos) {
          link_of[i] = p;  // a clickable card acts as its own link
        } else {
          control_of[i] = 1;
        }
      }
      if (node.role == PageRole::kStaticText && !node.name.empty()) {
        runs.push_back({p, link_of[i], heading_of[i], control_of[i] != 0});
      }
    }
    *has_heading = !headings.empty();

    // Each link's (or clickable's) visible text, from one pass over the
    // runs; and the first heading inside a link (headings are in order).
    std::vector<std::string> owner_text(stop - root);
    for (const Run& run : runs) {
      if (run.link == kNoPos) {
        continue;
      }
      std::string& raw = owner_text[run.link - root];
      if (raw.size() > kMaxEntryTitleChars * kGatherBytesPerChar) {
        continue;
      }
      const std::string& name = NodeAt(run.pos).name;
      if (!raw.empty()) {
        raw.push_back(' ');
      }
      raw.append(name, 0, std::min<size_t>(name.size(), kMaxSpeechScanBytes));
    }
    Spend(runs.size() + links.size());
    auto visible_text = [&](uint32_t owner, size_t max_chars) {
      return SpeechSafeText(owner_text[owner - root], max_chars);
    };
    auto heading_in = [&](uint32_t link) -> uint32_t {
      auto it = std::lower_bound(headings.begin(), headings.end(), link);
      return it != headings.end() && *it < end_[link] ? *it : kNoPos;
    };

    // The primary action: the link in or around a heading, else the link
    // with the longest visible text that isn't a duration, count or date,
    // else the first action, else the entry itself.
    uint32_t primary = kNoPos;
    uint32_t primary_heading = kNoPos;
    for (uint32_t link : links) {
      const uint32_t i = link - root;
      if (heading_of[i] != kNoPos) {
        primary = link;
        break;
      }
      if (const uint32_t h = heading_in(link); h != kNoPos) {
        primary = link;
        primary_heading = h;
        break;
      }
    }
    if (primary == kNoPos) {
      size_t best = 0;
      for (uint32_t link : links) {
        const std::string text = visible_text(link, kMaxEntryTitleChars);
        const std::string& shown =
            text.empty() ? map_.items[item_of_[link]].label : text;
        const size_t length = CountCodePoints(shown);
        if (length >= 2 && length > best && !IsFieldLike(shown)) {
          best = length;
          primary = link;
        }
      }
    }
    if (primary == kNoPos && !actions.empty()) {
      primary = actions.front();
    }
    if (primary == kNoPos && item_of_[root] >= 0) {
      primary = root;
    }

    // The title: the text of the heading in the main link (a search
    // result), else the main link's visible text when the link is in a
    // heading (not a long aria-label), else the entry's heading, else the
    // link's visible text or name, else the entry's first text.
    uint32_t title_heading = kNoPos;
    if (primary != kNoPos) {
      const PageItem& item = map_.items[item_of_[primary]];
      entry.number = item.number;
      entry.kind = item.kind;
      entry.handle = item.handle;
      if (entry.bounds.empty()) {
        entry.bounds = item.bounds;
      }
      const bool link_in_heading = heading_of[primary - root] != kNoPos;
      if (primary_heading != kNoPos) {
        title_heading = primary_heading;
      } else if (!link_in_heading && !headings.empty()) {
        title_heading = headings.front();
      }
    } else if (!headings.empty()) {
      title_heading = headings.front();
    }
    if (title_heading != kNoPos) {
      entry.title = HeadingRunsText(runs, title_heading, kMaxEntryTitleChars);
      if (entry.title.empty()) {
        entry.title =
            SpeechSafeText(NodeAt(title_heading).name, kMaxEntryTitleChars);
      }
    }
    if (entry.title.empty() && primary != kNoPos) {
      entry.title = visible_text(primary, kMaxEntryTitleChars);
      if (entry.title.empty()) {
        entry.title = SpeechSafeText(map_.items[item_of_[primary]].label,
                                     kMaxEntryTitleChars);
      }
    }
    if (entry.title.empty() && !runs.empty()) {
      entry.title =
          SpeechSafeText(NodeAt(runs.front().pos).name, kMaxEntryTitleChars);
    }

    // The channel: a link to a channel, author or user page, else another
    // short link to somewhere else, else the site named in the main link.
    const std::string primary_url = primary != kNoPos && item_of_[primary] >= 0
                                        ? map_.items[item_of_[primary]].url
                                        : std::string();
    // (Another link is a channel only when the main link holds no heading:
    // a search result's other links are site links, not its source.)
    uint32_t channel_link = kNoPos;
    const int passes = primary_heading == kNoPos ? 2 : 1;
    for (int pass = 0; pass < passes && channel_link == kNoPos; ++pass) {
      // Links with visible text first ("Queen Official"), then by their
      // name alone (an avatar's "Go to channel ...").
      for (int visible = 1; visible >= 0 && channel_link == kNoPos; --visible) {
        for (uint32_t link : links) {
          if (link == primary) {
            continue;
          }
          const PageItem& item = map_.items[item_of_[link]];
          if (!primary_url.empty() && item.url == primary_url) {
            continue;  // the thumbnail: the same target as the title
          }
          if (pass == 0 && !LooksLikeChannelUrl(item.url)) {
            continue;
          }
          std::string text = visible_text(link, kMaxEntryFieldChars);
          if (!visible && text.empty()) {
            text = SpeechSafeText(item.label, kMaxEntryFieldChars);
          }
          if (text.empty() || IsFieldLike(text) || LooksLikeAddress(text) ||
              (pass == 1 && CountCodePoints(text) > 60)) {
            continue;
          }
          channel_link = link;
          entry.channel = std::move(text);
          entry.channel_number = item.number;
          break;
        }
      }
    }

    // Fields, from the text outside the title, the channel and controls.
    const std::string folded_title = FoldForCompare(entry.title);
    std::vector<std::string> seen;
    if (!entry.channel.empty()) {
      seen.push_back(FoldForCompare(entry.channel));
    }
    size_t considered = 0;
    for (const Run& run : runs) {
      if (++considered > kMaxFieldRuns || !Spend(1)) {
        break;
      }
      if (run.in_control ||
          (channel_link != kNoPos && run.link == channel_link)) {
        continue;
      }
      if ((primary != kNoPos && run.link == primary &&
           (primary_heading == kNoPos || run.heading == primary_heading)) ||
          (title_heading != kNoPos && run.heading == title_heading)) {
        continue;  // the title's own text, or the main link's
      }
      std::string text =
          SpeechSafeText(NodeAt(run.pos).name, kMaxEntryFieldChars);
      if (text.empty() || HasNoWords(text)) {
        continue;
      }
      const std::string folded = FoldForCompare(text);
      if (folded == folded_title ||
          std::find(seen.begin(), seen.end(), folded) != seen.end()) {
        continue;
      }
      // The site named in a result's main link ("Wikipedia").
      if (entry.channel.empty() && primary != kNoPos && run.link == primary &&
          !IsFieldLike(text) && !LooksLikeAddress(text)) {
        seen.push_back(folded);
        entry.channel = std::move(text);
        continue;
      }
      if (folded.size() >= 4 &&
          folded_title.find(folded) != std::string::npos) {
        continue;
      }
      seen.push_back(folded);
      if (entry.duration_seconds < 0) {
        if (std::optional<int> seconds = ParseDuration(text)) {
          entry.duration_seconds = *seconds;
          entry.duration = std::move(text);
          continue;
        }
      }
      if (entry.date.empty() && LooksLikeDate(text)) {
        entry.date = std::move(text);
        continue;
      }
      if (std::optional<ParsedCount> count = ParseCount(text)) {
        if (entry.counts.size() < kMaxEntryCounts) {
          entry.counts.push_back({count->value, count->unit, std::move(text)});
        }
        continue;
      }
      if (LooksLikeAddress(text)) {
        continue;
      }
      if (entry.texts.size() < kMaxEntryTexts) {
        entry.texts.push_back(std::move(text));
      }
    }
    return entry;
  }

  // ---- The rest ----------------------------------------------------------

  void Finish() {
    map_.title = SpeechSafeText(tree_->title, kMaxPageTitleChars);
    if (map_.title.empty()) {
      map_.title = SpeechSafeText(NodeAt(0).name, kMaxPageTitleChars);
    }
    for (const PageItem& item : map_.items) {
      if (item.focused) {
        map_.focused_item = item.number;
        break;
      }
    }
  }

  // The input outlives the builder (BuildPageMap() makes it on the stack).
  MOLT_RAW_PTR_EXCLUSION const PageTree* const tree_;
  const PageMapLimits limits_;
  // Outlives the builder too; null without hints.
  MOLT_RAW_PTR_EXCLUSION const PageMapHints* const hints_;
  PageMap map_;
  size_t work_ = 0;

  std::vector<Visit> order_;
  std::vector<uint32_t> end_;
  std::vector<uint32_t> child_start_;
  std::vector<uint32_t> child_list_;
  std::vector<int8_t> kind_;
  std::vector<int32_t> item_of_;
  std::vector<uint8_t> action_below_;
  std::vector<uint32_t> items_before_;
};

}  // namespace

double PageRect::Area() const {
  if (!(width > 0) || !(height > 0) || !std::isfinite(width) ||
      !std::isfinite(height)) {
    return 0;
  }
  const double area = static_cast<double>(width) * static_cast<double>(height);
  return std::min(area, 1e12);
}

PageNode::PageNode() = default;
PageNode::~PageNode() = default;
PageNode::PageNode(const PageNode&) = default;
PageNode& PageNode::operator=(const PageNode&) = default;
PageNode::PageNode(PageNode&&) = default;
PageNode& PageNode::operator=(PageNode&&) = default;

PageTree::PageTree() = default;
PageTree::~PageTree() = default;
PageTree::PageTree(const PageTree&) = default;
PageTree& PageTree::operator=(const PageTree&) = default;
PageTree::PageTree(PageTree&&) = default;
PageTree& PageTree::operator=(PageTree&&) = default;

PageItem::PageItem() = default;
PageItem::~PageItem() = default;
PageItem::PageItem(const PageItem&) = default;
PageItem& PageItem::operator=(const PageItem&) = default;
PageItem::PageItem(PageItem&&) = default;
PageItem& PageItem::operator=(PageItem&&) = default;

PageListEntry::PageListEntry() = default;
PageListEntry::~PageListEntry() = default;
PageListEntry::PageListEntry(const PageListEntry&) = default;
PageListEntry& PageListEntry::operator=(const PageListEntry&) = default;
PageListEntry::PageListEntry(PageListEntry&&) = default;
PageListEntry& PageListEntry::operator=(PageListEntry&&) = default;

PageList::PageList() = default;
PageList::~PageList() = default;
PageList::PageList(const PageList&) = default;
PageList& PageList::operator=(const PageList&) = default;
PageList::PageList(PageList&&) = default;
PageList& PageList::operator=(PageList&&) = default;

SiteParts::SiteParts() = default;
SiteParts::~SiteParts() = default;
SiteParts::SiteParts(const SiteParts&) = default;
SiteParts& SiteParts::operator=(const SiteParts&) = default;
SiteParts::SiteParts(SiteParts&&) = default;
SiteParts& SiteParts::operator=(SiteParts&&) = default;

PageMap::PageMap() = default;
PageMap::~PageMap() = default;
PageMap::PageMap(const PageMap&) = default;
PageMap& PageMap::operator=(const PageMap&) = default;
PageMap::PageMap(PageMap&&) = default;
PageMap& PageMap::operator=(PageMap&&) = default;

const PageItem* PageMap::ItemByNumber(int number) const {
  if (number < 1 || static_cast<size_t>(number) > items.size()) {
    return nullptr;
  }
  return &items[number - 1];
}

const PageItem* PageMap::ItemForHandle(const PageNodeHandle& handle) const {
  for (const PageItem& item : items) {
    if (item.handle == handle) {
      return &item;
    }
  }
  return nullptr;
}

std::string_view ItemKindName(ItemKind kind) {
  switch (kind) {
    case ItemKind::kLink:
      return "link";
    case ItemKind::kButton:
      return "button";
    case ItemKind::kTextField:
      return "text field";
    case ItemKind::kCheckBox:
      return "check box";
    case ItemKind::kRadio:
      return "radio button";
    case ItemKind::kSwitch:
      return "switch";
    case ItemKind::kSelect:
      return "menu";
    case ItemKind::kOption:
      return "option";
    case ItemKind::kMenuItem:
      return "menu item";
    case ItemKind::kTab:
      return "tab";
    case ItemKind::kTreeItem:
      return "tree item";
    case ItemKind::kSlider:
      return "slider";
    case ItemKind::kMedia:
      return "media";
    case ItemKind::kClickable:
      return "clickable";
  }
  return "clickable";
}

std::string_view CredentialKindName(CredentialKind kind) {
  switch (kind) {
    case CredentialKind::kNone:
      return "";
    case CredentialKind::kPassword:
      return "password";
    case CredentialKind::kPaymentCard:
      return "payment card";
    case CredentialKind::kOneTimeCode:
      return "one-time code";
  }
  return "";
}

std::string_view ListKindName(ListKind kind) {
  switch (kind) {
    case ListKind::kList:
      return "list";
    case ListKind::kFeed:
      return "feed";
    case ListKind::kTable:
      return "table";
    case ListKind::kRepeated:
      return "repeated";
  }
  return "list";
}

std::string_view LandmarkKindName(LandmarkKind kind) {
  switch (kind) {
    case LandmarkKind::kMain:
      return "main";
    case LandmarkKind::kNavigation:
      return "navigation";
    case LandmarkKind::kBanner:
      return "banner";
    case LandmarkKind::kContentInfo:
      return "content info";
    case LandmarkKind::kComplementary:
      return "complementary";
    case LandmarkKind::kSearch:
      return "search";
    case LandmarkKind::kForm:
      return "form";
    case LandmarkKind::kRegion:
      return "region";
  }
  return "region";
}

CredentialKind ClassifyCredential(const PageNode& node) {
  if (node.Has(kStateProtected)) {
    return CredentialKind::kPassword;
  }
  // As agent_safety::ClassifyField(): the type, then the autocomplete
  // tokens (a password beats a card, a card beats a one-time code).
  std::string_view type_view = node.input_type;
  while (!type_view.empty() && IsHtmlSpace(type_view.front())) {
    type_view.remove_prefix(1);
  }
  while (!type_view.empty() && IsHtmlSpace(type_view.back())) {
    type_view.remove_suffix(1);
  }
  bool password = LowerAscii(type_view, 16) == "password";
  bool card = false;
  bool otp = false;
  const std::string autocomplete =
      LowerAscii(node.autocomplete, kMaxSpeechScanBytes);
  size_t start = 0;
  for (size_t i = 0; i <= autocomplete.size(); ++i) {
    if (i < autocomplete.size() && !IsHtmlSpace(autocomplete[i])) {
      continue;
    }
    const std::string_view token =
        std::string_view(autocomplete).substr(start, i - start);
    if (token == "current-password" || token == "new-password") {
      password = true;
    } else if (token.substr(0, 3) == "cc-") {
      card = true;
    } else if (token == "one-time-code") {
      otp = true;
    }
    start = i + 1;
  }
  if (password) {
    return CredentialKind::kPassword;
  }
  if (card) {
    return CredentialKind::kPaymentCard;
  }
  if (otp) {
    return CredentialKind::kOneTimeCode;
  }
  for (const std::string* label :
       {&node.name, &node.placeholder, &node.description}) {
    const CredentialKind kind = CredentialFromLabel(*label);
    if (kind != CredentialKind::kNone) {
      return kind;
    }
  }
  return CredentialKind::kNone;
}

PageMap BuildPageMap(const PageTree& tree, const PageMapLimits& limits) {
  return Builder(tree, limits, nullptr).Build();
}

PageMap BuildPageMap(const PageTree& tree,
                     const PageMapLimits& limits,
                     const PageMapHints& hints) {
  return Builder(tree, limits, &hints).Build();
}

}  // namespace voice
}  // namespace molt_ai
