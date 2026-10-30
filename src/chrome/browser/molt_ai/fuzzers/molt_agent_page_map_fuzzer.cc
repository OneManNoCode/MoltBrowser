// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The web agent's observation through the accessibility page map
// (agents/agent_page_map.h) over hostile page maps: a page's accessibility
// tree (through a renderer that may be compromised) built from the input,
// mapped by voice::BuildPageMap(), then turned into the agent's elements and
// text, with select options, a URL and page text that are any bytes, and a
// model reply (what a compromised inference service may send) read against
// the elements.
//
// Checks: the observation returns and is the same twice; its text is valid
// UTF-8 within kMaxTextChars, every line one the observation writes (no
// page text starts a line of its own), with ELEMENTS last; at most
// kMaxObservedElements elements, each "e" and its item's number, in order,
// naming the item's node, in a tree of the page's origin, with a one-line
// label without double quotes; no credential field and nothing that isn't
// a writable text field is typeable; a select's options are its own, at
// most kMaxOptionsPerSelect, without double quotes; every element is found
// again on the same map (FindLiveItem()); the action schema converts; a
// reply the parser takes acts only on what was listed (TYPE on a text
// field, SELECT on a listed option); the probe of a node and its ancestors
// names only a link's target, never types into anything it calls a
// credential, and caps its label.
//
// Format: [header][page map]
//   header: 1 byte mask of the trees (0-7) of the page's origin; the URL,
//     the page text and the reply (strings); 1 byte number of selects
//     given options, then for each a count byte and that many strings;
//     1 byte form index for the probe.
//   page map: as molt_page_map_fuzzer reads it (its seeds, after a
//     header, are this fuzzer's).
//   string: a length byte and the bytes; 255 then 2 bytes n and 1 byte c
//     is c repeated n times.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/check_op.h"
#include "chrome/browser/molt_ai/agents/agent_actions.h"
#include "chrome/browser/molt_ai/agents/agent_page_map.h"
#include "chrome/browser/molt_ai/security/agent_safety.h"
#include "chrome/browser/molt_ai/voice/page_map.h"
#include "chrome/browser/molt_ai/voice/page_map_text.h"
#include "chrome/services/molt_ai/inference/engine/json_schema_grammar.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace {

using molt_ai::agent_actions::ObservedElement;
using molt_ai::agent_page_map::PageMapInput;
using molt_ai::agent_page_map::PageMapObservation;
using molt_ai::voice::PageItem;
using molt_ai::voice::PageMap;
using molt_ai::voice::PageNode;
using molt_ai::voice::PageRole;
using molt_ai::voice::PageTree;

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

// molt_page_map_fuzzer's tree format (its limits flags byte included).
PageTree ReadTree(Reader& in) {
  in.U8();  // the page map fuzzer's limits: the default limits here
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
  return tree;
}

// Valid UTF-8 without control characters (but line breaks).
void CheckText(std::string_view s) {
  size_t i = 0;
  while (i < s.size()) {
    const uint8_t b = static_cast<uint8_t>(s[i]);
    size_t n = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
    if (n == 1) {
      CHECK_LT(b, 0x80);
      CHECK(b == '\n' || (b >= 0x20 && b != 0x7F));
    }
    CHECK_LE(i + n, s.size());
    for (size_t k = 1; k < n; ++k) {
      CHECK_EQ(static_cast<uint8_t>(s[i + k]) & 0xC0, 0x80);
    }
    i += n;
  }
}

bool StartsWith(std::string_view s, std::string_view prefix) {
  return s.substr(0, prefix.size()) == prefix;
}

