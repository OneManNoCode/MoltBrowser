// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/page_reading.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <utility>

#include "chrome/browser/molt_ai/voice/page_map_text.h"
#include "chrome/browser/molt_ai/voice/spoken_choice.h"
#include "chrome/services/molt_ai/inference/engine/raw_ptr_exclusion.h"

namespace molt_ai {
namespace voice {

namespace {

constexpr uint32_t kNoPos = std::numeric_limits<uint32_t>::max();
// A block's raw text is cut into pieces of at most this many bytes, each a
// block of its own (SpeechSafeText() reads 8 KiB at most; this many bytes
// are at most kMaxReadingBlockChars code points).
constexpr size_t kMaxBlockBytes = kMaxReadingBlockChars;
// Speech rate 1 says about this many code points a second (Piper's
// ljspeech voice and the platforms' default voices), and a chunk is about
// this long.
constexpr double kCharsPerSecond = 15;
constexpr double kChunkSeconds = 13;
constexpr size_t kMinChunkChars = 80;
constexpr size_t kMaxChunkChars = 300;

// Words a full stop after which doesn't end a sentence.
constexpr std::string_view kAbbreviations[] = {
    "mr", "mrs", "ms", "dr", "st", "jr", "sr", "vs", "etc", "no",
    "fig", "approx", "inc", "ltd", "co", "mt", "prof", "gen", "lt",
};

bool IsUtf8Continuation(char c) {
  return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

size_t CodePoints(std::string_view s) {
  return CountCodePoints(s);
}

// The byte offset |chars| code points into |s| (or its end).
size_t OffsetOfChars(std::string_view s, size_t chars) {
  size_t i = 0;
  size_t seen = 0;
  while (i < s.size()) {
    if (!IsUtf8Continuation(s[i])) {
      if (seen == chars) {
        return i;
      }
      ++seen;
    }
    ++i;
  }
  return s.size();
}

char LowerAscii(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// Whether the full stop at |dot| in |text| ends a sentence: not after an
// abbreviation or a single letter ("U.S.", "J. R. R.").
bool EndsSentence(std::string_view text, size_t dot) {
  if (text[dot] != '.') {
    return true;  // ! ? and the ellipsis
  }
  size_t start = dot;
  while (start > 0 && text[start - 1] != ' ') {
    --start;
  }
  std::string word;
  for (size_t i = start; i < dot && word.size() < 8; ++i) {
    word.push_back(LowerAscii(text[i]));
  }
  // A single letter ("J. R. R.") or dotted letters ("U.S.", "e.g.").
  if (word.size() <= 1 || word.find('.') != std::string::npos) {
    return false;
  }
  return std::find(std::begin(kAbbreviations), std::end(kAbbreviations),
                   word) == std::end(kAbbreviations);
}

// Where |text|'s sentences start: after ". ", "! ", "? " or "… " followed
// by what can start a sentence; a sentence longer than
// kMaxReadingSentenceChars is cut at its last ", " or space before that.
std::vector<uint32_t> SentenceStarts(std::string_view text) {
  std::vector<uint32_t> starts = {0};
  auto starts_sentence = [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '"' ||
           c == '\'' || c == '(' || c == '[' ||
           static_cast<unsigned char>(c) >= 0x80;
  };
  for (size_t i = 0; i + 2 < text.size(); ++i) {
    const bool stop = text[i] == '.' || text[i] == '!' || text[i] == '?';
    // U+2026 HORIZONTAL ELLIPSIS ends at its last byte.
    const bool ellipsis = i >= 2 && text.substr(i - 2, 3) == "\xE2\x80\xA6";
    if ((stop || ellipsis) && text[i + 1] == ' ' &&
        starts_sentence(text[i + 2]) && (ellipsis || EndsSentence(text, i))) {
      starts.push_back(static_cast<uint32_t>(i + 2));
    }
  }
  // Sentences too long to say in one go are cut.
  std::vector<uint32_t> out;
  for (size_t k = 0; k < starts.size(); ++k) {
    size_t begin = starts[k];
    const size_t end = k + 1 < starts.size() ? starts[k + 1] : text.size();
    out.push_back(static_cast<uint32_t>(begin));
    while (CodePoints(text.substr(begin, end - begin)) >
           kMaxReadingSentenceChars) {
      const size_t limit =
          begin + OffsetOfChars(text.substr(begin), kMaxReadingSentenceChars);
      const std::string_view head = text.substr(begin, limit - begin);
      size_t cut = head.rfind(", ");
      if (cut == std::string_view::npos || cut < head.size() / 2) {
        cut = head.rfind(' ');
      } else {
        ++cut;  // after the comma
      }
      if (cut == std::string_view::npos || cut == 0) {
        cut = head.size();  // no space: cut at the cap
      } else {
        ++cut;  // after the space
      }
      begin += cut;
      if (begin >= end) {
        break;
      }
      out.push_back(static_cast<uint32_t>(begin));
    }
  }
  return out;
}

// The top-level document of a tree, walked once in reading order, bounded
// (each node once, at most max_nodes, at most max_depth deep); frames'
// trees are left out with everything under them.
class Walk {
 public:
  Walk(const PageTree& tree, const ReadingLimits& limits) : tree_(&tree) {
    const std::vector<PageNode>& nodes = tree.nodes;
    if (nodes.empty()) {
      return;
    }
    const int32_t page = nodes[0].handle.tree;
    std::vector<uint8_t> seen(nodes.size(), 0);
    struct Pending {
      uint32_t node;
      uint32_t parent;
      uint32_t depth;
    };
    std::vector<Pending> stack = {{0, kNoPos, 0}};
    const size_t max_pending = limits.max_nodes * 2 + 16;
    while (!stack.empty() && order_.size() < limits.max_nodes) {
      const Pending next = stack.back();
      stack.pop_back();
      if (seen[next.node]) {
        continue;
      }
      seen[next.node] = 1;
      order_.push_back(next.node);
      parent_.push_back(next.parent);
      const uint32_t pos = static_cast<uint32_t>(order_.size() - 1);
      const PageNode& node = nodes[next.node];
      for (size_t k = node.children.size(); k-- > 0;) {
        const uint32_t child = node.children[k];
        if (child >= nodes.size() || seen[child] ||
            nodes[child].handle.tree != page ||
            next.depth + 1 > limits.max_depth || stack.size() >= max_pending) {
          continue;
        }
        stack.push_back({child, pos, next.depth + 1});
      }
    }
    capped_ = !stack.empty();
    const size_t n = order_.size();
    end_.resize(n);
    for (size_t p = 0; p < n; ++p) {
      end_[p] = static_cast<uint32_t>(p + 1);
    }
    for (size_t p = n; p-- > 1;) {
      end_[parent_[p]] = std::max(end_[parent_[p]], end_[p]);
    }
  }

  uint32_t size() const { return static_cast<uint32_t>(order_.size()); }
  const PageNode& node(uint32_t pos) const {
    return tree_->nodes[order_[pos]];
  }
  uint32_t parent(uint32_t pos) const { return parent_[pos]; }
  uint32_t end(uint32_t pos) const { return end_[pos]; }
  bool capped() const { return capped_; }

  uint32_t Find(const PageNodeHandle& handle) const {
    if (!handle.valid()) {
      return kNoPos;
    }
    for (uint32_t p = 0; p < size(); ++p) {
      if (node(p).handle == handle) {
        return p;
      }
    }
    return kNoPos;
  }

 private:
  // Outlives the walk (made on the stack of the functions below).
  MOLT_RAW_PTR_EXCLUSION const PageTree* tree_;
  std::vector<uint32_t> order_;
  std::vector<uint32_t> parent_;
  std::vector<uint32_t> end_;
  bool capped_ = false;
};

bool IsBlockTag(std::string_view tag) {
  for (std::string_view block :
       {"div", "section", "blockquote", "pre", "figcaption", "figure", "dd",
        "dt", "dl", "address", "details", "summary", "center", "p", "li",
        "header", "footer", "article", "aside", "main", "nav", "caption",
        "legend", "fieldset", "body"}) {
    if (tag == block) {
      return true;
    }
  }
  return false;
}

// Whether text under |node| belongs to it as a block of its own (rather
// than flowing into its parent's).
bool IsBlock(const PageNode& node) {
  switch (node.role) {
    case PageRole::kRootWebArea:
    case PageRole::kHeading:
    case PageRole::kParagraph:
    case PageRole::kListItem:
    case PageRole::kArticle:
    case PageRole::kMain:
    case PageRole::kRegion:
    case PageRole::kBanner:
    case PageRole::kContentInfo:
    case PageRole::kComplementary:
    case PageRole::kList:
    case PageRole::kFeed:
    case PageRole::kNavigation:
    case PageRole::kSearch:
    case PageRole::kForm:
    case PageRole::kDialog:
      return true;
    case PageRole::kGeneric:
      return !node.Has(kStateIgnored) && IsBlockTag(node.html_tag);
    default:
      return false;
  }
}

// Whether a subtree isn't read: controls, media, tables, frames, menus and
// dialogs anywhere; navigation and search anywhere; and the page's banner,
// footer and asides when the whole page is read.
bool LeftOut(const PageNode& node, bool whole_page) {
  if (node.Has(kStateInvisible)) {
    return true;
  }
  switch (node.role) {
    case PageRole::kNavigation:
    case PageRole::kSearch:
    case PageRole::kForm:
    case PageRole::kMenu:
    case PageRole::kDialog:
    case PageRole::kTable:
    case PageRole::kGrid:
    case PageRole::kListBox:
    case PageRole::kButton:
    case PageRole::kTextField:
    case PageRole::kSearchBox:
    case PageRole::kTextArea:
    case PageRole::kComboBox:
    case PageRole::kSelect:
    case PageRole::kCheckBox:
    case PageRole::kRadio:
    case PageRole::kSwitch:
    case PageRole::kSlider:
    case PageRole::kOption:
    case PageRole::kMenuItem:
    case PageRole::kTab:
    case PageRole::kTreeItem:
    case PageRole::kImage:
    case PageRole::kVideo:
    case PageRole::kAudio:
    case PageRole::kIframe:
      return true;
    case PageRole::kBanner:
    case PageRole::kContentInfo:
    case PageRole::kComplementary:
      return whole_page;
    default:
      return false;
  }
}

ReadingBlockKind KindOf(const PageNode& node) {
  switch (node.role) {
    case PageRole::kHeading:
      return ReadingBlockKind::kHeading;
    case PageRole::kParagraph:
      return ReadingBlockKind::kParagraph;
    case PageRole::kListItem:
      return ReadingBlockKind::kListItem;
    default:
      return ReadingBlockKind::kOther;
  }
}

// Joins a piece of text to what a block has: a space between, unless one
// side has one or the piece starts with punctuation that follows a word.
void AppendPiece(std::string_view piece, std::string* raw) {
  if (piece.empty()) {
    return;
  }
  if (!raw->empty() && raw->back() != ' ' && piece.front() != ' ') {
    const char c = piece.front();
    if (c != '.' && c != ',' && c != ';' && c != ':' && c != '!' &&
        c != '?' && c != ')' && c != ']' && c != '%' && c != '\'') {
      raw->push_back(' ');
    }
  }
  raw->append(piece);
}

// The block being gathered.
struct Gathering {
  uint32_t block_pos = kNoPos;
  std::string raw;
  PageNodeHandle first_link;
};

}  // namespace

ReadingBlock::ReadingBlock() = default;
ReadingBlock::~ReadingBlock() = default;
ReadingBlock::ReadingBlock(const ReadingBlock&) = default;
ReadingBlock& ReadingBlock::operator=(const ReadingBlock&) = default;
ReadingBlock::ReadingBlock(ReadingBlock&&) = default;
ReadingBlock& ReadingBlock::operator=(ReadingBlock&&) = default;

ReadingDocument::ReadingDocument() = default;
ReadingDocument::~ReadingDocument() = default;
ReadingDocument::ReadingDocument(const ReadingDocument&) = default;
ReadingDocument& ReadingDocument::operator=(const ReadingDocument&) = default;
ReadingDocument::ReadingDocument(ReadingDocument&&) = default;
ReadingDocument& ReadingDocument::operator=(ReadingDocument&&) = default;

ReadingOptions::ReadingOptions() = default;
ReadingOptions::~ReadingOptions() = default;
ReadingOptions::ReadingOptions(const ReadingOptions&) = default;
ReadingOptions& ReadingOptions::operator=(const ReadingOptions&) = default;
ReadingOptions::ReadingOptions(ReadingOptions&&) = default;
ReadingOptions& ReadingOptions::operator=(ReadingOptions&&) = default;

ReadingChunk::ReadingChunk() = default;
ReadingChunk::~ReadingChunk() = default;
ReadingChunk::ReadingChunk(const ReadingChunk&) = default;
ReadingChunk& ReadingChunk::operator=(const ReadingChunk&) = default;
ReadingChunk::ReadingChunk(ReadingChunk&&) = default;
ReadingChunk& ReadingChunk::operator=(ReadingChunk&&) = default;

std::string_view ReadingSourceName(ReadingSource source) {
  switch (source) {
    case ReadingSource::kSkill:
      return "skill";
    case ReadingSource::kMain:
      return "main";
    case ReadingSource::kArticle:
      return "article";
    case ReadingSource::kPage:
      return "page";
  }
  return "page";
}

ReadingDocument BuildReadingDocument(const PageTree& tree,
                                     const ReadingOptions& options,
                                     const ReadingLimits& limits) {
  ReadingDocument document;
  document.title = SpeechSafeText(tree.title, kMaxPageTitleChars);
  const Walk walk(tree, limits);
  if (walk.size() == 0) {
    return document;
  }
  document.capped = walk.capped();

  // The root: the skill's, the main landmark, the first article, the page.
  uint32_t root = walk.Find(options.root);
  if (root != kNoPos && !walk.node(root).Has(kStateInvisible)) {
    document.source = ReadingSource::kSkill;
  } else {
    root = kNoPos;
    for (const PageRole role : {PageRole::kMain, PageRole::kArticle}) {
      for (uint32_t p = 0; p < walk.size() && root == kNoPos; ++p) {
        const PageNode& node = walk.node(p);
        if (node.role == role && !node.Has(kStateInvisible) &&
            !node.Has(kStateIgnored)) {
          root = p;
          document.source = role == PageRole::kMain ? ReadingSource::kMain
                                                     : ReadingSource::kArticle;
        }
      }
    }
    if (root == kNoPos) {
      root = 0;
      document.source = ReadingSource::kPage;
    }
  }
  const bool whole_page = root == 0;

  // The skill's subtrees left out, and the node to locate.
  std::unordered_set<int64_t> skip;
  auto key = [](const PageNodeHandle& h) {
    return (static_cast<int64_t>(h.tree) << 32) ^ static_cast<uint32_t>(h.node);
  };
  for (const PageNodeHandle& handle : options.skip) {
    skip.insert(key(handle));
  }
  const uint32_t locate = walk.Find(options.locate);

  // Per position: the block its text belongs to, and the link it is in.
  const uint32_t stop = walk.end(root);
  std::vector<uint32_t> block_of(walk.size(), kNoPos);
  std::vector<uint32_t> link_of(walk.size(), kNoPos);
  size_t bytes = 0;
  Gathering gathering;
  auto flush = [&]() {
    if (gathering.block_pos == kNoPos || gathering.raw.empty()) {
      gathering.raw.clear();
      return;
    }
    const PageNode& node = walk.node(gathering.block_pos);
    std::string_view rest = gathering.raw;
    bool first = true;
    while (!rest.empty()) {
      size_t take = rest.size();
      if (take > kMaxBlockBytes) {
        take = rest.substr(0, kMaxBlockBytes).rfind(' ');
        if (take == std::string_view::npos || take == 0) {
          take = kMaxBlockBytes;
          while (take > 0 && IsUtf8Continuation(rest[take])) {
            --take;
          }
        }
      }
      ReadingBlock block;
      block.text = SpeechSafeText(rest.substr(0, take), kMaxReadingBlockChars);
      rest.remove_prefix(take);
      if (block.text.empty() || HasNoWords(block.text)) {
        continue;
      }
      if (document.blocks.size() >= limits.max_blocks ||
          bytes + block.text.size() > limits.max_bytes) {
        document.capped = true;
        break;
      }
      block.kind = KindOf(node);
      block.level = block.kind == ReadingBlockKind::kHeading
                        ? std::clamp(node.heading_level, 1, 6)
                        : 0;
      block.node = node.handle;
      if (first) {
        block.first_link = gathering.first_link;
        first = false;
      }
      block.sentences = SentenceStarts(block.text);
      bytes += block.text.size();
      document.blocks.push_back(std::move(block));
    }
    gathering.raw.clear();
    gathering.first_link = PageNodeHandle();
  };

  for (uint32_t p = root; p < stop; ++p) {
    const PageNode& node = walk.node(p);
    if (p != root && (LeftOut(node, whole_page) || skip.count(key(node.handle)))) {
      if (locate != kNoPos && locate >= p && locate < walk.end(p) &&
          document.located < 0) {
        // The node to locate isn't read: the block after it.
        flush();
        document.located = static_cast<int>(document.blocks.size());
      }
      p = walk.end(p) - 1;
      continue;
    }
    const uint32_t parent = walk.parent(p);
    const bool inherit = p != root && parent != kNoPos && parent >= root;
    block_of[p] = IsBlock(node) || !inherit ? p : block_of[parent];
    link_of[p] = node.role == PageRole::kLink ? p
                 : inherit && !IsBlock(node) ? link_of[parent]
                                             : kNoPos;
    if (p == locate && document.located < 0) {
      flush();
      document.located = static_cast<int>(document.blocks.size());
    }
    if (node.role != PageRole::kStaticText || node.name.empty() ||
        node.Has(kStateIgnored)) {
      continue;
    }
    if (block_of[p] != gathering.block_pos) {
      flush();
      gathering.block_pos = block_of[p];
    }
    if (gathering.raw.size() > limits.max_bytes) {
      continue;
    }
    if (link_of[p] != kNoPos && !gathering.first_link.valid()) {
      gathering.first_link = walk.node(link_of[p]).handle;
    }
    AppendPiece(node.name, &gathering.raw);
    if (document.capped && document.blocks.size() >= limits.max_blocks) {
      break;
    }
  }
  flush();
  // A node before the text (in the page's header): its first block.
  if (document.located < 0 && locate != kNoPos && locate < root &&
      !document.blocks.empty()) {
    document.located = 0;
  }
  if (document.located >= static_cast<int>(document.blocks.size())) {
    document.located = document.blocks.empty()
                           ? -1
                           : static_cast<int>(document.blocks.size()) - 1;
  }
  return document;
}

std::optional<std::string> SelectedText(const PageTree& tree,
                                        size_t max_chars,
                                        const ReadingLimits& limits) {
  const PageSelection& selection = tree.selection;
  if (!selection.valid()) {
    return std::nullopt;
  }
  const Walk walk(tree, limits);
  uint32_t start = walk.Find(selection.anchor);
  uint32_t end = walk.Find(selection.focus);
  if (start == kNoPos || end == kNoPos) {
    return std::nullopt;
  }
  size_t start_offset = static_cast<size_t>(selection.anchor_offset);
  size_t end_offset = static_cast<size_t>(selection.focus_offset);
  if (start > end || (start == end && start_offset > end_offset)) {
    std::swap(start, end);
    std::swap(start_offset, end_offset);
  }
  // UTF-16 offsets into UTF-8 text, as bytes (clamped to the text).
  auto byte_offset = [](std::string_view text, size_t units) {
    size_t i = 0;
    while (i < text.size() && units > 0) {
      const unsigned char c = static_cast<unsigned char>(text[i]);
      const size_t length = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
      units -= std::min<size_t>(units, length == 4 ? 2 : 1);
      i = std::min(text.size(), i + length);
    }
    return i;
  };
  // Inside one text field: its value, never a credential field's.
  const PageNode& first = walk.node(start);
  if (start == end && (first.role == PageRole::kTextField ||
                       first.role == PageRole::kSearchBox ||
                       first.role == PageRole::kTextArea ||
                       first.role == PageRole::kComboBox)) {
    if (ClassifyCredential(first) != CredentialKind::kNone) {
      return std::nullopt;
    }
    const size_t from = byte_offset(first.value, start_offset);
    const size_t to = byte_offset(first.value, end_offset);
    const std::string text = SpeechSafeText(
        std::string_view(first.value).substr(from, to - from), max_chars);
    return text.empty() ? std::nullopt : std::optional<std::string>(text);
  }
  // Across nodes: the static text between the two ends. An end on an
  // element is a child index: the selection starts at that child's
  // subtree, or ends before it.
  auto child_pos = [&](uint32_t pos, size_t index, bool* past) {
    size_t k = 0;
    for (uint32_t c = pos + 1; c < walk.end(pos); c = walk.end(c)) {
      if (k++ == index) {
        return c;
      }
    }
    *past = true;
    return walk.end(pos);
  };
  uint32_t from = start;
  uint32_t to = end;
  bool from_text = walk.node(start).role == PageRole::kStaticText;
  bool to_text = walk.node(end).role == PageRole::kStaticText;
  if (!from_text) {
    bool past = false;
    from = child_pos(start, start_offset, &past);
  }
  if (!to_text) {
    bool past = false;
    to = child_pos(end, end_offset, &past);
  } else {
    to = end + 1;
  }
  std::string raw;
  const size_t max_bytes = max_chars * 4 + 16;
  for (uint32_t p = from; p < to && p < walk.size() && raw.size() < max_bytes;
       ++p) {
    const PageNode& node = walk.node(p);
    if (node.Has(kStateInvisible) ||
        ClassifyCredential(node) != CredentialKind::kNone) {
      p = walk.end(p) - 1;
      continue;
    }
    if (node.role != PageRole::kStaticText) {
      continue;
    }
    std::string_view text = node.name;
    if (p == end && to_text) {
      text = text.substr(0, byte_offset(text, end_offset));
    }
    if (p == start && from_text) {
      text = text.substr(byte_offset(text, start_offset));
    }
    AppendPiece(text, &raw);
  }
  const std::string text = SpeechSafeText(raw, max_chars);
  return text.empty() ? std::nullopt : std::optional<std::string>(text);
}

size_t ReadingChunkChars(double rate) {
  if (!std::isfinite(rate) || rate <= 0) {
    rate = 1;
  }
  const double chars = kCharsPerSecond * kChunkSeconds * rate;
  return std::clamp(static_cast<size_t>(std::min(chars, 1e6)), kMinChunkChars,
                    kMaxChunkChars);
}

std::optional<ReadingChunk> ChunkAt(const ReadingDocument& document,
                                    ReadingPosition at,
                                    size_t max_chars,
                                    size_t end_block) {
  max_chars = std::max<size_t>(max_chars, 1);
  end_block = std::min(end_block, document.blocks.size());
  while (at.block < end_block &&
         at.offset >= document.blocks[at.block].text.size()) {
    at = {at.block + 1, 0};
  }
  if (at.block >= end_block) {
    return std::nullopt;
  }
  const ReadingBlock& block = document.blocks[at.block];
  const std::string_view text = block.text;
  // The sentences from |at|: whole ones while they fit.
  auto starts = std::upper_bound(block.sentences.begin(),
                                 block.sentences.end(),
                                 static_cast<uint32_t>(at.offset));
  size_t end = at.offset;
  for (auto it = starts; it != block.sentences.end() || end < text.size();) {
    const size_t next = it != block.sentences.end() ? *it : text.size();
    if (end > at.offset &&
        CodePoints(text.substr(at.offset, next - at.offset)) > max_chars) {
      break;
    }
    end = next;
    if (it == block.sentences.end()) {
      break;
    }
    ++it;
  }
  // One sentence too long: cut at a word.
  if (CodePoints(text.substr(at.offset, end - at.offset)) > max_chars) {
    const size_t limit =
        at.offset + OffsetOfChars(text.substr(at.offset), max_chars);
    const size_t space = text.substr(at.offset, limit - at.offset).rfind(' ');
    end = space == std::string_view::npos || space == 0 ? limit
                                                        : at.offset + space + 1;
  }
  ReadingChunk chunk;
  chunk.at = at;
  chunk.next = end >= text.size() ? ReadingPosition{at.block + 1, 0}
                                  : ReadingPosition{at.block, end};
  std::string said(text.substr(at.offset, end - at.offset));
  while (!said.empty() && said.back() == ' ') {
    said.pop_back();
  }
  // A heading or list item ends with a pause.
  if (end >= text.size() && block.kind != ReadingBlockKind::kParagraph &&
      !said.empty()) {
    const char last = said.back();
    if (last != '.' && last != '!' && last != '?' && last != ':' &&
        last != ';') {
      said.push_back('.');
    }
  }
  chunk.text = std::move(said);
  return chunk;
}

std::optional<size_t> MoveBlock(const ReadingDocument& document,
                                size_t from,
                                ReadingUnit unit,
                                bool forward) {
  const size_t n = document.blocks.size();
  auto fits = [&](size_t b) {
    const bool heading =
        document.blocks[b].kind == ReadingBlockKind::kHeading;
    return unit == ReadingUnit::kHeading ? heading : !heading;
  };
  if (forward) {
    // From "nowhere" (past the end as the starting point), the first.
    for (size_t b = from >= n ? 0 : from + 1; b < n; ++b) {
      if (fits(b)) {
        return b;
      }
    }
    return std::nullopt;
  }
  for (size_t b = std::min(from, n); b-- > 0;) {
    if (fits(b)) {
      return b;
    }
  }
  return std::nullopt;
}

std::optional<size_t> FindSection(const ReadingDocument& document,
                                  std::string_view spoken,
                                  std::vector<size_t>* close) {
  if (close) {
    close->clear();
  }
  std::vector<ChoiceCandidate> candidates;
  std::vector<size_t> blocks;
  for (size_t b = 0; b < document.blocks.size() &&
                     candidates.size() < kMaxChoiceCandidates;
       ++b) {
    if (document.blocks[b].kind != ReadingBlockKind::kHeading) {
      continue;
    }
    ChoiceCandidate candidate;
    candidate.position = static_cast<int>(candidates.size() + 1);
    candidate.role = ChoiceRole::kHeading;
    candidate.title = document.blocks[b].text;
    candidates.push_back(std::move(candidate));
    blocks.push_back(b);
  }
  const std::optional<ChoiceRef> ref =
      ParseChoiceRef(spoken, Language::kEnglish, /*bare_numbers=*/false);
  if (candidates.empty() || !ref) {
    return std::nullopt;
  }
  const ChoiceMatch match = MatchSpokenChoice(*ref, candidates, MatchOptions());
  if (match.outcome == ChoiceOutcome::kUnique) {
    return blocks[match.index];
  }
  if (match.outcome == ChoiceOutcome::kAmbiguous && close) {
    for (size_t i : match.close) {
      close->push_back(blocks[i]);
    }
  }
  return std::nullopt;
}

size_t SectionEnd(const ReadingDocument& document, size_t heading) {
  const size_t n = document.blocks.size();
  if (heading >= n) {
    return n;
  }
  const int level = document.blocks[heading].level;
  for (size_t b = heading + 1; b < n; ++b) {
    const ReadingBlock& block = document.blocks[b];
    if (block.kind == ReadingBlockKind::kHeading && block.level <= level) {
      return b;
    }
  }
  return n;
}

std::string MoveSpeech(const ReadingDocument& document, size_t block) {
  if (block >= document.blocks.size()) {
    return std::string();
  }
  const ReadingBlock& b = document.blocks[block];
  if (b.kind == ReadingBlockKind::kHeading) {
    return "Heading level " + std::to_string(b.level) + ": " + b.text + ".";
  }
  const std::optional<ReadingChunk> chunk =
      ChunkAt(document, {block, 0}, kMaxChunkChars, block + 1);
  return chunk ? chunk->text : std::string();
}

ReadingQueue::ReadingQueue() = default;
ReadingQueue::~ReadingQueue() = default;

void ReadingQueue::Start(ReadingDocument document,
                         ReadingPosition from,
                         size_t end_block) {
  document_ = std::move(document);
  loaded_ = true;
  end_block_ = std::min(end_block, document_.blocks.size());
  next_ = from;
  handed_.clear();
}

std::optional<ReadingChunk> ReadingQueue::Next(double rate) {
  if (!loaded_ || handed_.size() > kReadingAhead) {
    return std::nullopt;
  }
  std::optional<ReadingChunk> chunk =
      ChunkAt(document_, next_, ReadingChunkChars(rate), end_block_);
  if (!chunk) {
    return std::nullopt;
  }
  next_ = chunk->next;
  handed_.push_back(*chunk);
  return chunk;
}

void ReadingQueue::Spoken() {
  if (!handed_.empty()) {
    current_block_ = static_cast<int>(handed_.front().at.block);
    handed_.pop_front();
  }
}

void ReadingQueue::Stop() {
  if (!handed_.empty()) {
    next_ = handed_.front().at;
    current_block_ = static_cast<int>(next_.block);
    handed_.clear();
  }
}

void ReadingQueue::Clear() {
  document_ = ReadingDocument();
  loaded_ = false;
  end_block_ = 0;
  next_ = ReadingPosition();
  handed_.clear();
  current_block_ = -1;
}

bool ReadingQueue::finished() const {
  return loaded_ && handed_.empty() &&
         !ChunkAt(document_, next_, kMaxChunkChars, end_block_);
}

ReadingPosition ReadingQueue::position() const {
  return handed_.empty() ? next_ : handed_.front().at;
}

void ReadingQueue::set_position(ReadingPosition position) {
  next_ = position;
  handed_.clear();
}

}  // namespace voice
}  // namespace molt_ai
