// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The page map (voice/page_map.h) over trees built from the input: what a
// page, through a renderer that may be compromised, puts in its
// accessibility tree. The input is a small binary format (below), so the
// seed corpus holds the unit tests' hand-built YouTube-, Google- and
// Wikipedia-like pages and forms, and the fuzzer mutates them: roles,
// states, strings (bidi, invisible and control characters, huge ones),
// bounds, child links that cycle, repeat or point nowhere, and the limits.
//
// Checks: BuildPageMap() returns (no hang: its work is capped) and gives
// the same map twice; at most max_nodes nodes are visited and max_items
// items made; items are numbered 1, 2, ... and each names a node of the
// input; every string in the map is valid UTF-8 with no control, bidi or
// invisible character (checked here with an independent decoder and
// hidden_chars), within its cap and unchanged by SpeechSafeText(); a
// protected or credential field's value is never in the map and such a
// field is never writable; the top-level document's lists come first,
// each part sorted by prominence, within their caps, and an entry's number
// names an item with the entry's node and kind; the frames' items stay
// within their share, and the page's media come before the frames'; the
// focused item is the focused node's.
//
// Format: [limits flags][title][focus][records...]
//   limits flags (1 byte): bit 0 small max_nodes (next byte + 1), bit 1
//     small max_depth (next byte), bit 2 small max_work (next 2 bytes),
//     bit 3 small max_items (next byte), bit 4 small list caps (next byte),
//     bit 5 small max_frame_items (next byte).
//   title: a string; focus: 2 bytes (a node index, 0xFFFF for none).
//   string: a length byte and the bytes; 255 then 2 bytes n and 1 byte c
//     is c repeated n times.
//   record: a tag byte. Even: a node: role, states (4 bytes), tree, parent
//     distance (2 bytes; 0 for none, else the node that many before),
//     heading level, pos/set size (1 byte each), bounds (4 x 2 bytes,
//     signed), then name, value, description, placeholder, url, html tag,
//     input type and autocomplete strings. Odd: a link: from and to (2
//     bytes each; any value, valid or not).

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "base/check.h"
#include "base/check_op.h"
#include "chrome/browser/molt_ai/agents/hidden_chars.h"
#include "chrome/browser/molt_ai/voice/page_map.h"
#include "chrome/browser/molt_ai/voice/page_map_text.h"