// Every line is one the observation writes: page text never starts one.
void CheckLines(const std::string& text) {
  CHECK(!text.empty() && text.back() == '\n');
  bool elements = false;
  bool after_text = false;
  size_t start = 0;
  size_t index = 0;
  while (start < text.size()) {
    const size_t end = text.find('\n', start);
    CHECK_NE(end, std::string::npos);
    const std::string_view line(text.data() + start, end - start);
    start = end + 1;
    ++index;
    if (index == 1) {
      CHECK(StartsWith(line, "URL: "));
      continue;
    }
    if (index == 2) {
      CHECK(StartsWith(line, "Title: "));
      continue;
    }
    if (index == 3) {
      CHECK(StartsWith(line, "SOURCE: "));
      continue;
    }
    if (elements) {
      // Only element lines, or "(none)", after ELEMENTS.
      CHECK(line == "(none)" || (line.size() > 2 && line[0] == 'e' &&
                                 line[1] >= '1' && line[1] <= '9'));
      continue;
    }
    if (after_text) {
      after_text = false;  // the page's text: one line, any words
      continue;
    }
    if (line == "ELEMENTS:") {
      elements = true;
      continue;
    }
    if (line == "TEXT:") {
      after_text = true;
      continue;
    }
    const bool heading = line.size() > 2 && line[0] == 'h' && line[1] >= '1' &&
                         line[1] <= '6' && line[2] == ' ';
    const bool entry = line.size() > 3 && line[0] == ' ' && line[1] == ' ' &&
                       ((line[2] >= '1' && line[2] <= '9') || line[2] == '(');
    CHECK(line == "HEADINGS:" || line == "LISTS:" ||
          StartsWith(line, "List ") || heading || entry);
  }
  CHECK(elements);
}

void CheckObservation(const PageMapInput& input,
                      const PageMapObservation& out) {
  const PageMap& map = *input.map;
  const auto& elements = out.observation.elements;
  CHECK(out.observation.from_page_map);
  CHECK_LE(molt_ai::voice::CountCodePoints(out.text),
           molt_ai::agent_page_map::kMaxTextChars);
  CheckText(out.text);
  CheckLines(out.text);
  CHECK_LE(elements.size(), molt_ai::agent_actions::kMaxObservedElements);
  int last = 0;
  for (const ObservedElement& e : elements) {
    CHECK_EQ(e.ref, "e" + std::to_string(e.item_number));
    CHECK_GT(e.item_number, last);
    last = e.item_number;
    const PageItem* item = map.ItemByNumber(e.item_number);
    CHECK(item);
    CHECK_EQ(item->handle.tree, e.node_tree);
    CHECK_EQ(item->handle.node, e.node_id);
    CHECK(std::find(input.same_origin_trees.begin(),
                    input.same_origin_trees.end(),
                    e.node_tree) != input.same_origin_trees.end());
    CHECK(!item->disabled);
    CHECK(e.selector.empty());
    CHECK_EQ(e.label.find('"'), std::string::npos);
    CHECK_EQ(e.label.find('\n'), std::string::npos);
    CHECK_LE(molt_ai::voice::CountCodePoints(e.label),
             molt_ai::agent_actions::kMaxElementLabelChars);
    if (e.typeable) {
      CHECK(item->kind == molt_ai::voice::ItemKind::kTextField);
      CHECK(item->writable);
      CHECK(item->credential == molt_ai::voice::CredentialKind::kNone);
      CHECK(!molt_ai::agent_safety::IsSensitive(e.field_kind));
    }
    if (e.selectable) {
      CHECK(item->kind == molt_ai::voice::ItemKind::kSelect);
      CHECK(!e.options.empty());
    }
    CHECK_LE(e.options.size(), molt_ai::agent_page_map::kMaxOptionsPerSelect);
    for (const std::string& option : e.options) {
      CHECK(!option.empty());
      CHECK_EQ(option.find('"'), std::string::npos);
      CHECK_LE(molt_ai::voice::CountCodePoints(option),
               molt_ai::agent_actions::kMaxOptionChars);
      bool given = false;
      for (const auto& [number, raw] : input.select_options) {
        given =
            given || (number == e.item_number &&
                      std::find(raw.begin(), raw.end(), option) != raw.end());
      }
      CHECK(given) << "an option the select doesn't have";
    }
    // Found again on the same page, as before every action.
    CHECK_EQ(molt_ai::agent_page_map::FindLiveItem(e, map), item);
  }
  CHECK(JsonSchemaToGrammar(
            molt_ai::agent_actions::BuildActionSchema(out.observation))
            .status == SchemaGrammarStatus::kOk);
}

