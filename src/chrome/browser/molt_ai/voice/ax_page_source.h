// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Chromium's accessibility trees as the page map's input
// (docs/design/TOUCHLESS_BROWSING.md §6.3). Every frame has a tree of its
// own; a frame's host node (the iframe) names it by its child tree id.
// BuildPageTree() walks the top-level document's tree and joins each child
// frame's tree under its host, so the result reads as one page, in reading
// order, as a screen reader would walk it.
//
// The trees come from renderers and are untrusted: the walk is bounded in
// nodes, depth and trees, visits each tree at most once (a child tree id
// that points back up is ignored), and copies at most a capped prefix of
// each string. voice_page_controller.h keeps the trees; this file only reads
// them.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_AX_PAGE_SOURCE_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_AX_PAGE_SOURCE_H_

#include <cstddef>
#include <cstdint>

#include "base/functional/function_ref.h"
#include "base/memory/raw_ptr.h"
#include "chrome/browser/molt_ai/voice/page_map.h"

namespace ui {
class AXTree;
class AXTreeID;
struct AXNodeData;
}  // namespace ui

namespace molt_ai {
namespace voice {

// A frame's tree and the index the page map's handles give it
// (PageNodeHandle::tree).
struct AXFrameTree {
  int32_t index = -1;
  raw_ptr<const ui::AXTree> tree = nullptr;
};

// Finds a frame's tree by its tree id; {-1, nullptr} if there is none.
using AXTreeLookup = base::FunctionRef<AXFrameTree(const ui::AXTreeID&)>;

struct AXPageSourceLimits {
  size_t max_nodes = 20000;
  size_t max_depth = 256;
  size_t max_trees = 64;
  // Each string attribute is cut to this many bytes (at a character
  // boundary); the page map cleans and caps it further.
  size_t max_string_bytes = 1024;
};

// The page: |main| (the top-level document's tree) with every child frame
// that |lookup| finds joined under its host node. Bounds are in |main|'s
// coordinate space (a frame's are offset by its host's position), as Blink
// reports them: physical pixels relative to the top-level viewport, not
// clipped (a node scrolled away keeps its real position, so the map's
// prominence doesn't depend on scrolling); kStateOffscreen marks a node
// outside its frame's viewport or in a frame that is. The focus is the
// focused frame's focused node.
PageTree BuildPageTree(const AXFrameTree& main,
                       AXTreeLookup lookup,
                       const AXPageSourceLimits& limits = AXPageSourceLimits());

// The page map's role for a node.
PageRole PageRoleFor(const ui::AXNodeData& data);

// A node's role, states (but kStateOffscreen and kStateFocused, which need
// the tree) and string attributes, each cut to |max_string_bytes|. No
// children and no bounds.
PageNode PageNodeFromData(const ui::AXNodeData& data,
                          int32_t tree_index,
                          size_t max_string_bytes);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_AX_PAGE_SOURCE_H_