namespace {

using molt_ai::voice::BuildPageMap;
using molt_ai::voice::CountCodePoints;
using molt_ai::voice::CredentialKind;
using molt_ai::voice::PageItem;
using molt_ai::voice::PageList;
using molt_ai::voice::PageListEntry;
using molt_ai::voice::PageMap;
using molt_ai::voice::PageMapLimits;
using molt_ai::voice::PageNode;
using molt_ai::voice::PageNodeHandle;
using molt_ai::voice::PageRole;
using molt_ai::voice::PageTree;
using molt_ai::voice::SpeechSafeText;

constexpr uint8_t kRoleCount = static_cast<uint8_t>(PageRole::kTime) + 1;
constexpr size_t kMaxFuzzNodes = 4000;

class Reader {
 public:
  Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  bool empty() const { return at_ >= size_; }
  uint8_t U8() { return at_ < size_ ? data_[at_++] : 0; }
  uint16_t U16() {
    const uint16_t lo = U8();
    return static_cast<uint16_t>(lo | (U8() << 8));
  }
  uint32_t U32() {
    const uint32_t lo = U16();
    return lo | (static_cast<uint32_t>(U16()) << 16);
  }
  std::string String() {
    const uint8_t length = U8();
    if (length == 255) {
      const uint16_t count = U16();
      return std::string(count, static_cast<char>(U8()));
    }
    std::string out;
    for (uint8_t i = 0; i < length && !empty(); ++i) {
      out.push_back(static_cast<char>(U8()));
    }
    return out;
  }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t at_ = 0;
};

// An independent check: valid UTF-8, and no character the map promises to
// leave out.
void CheckSpeakable(std::string_view s, size_t cap) {
  CHECK_LE(CountCodePoints(s), cap);
  CHECK(s.empty() || (s.front() != ' ' && s.back() != ' '));
  CHECK(s.find("  ") == std::string_view::npos);
  size_t i = 0;
  while (i < s.size()) {
    CHECK_EQ(molt_ai::hidden_chars::HiddenCharAt(s, i), 0u);
    const uint8_t b = static_cast<uint8_t>(s[i]);
    size_t n = 1;
    uint32_t cp = b;
    if (b >= 0xF0) {
      n = 4;
      cp = b & 0x07;
    } else if (b >= 0xE0) {
      n = 3;
      cp = b & 0x0F;
    } else if (b >= 0xC0) {
      n = 2;
      cp = b & 0x1F;
    } else {
      CHECK_LT(b, 0x80);  // a lone continuation byte
    }
    CHECK_LE(i + n, s.size());
    for (size_t k = 1; k < n; ++k) {
      const uint8_t c = static_cast<uint8_t>(s[i + k]);
      CHECK_EQ(c & 0xC0, 0x80);
      cp = (cp << 6) | (c & 0x3F);
    }
    // Shortest form, no surrogate, in range, no control character.
    CHECK(n == 1 || cp >= (n == 2 ? 0x80u : n == 3 ? 0x800u : 0x10000u));
    CHECK(cp < 0xD800 || cp > 0xDFFF);
    CHECK_LE(cp, 0x10FFFFu);
    CHECK(cp >= 0x20 && cp != 0x7F);
    i += n;
  }
  CHECK_EQ(SpeechSafeText(s, cap), std::string(s));
}

bool SameHandle(const PageNodeHandle& a, const PageNodeHandle& b) {
  return a == b;
}

void CheckMap(const PageTree& tree,
              const PageMapLimits& limits,
              const PageMap& map) {
  CHECK_LE(map.stats.nodes_visited, limits.max_nodes);
  CHECK_LE(map.stats.nodes_visited, tree.nodes.size());
  CHECK_LE(map.items.size(), limits.max_items);
  CheckSpeakable(map.title, molt_ai::voice::kMaxPageTitleChars);
  CHECK_EQ(map.page_tree, tree.nodes.empty() ? -1 : tree.nodes[0].handle.tree);
  size_t frame_items = 0;
  for (const PageItem& item : map.items) {
    frame_items += map.InPage(item.handle) ? 0 : 1;
  }
  CHECK_LE(frame_items, limits.max_frame_items);

  for (size_t i = 0; i < map.items.size(); ++i) {
    const PageItem& item = map.items[i];
    CHECK_EQ(item.number, static_cast<int>(i + 1));
    CheckSpeakable(item.label, molt_ai::voice::kMaxItemLabelChars);
    CheckSpeakable(item.value, molt_ai::voice::kMaxItemValueChars);
    CHECK_LE(item.url.size(), molt_ai::voice::kMaxLinkUrlBytes);
    // The node exists in the input (node ids here are index + 1), and a
    // secret's value is not kept.
    CHECK_GE(item.handle.node, 1);
    CHECK_LE(static_cast<size_t>(item.handle.node), tree.nodes.size());
    const PageNode* node = &tree.nodes[item.handle.node - 1];
    CHECK(SameHandle(node->handle, item.handle));
    if (node->Has(molt_ai::voice::kStateProtected)) {
      CHECK(item.value.empty());
      CHECK(!item.writable);
    }
    if (item.kind == molt_ai::voice::ItemKind::kTextField &&
        molt_ai::voice::ClassifyCredential(*node) != CredentialKind::kNone) {
      CHECK(item.value.empty());
      CHECK(!item.writable);
      CHECK(item.credential != CredentialKind::kNone);
      CHECK(map.has_credential_fields);
    }
    CHECK(!node->Has(molt_ai::voice::kStateInvisible));
    CHECK(!node->Has(molt_ai::voice::kStateIgnored));
    if (item.writable) {
      CHECK(item.credential == CredentialKind::kNone);
      CHECK(item.kind == molt_ai::voice::ItemKind::kTextField);
    }
    if (item.list >= 0) {
      CHECK_LT(static_cast<size_t>(item.list), map.lists.size());
      const PageList& list = map.lists[item.list];
      CHECK_LT(static_cast<size_t>(item.entry), list.entries.size());
      CHECK_EQ(list.entries[item.entry].number, item.number);
    }
  }
  if (map.focused_item > 0) {
    CHECK(map.ItemByNumber(map.focused_item)->focused);
  }

  CHECK_LE(map.lists.size(), limits.max_lists);
  for (size_t l = 0; l < map.lists.size(); ++l) {
    const PageList& list = map.lists[l];
    if (l > 0) {
      const PageList& before = map.lists[l - 1];
      const bool in_page = map.InPage(list.container);
      CHECK(map.InPage(before.container) || !in_page);
      if (map.InPage(before.container) == in_page) {
        CHECK_GE(before.prominence, list.prominence);
      }
    }
    CheckSpeakable(list.label, molt_ai::voice::kMaxItemLabelChars);
    CHECK_LE(list.entries.size(), limits.max_entries_per_list);
    for (size_t e = 0; e < list.entries.size(); ++e) {
      const PageListEntry& entry = list.entries[e];
      CHECK_EQ(entry.position, static_cast<int>(e + 1));
      CheckSpeakable(entry.title, molt_ai::voice::kMaxEntryTitleChars);
      CheckSpeakable(entry.channel, molt_ai::voice::kMaxEntryFieldChars);
      CheckSpeakable(entry.duration, molt_ai::voice::kMaxEntryFieldChars);
      CheckSpeakable(entry.date, molt_ai::voice::kMaxEntryFieldChars);
      CHECK_LE(entry.texts.size(), molt_ai::voice::kMaxEntryTexts);
      for (const std::string& text : entry.texts) {
        CheckSpeakable(text, molt_ai::voice::kMaxEntryFieldChars);
      }
      CHECK_LE(entry.counts.size(), molt_ai::voice::kMaxEntryCounts);
      for (const auto& count : entry.counts) {
        CHECK_GE(count.value, 0);
        CheckSpeakable(count.text, molt_ai::voice::kMaxEntryFieldChars);
      }
      CHECK(entry.duration_seconds >= -1);
      CHECK_EQ(entry.duration_seconds >= 0, !entry.duration.empty());
      if (entry.number > 0) {
        const PageItem* item = map.ItemByNumber(entry.number);
        CHECK(item);
        CHECK(SameHandle(item->handle, entry.handle));
        CHECK(item->kind == entry.kind);
      }
      if (entry.channel_number > 0) {
        CHECK(map.ItemByNumber(entry.channel_number));
      }
    }
  }
  CHECK_LE(map.headings.size(), limits.max_headings);
  for (const auto& heading : map.headings) {
    CHECK(heading.level >= 1 && heading.level <= 6);
    CheckSpeakable(heading.text, molt_ai::voice::kMaxHeadingChars);
  }
  CHECK_LE(map.landmarks.size(), limits.max_landmarks);
  for (const auto& landmark : map.landmarks) {
    CheckSpeakable(landmark.label, molt_ai::voice::kMaxItemLabelChars);
  }
  CHECK_LE(map.media.size(), limits.max_media);
  for (size_t m = 0; m < map.media.size(); ++m) {
    const auto& media = map.media[m];
    const PageItem* item = map.ItemByNumber(media.number);
    CHECK(item);
    CHECK(item->kind == molt_ai::voice::ItemKind::kMedia);
    if (m > 0) {
      CHECK(map.InPage(map.media[m - 1].handle) || !map.InPage(media.handle));
    }
  }
}

void CheckSameMap(const PageMap& a, const PageMap& b) {
  CHECK_EQ(a.items.size(), b.items.size());
  for (size_t i = 0; i < a.items.size(); ++i) {
    CHECK(SameHandle(a.items[i].handle, b.items[i].handle));
    CHECK_EQ(a.items[i].label, b.items[i].label);
  }
  CHECK_EQ(a.lists.size(), b.lists.size());
  for (size_t l = 0; l < a.lists.size(); ++l) {
    CHECK_EQ(a.lists[l].entries.size(), b.lists[l].entries.size());
    for (size_t e = 0; e < a.lists[l].entries.size(); ++e) {
      CHECK_EQ(a.lists[l].entries[e].title, b.lists[l].entries[e].title);
    }
  }
  CHECK_EQ(a.title, b.title);
  CHECK_EQ(a.focused_item, b.focused_item);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  Reader in(data, size);
  PageMapLimits limits;
  const uint8_t flags = in.U8();
  if (flags & 1) {
    limits.max_nodes = 1 + in.U8();
  }
  if (flags & 2) {
    limits.max_depth = in.U8();
  }
  if (flags & 4) {
    limits.max_work = in.U16();
  }
  if (flags & 8) {
    limits.max_items = in.U8();
  }
  if (flags & 16) {
    const uint8_t cap = in.U8();
    limits.max_lists = cap & 15;
    limits.max_entries_per_list = cap >> 4;
  }
  if (flags & 32) {
    limits.max_frame_items = in.U8();
  }

  PageTree tree;
  tree.title = in.String();
  const uint16_t focus = in.U16();
  while (!in.empty() && tree.nodes.size() < kMaxFuzzNodes) {
    const uint8_t tag = in.U8();
    if (tag & 1) {
      const uint16_t from = in.U16();
      const uint16_t to = in.U16();
      if (from < tree.nodes.size()) {
        tree.nodes[from].children.push_back(to);
      }
      continue;
    }
    PageNode node;
    const uint32_t index = static_cast<uint32_t>(tree.nodes.size());
    node.role = static_cast<PageRole>(in.U8() % kRoleCount);
    node.states = in.U32();
    node.handle.tree = in.U8() & 7;
    node.handle.node = static_cast<int32_t>(index + 1);
    const uint16_t back = in.U16();
    node.heading_level = in.U8();
    node.pos_in_set = in.U8();
    node.set_size = in.U8();
    node.bounds.x = static_cast<int16_t>(in.U16());
    node.bounds.y = static_cast<int16_t>(in.U16());
    node.bounds.width = static_cast<int16_t>(in.U16());
    node.bounds.height = static_cast<int16_t>(in.U16());
    node.name = in.String();
    node.value = in.String();
    node.description = in.String();
    node.placeholder = in.String();
    node.url = in.String();
    node.html_tag = in.String();
    node.input_type = in.String();
    node.autocomplete = in.String();
    tree.nodes.push_back(std::move(node));
    if (back > 0 && back <= index) {
      tree.nodes[index - back].children.push_back(index);
    }
  }
  if (focus < tree.nodes.size()) {
    tree.focus = tree.nodes[focus].handle;
  }

  const PageMap map = BuildPageMap(tree, limits);
  CheckMap(tree, limits, map);
  CheckSameMap(map, BuildPageMap(tree, limits));
  return 0;
}