void CheckReply(std::string_view reply,
                const molt_ai::agent_actions::Observation& observation) {
  using molt_ai::agent_actions::ActionType;
  const molt_ai::agent_actions::ParsedAction parsed =
      molt_ai::agent_actions::ParseAction(reply, observation);
  if (!parsed.action) {
    return;
  }
  const auto& a = *parsed.action;
  switch (a.type) {
    case ActionType::kClick:
      CHECK(observation.Find(a.ref));
      break;
    case ActionType::kType: {
      const ObservedElement* e = observation.Find(a.ref);
      CHECK(e && e->typeable);
      break;
    }
    case ActionType::kFillForm:
      for (const auto& field : a.fields) {
        const ObservedElement* e = observation.Find(field.ref);
        CHECK(e && e->typeable);
      }
      break;
    case ActionType::kSelect: {
      const ObservedElement* e = observation.Find(a.ref);
      CHECK(e && e->selectable);
      CHECK(std::find(e->options.begin(), e->options.end(), a.option) !=
            e->options.end());
      break;
    }
    case ActionType::kCallTool:
    case ActionType::kReplan:
      NOTREACHED() << "not offered";
    default:
      break;
  }
}

void CheckProbe(const PageTree& tree, int form_index, uint8_t start) {
  // A node and the ones before it, as a chain of ancestors.
  std::vector<PageNode> chain;
  for (size_t i = start; i < tree.nodes.size() && chain.size() < 8; ++i) {
    chain.push_back(tree.nodes[i]);
  }
  const molt_ai::agent_safety::ElementProbe probe =
      molt_ai::agent_page_map::ProbeNode(chain, form_index);
  CHECK_EQ(probe.found, !chain.empty());
  if (chain.empty()) {
    return;
  }
  CHECK(probe.field_kind == molt_ai::agent_page_map::FieldKindOf(
                                molt_ai::voice::ClassifyCredential(chain[0])));
  if (!probe.navigates_to.empty()) {
    bool from_link = false;
    for (const PageNode& node : chain) {
      from_link = from_link || (node.role == PageRole::kLink &&
                                node.url == probe.navigates_to);
    }
    CHECK(from_link);
  }
  CHECK(!probe.submits_form || form_index > 0);
  CHECK_LE(molt_ai::voice::CountCodePoints(probe.label), 60u);
  const std::vector<url::Origin> allowed = {
      url::Origin::Create(GURL("https://www.example.test/"))};
  const molt_ai::agent_page_map::ClickQuestion question =
      molt_ai::agent_page_map::CheckClick(probe, allowed,
                                          "https://www.example.test");
  CHECK_EQ(question.effects.empty(),
           !question.new_origin && !probe.submits_form);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  Reader in(data, size);
  const uint8_t mask = in.U8();
  PageMapInput input;
  input.url = in.String();
  input.origin = "https://www.example.test";
  input.page_text = in.String();
  const std::string reply = in.String();
  const uint8_t selects = in.U8() & 15;
  for (uint8_t s = 0; s < selects; ++s) {
    const int number = in.U8();
    std::vector<std::string> options;
    const uint8_t count = in.U8() & 31;
    for (uint8_t o = 0; o < count; ++o) {
      options.push_back(in.String());
    }
    input.select_options.emplace_back(number, std::move(options));
  }
  const int form_index = static_cast<int>(in.U8() % 10) - 1;
  for (int32_t tree = 0; tree < 8; ++tree) {
    if (mask & (1 << tree)) {
      input.same_origin_trees.push_back(tree);
    }
  }

  const PageTree tree = ReadTree(in);
  const PageMap map = molt_ai::voice::BuildPageMap(tree);
  input.map = &map;

  const PageMapObservation out =
      molt_ai::agent_page_map::BuildObservation(input);
  CheckObservation(input, out);
  const PageMapObservation again =
      molt_ai::agent_page_map::BuildObservation(input);
  CHECK_EQ(out.text, again.text);
  CHECK_EQ(out.usable, again.usable);
  CHECK_EQ(out.observation.elements.size(), again.observation.elements.size());

  CheckReply(reply, out.observation);
  CheckProbe(tree, form_index, mask);
  return 0;
}
