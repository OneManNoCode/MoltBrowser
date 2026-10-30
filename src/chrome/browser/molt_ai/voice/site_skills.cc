// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/site_skills.h"

#include <algorithm>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
#include <unordered_map>
#include <utility>

#include "chrome/browser/molt_ai/voice/spoken_text.h"
#include "chrome/services/molt_ai/inference/engine/bounded_json.h"
#include "chrome/services/molt_ai/inference/engine/raw_ptr_exclusion.h"

namespace molt_ai {
namespace voice {

namespace {

constexpr uint32_t kNoPos = std::numeric_limits<uint32_t>::max();
// A link target is scanned this far for url_has and url_not.
constexpr size_t kMaxUrlScanBytes = 2048;
// Selector checks a page may cost, in all (a hostile page can't make a
// skill slow: it then finds less).
constexpr size_t kMaxMatchWork = 2000000;

// The JSON names of PageRole's values, in order.
constexpr std::string_view kRoleNames[] = {
    "generic",    "document",      "text",       "image",
    "link",       "button",        "textfield",  "searchbox",
    "textarea",   "combobox",      "select",     "checkbox",
    "radio",      "switch",        "slider",     "option",
    "menuitem",   "tab",           "treeitem",   "listbox",
    "list",       "listitem",      "feed",       "article",
    "table",      "grid",          "row",        "cell",
    "columnheader", "rowheader",   "heading",    "video",
    "audio",      "main",          "navigation", "banner",
    "contentinfo", "complementary", "search",    "form",
    "region",     "dialog",        "menu",       "iframe",
    "paragraph",  "time",
};
static_assert(std::size(kRoleNames) ==
                  static_cast<size_t>(PageRole::kTime) + 1,
              "a name for every PageRole");
static_assert(std::size(kRoleNames) <= 64, "roles fit SkillSelector::roles");

std::string LowerAscii(std::string_view s, size_t max_bytes) {
  std::string out(s.substr(0, max_bytes));
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

bool IsHtmlSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

// Whether |haystack| holds |words| as whole words (both folded: words
// joined by single spaces).
bool HasWords(std::string_view haystack, std::string_view words) {
  if (words.empty() || haystack.size() < words.size()) {
    return false;
  }
  size_t at = 0;
  while ((at = haystack.find(words, at)) != std::string_view::npos) {
    const size_t end = at + words.size();
    if ((at == 0 || haystack[at - 1] == ' ') &&
        (end == haystack.size() || haystack[end] == ' ')) {
      return true;
    }
    ++at;
  }
  return false;
}

bool HostIsOrIsUnder(std::string_view host, std::string_view domain) {
  if (host == domain) {
    return true;
  }
  return host.size() > domain.size() &&
         host.substr(host.size() - domain.size()) == domain &&
         host[host.size() - domain.size() - 1] == '.';
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

class SkillParser {
 public:
  explicit SkillParser(std::string* error) : error_(error) {}

  bool Fail(std::string why) {
    if (error_->empty()) {
      *error_ = std::move(why);
    }
    return false;
  }

  // A string of choices "a|b|c": each trimmed, non-empty, at most
  // kMaxSkillStringBytes, no control character; at most kMaxSkillChoices.
  bool Choices(const JsonValue& value,
               std::string_view key,
               std::vector<std::string>* out) {
    if (!value.is_string()) {
      return Fail(std::string(key) + ": not a string");
    }
    const std::string& text = value.text;
    size_t start = 0;
    while (start <= text.size()) {
      size_t bar = text.find('|', start);
      if (bar == std::string::npos) {
        bar = text.size();
      }
      std::string_view choice =
          std::string_view(text).substr(start, bar - start);
      while (!choice.empty() && choice.front() == ' ') {
        choice.remove_prefix(1);
      }
      while (!choice.empty() && choice.back() == ' ') {
        choice.remove_suffix(1);
      }
      if (choice.empty() || choice.size() > kMaxSkillStringBytes) {
        return Fail(std::string(key) + ": empty or too long a choice");
      }
      for (char c : choice) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
          return Fail(std::string(key) + ": a control character");
        }
      }
      if (out->size() >= kMaxSkillChoices) {
        return Fail(std::string(key) + ": too many choices");
      }
      out->emplace_back(choice);
      start = bar + 1;
    }
    return true;
  }

  bool LowerChoices(const JsonValue& value,
                    std::string_view key,
                    std::vector<std::string>* out) {
    if (!Choices(value, key, out)) {
      return false;
    }
    for (std::string& choice : *out) {
      choice = LowerAscii(choice, kMaxSkillStringBytes);
    }
    return true;
  }

  bool FoldedChoices(const JsonValue& value,
                     std::string_view key,
                     std::vector<std::string>* out) {
    if (!Choices(value, key, out)) {
      return false;
    }
    for (std::string& choice : *out) {
      choice = FoldForComparison(choice);
      if (choice.empty()) {
        return Fail(std::string(key) + ": a choice with no words");
      }
    }
    return true;
  }

  bool Level(const JsonValue& value, SkillSelector* selector) {
    auto digit = [](char c) { return c >= '1' && c <= '9'; };
    const std::string& text = value.text;
    if (value.is_number() && text.size() == 1 && digit(text[0])) {
      selector->min_level = selector->max_level = text[0] - '0';
      return true;
    }
    if (value.is_string() && text.size() == 3 && digit(text[0]) &&
        text[1] == '-' && digit(text[2]) && text[0] <= text[2]) {
      selector->min_level = text[0] - '0';
      selector->max_level = text[2] - '0';
      return true;
    }
    return Fail("level: a digit 1-9, or \"a-b\"");
  }

  bool Selector(const JsonValue& value, int depth, SkillSelector* out) {
    if (!value.is_object() || value.members.empty()) {
      return Fail("a selector must be a non-empty object");
    }
    for (const auto& [key, member] : value.members) {
      if (key == "role") {
        std::vector<std::string> roles;
        if (!LowerChoices(member, key, &roles)) {
          return false;
        }
        for (const std::string& role : roles) {
          const auto* it =
              std::find(std::begin(kRoleNames), std::end(kRoleNames), role);
          if (it == std::end(kRoleNames)) {
            return Fail("role: unknown role '" + role + "'");
          }
          out->roles |= uint64_t{1} << (it - std::begin(kRoleNames));
        }
      } else if (key == "name") {
        if (!FoldedChoices(member, key, &out->names)) {
          return false;
        }
      } else if (key == "name_has") {
        if (!FoldedChoices(member, key, &out->name_has)) {
          return false;
        }
      } else if (key == "id") {
        if (!Choices(member, key, &out->ids)) {
          return false;
        }
      } else if (key == "class") {
        if (!Choices(member, key, &out->classes)) {
          return false;
        }
      } else if (key == "tag") {
        if (!LowerChoices(member, key, &out->tags)) {
          return false;
        }
      } else if (key == "url_has") {
        if (!LowerChoices(member, key, &out->url_has)) {
          return false;
        }
      } else if (key == "url_not") {
        if (!LowerChoices(member, key, &out->url_not)) {
          return false;
        }
      } else if (key == "input_type") {
        if (!LowerChoices(member, key, &out->input_types)) {
          return false;
        }
      } else if (key == "level") {
        if (!Level(member, out)) {
          return false;
        }
      } else if (key == "inside" || key == "has") {
        if (depth > 0) {
          return Fail(key + ": nested too deep");
        }
        SkillSelector nested;
        if (!Selector(member, depth + 1, &nested)) {
          return false;
        }
        (key == "inside" ? out->inside : out->has).push_back(std::move(nested));
      } else {
        return Fail("unknown selector key '" + key + "'");
      }
    }
    return true;
  }

  bool Part(const JsonValue& value, std::string_view key, SkillPart* out) {
    if (!value.is_array() || value.items.empty() ||
        value.items.size() > kMaxSkillAlternatives) {
      return Fail(std::string(key) + ": 1 to 12 selectors");
    }
    for (const JsonValue& item : value.items) {
      SkillSelector selector;
      if (!Selector(item, 0, &selector)) {
        return Fail(std::string(key) + ": " + *error_);
      }
      out->push_back(std::move(selector));
    }
    return true;
  }

  // An object of parts: each key one of |names| (and its part).
  bool Parts(const JsonValue& value,
             std::string_view key,
             std::initializer_list<std::pair<std::string_view, SkillPart*>>
                 parts,
             std::string* label = nullptr) {
    if (!value.is_object()) {
      return Fail(std::string(key) + ": not an object");
    }
    for (const auto& [name, member] : value.members) {
      if (label && name == "label") {
        if (!member.is_string() || member.text.size() > kMaxSkillStringBytes) {
          return Fail(std::string(key) + ".label: a short string");
        }
        *label = member.text;
        continue;
      }
      bool known = false;
      for (const auto& [part_name, part] : parts) {
        if (name == part_name) {
          known = true;
          if (!Part(member, std::string(key) + "." + name, part)) {
            return false;
          }
        }
      }
      if (!known) {
        return Fail(std::string(key) + ": unknown key '" + name + "'");
      }
    }
    return true;
  }

  bool Hosts(const JsonValue& value,
             std::string_view key,
             std::vector<std::string>* out) {
    if (!value.is_array() || value.items.size() > kMaxSkillHosts) {
      return Fail(std::string(key) + ": at most 16 hosts");
    }
    for (const JsonValue& item : value.items) {
      if (!item.is_string() || item.text.empty() ||
          item.text.size() > kMaxSkillStringBytes) {
        return Fail(std::string(key) + ": a host name");
      }
      for (char c : item.text) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
              c == '-')) {
          return Fail(std::string(key) + ": a lower-case host name");
        }
      }
      out->push_back(item.text);
    }
    return true;
  }

  bool Pages(const JsonValue& value, SiteSkill* skill) {
    if (!value.is_array() || value.items.size() > kMaxSkillPages) {
      return Fail("pages: at most 8");
    }
    for (const JsonValue& item : value.items) {
      const JsonValue* path = item.is_object() ? item.Find("path") : nullptr;
      const JsonValue* expect = item.is_object() ? item.Find("expect") : nullptr;
      if (!path || !expect || item.members.size() != 2 || !path->is_string() ||
          path->text.empty() || path->text[0] != '/' ||
          path->text.size() > kMaxSkillStringBytes || !expect->is_array()) {
        return Fail("pages: {\"path\": \"/...\", \"expect\": [...]}");
      }
      SiteSkill::Page page;
      page.path = path->text;
      for (const JsonValue& part : expect->items) {
        bool known = false;
        for (uint32_t bit = kPartSearch; bit <= kPartContent; bit <<= 1) {
          if (part.is_string() &&
              part.text == SkillPartName(static_cast<SkillPartBit>(bit))) {
            page.expect |= bit;
            known = true;
          }
        }
        if (!known) {
          return Fail("pages.expect: unknown part");
        }
      }
      skill->pages.push_back(std::move(page));
    }
    return true;
  }

  bool Skill(const JsonValue& value, SiteSkill* skill) {
    if (!value.is_object()) {
      return Fail("not an object");
    }
    for (const auto& [key, member] : value.members) {
      if (key == "id") {
        if (!member.is_string() || member.text.empty() ||
            member.text.size() > 32) {
          return Fail("id: a short string");
        }
        for (char c : member.text) {
          if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                c == '-')) {
            return Fail("id: lower-case letters, digits and '-'");
          }
        }
        skill->id = member.text;
      } else if (key == "version") {
        const std::string& text = member.text;
        if (!member.is_number() || text.empty() || text.size() > 4 ||
            text[0] == '0' ||
            !std::all_of(text.begin(), text.end(),
                         [](char c) { return c >= '0' && c <= '9'; })) {
          return Fail("version: a positive integer");
        }
        int version = 0;
        for (char c : text) {
          version = version * 10 + (c - '0');
        }
        skill->version = version;
      } else if (key == "name") {
        if (!member.is_string() || member.text.empty() ||
            member.text.size() > kMaxSkillStringBytes) {
          return Fail("name: a short string");
        }
        skill->name = member.text;
      } else if (key == "about") {
        if (!member.is_string()) {
          return Fail("about: a string");
        }
      } else if (key == "hosts") {
        if (!Hosts(member, key, &skill->hosts)) {
          return false;
        }
      } else if (key == "domains") {
        if (!Hosts(member, key, &skill->domains)) {
          return false;
        }
      } else if (key == "except_hosts") {
        if (!Hosts(member, key, &skill->except_hosts)) {
          return false;
        }
      } else if (key == "search") {
        if (!Parts(member, key,
                   {{"field", &skill->search_field},
                    {"submit", &skill->search_submit}})) {
          return false;
        }
      } else if (key == "results") {
        if (!Parts(member, key,
                   {{"within", &skill->results_within},
                    {"title", &skill->result_title}},
                   &skill->results_label)) {
          return false;
        }
      } else if (key == "player") {
        if (!Parts(member, key,
                   {{"media", &skill->media},
                    {"skip_ad", &skill->skip_ad},
                    {"captions", &skill->captions},
                    {"fullscreen", &skill->fullscreen}})) {
          return false;
        }
      } else if (key == "next_page") {
        if (!Part(member, key, &skill->next_page)) {
          return false;
        }
      } else if (key == "next_page_scrolls") {
        if (!member.is_bool()) {
          return Fail("next_page_scrolls: true or false");
        }
        skill->next_page_scrolls = member.boolean;
      } else if (key == "reading") {
        if (!Parts(member, key,
                   {{"content", &skill->content},
                    {"skip", &skill->reading_skip}})) {
          return false;
        }
      } else if (key == "pages") {
        if (!Pages(member, skill)) {
          return false;
        }
      } else {
        return Fail("unknown key '" + key + "'");
      }
    }
    if (skill->id.empty() || skill->version <= 0 || skill->name.empty() ||
        (skill->hosts.empty() && skill->domains.empty())) {
      return Fail("id, version, name and hosts or domains are needed");
    }
    return true;
  }

 private:
  // The caller's, which outlives the parser.
  MOLT_RAW_PTR_EXCLUSION std::string* error_;
};

// ---------------------------------------------------------------------------
// Matching
// ---------------------------------------------------------------------------

// The top-level document's part of a tree, walked once in reading order
// (pre-order), bounded as the page map's walk is: each node once, at most
// max_nodes, at most max_depth deep. A frame's tree (another tree index) is
// left out with everything under it.
class PageWalk {
 public:
  PageWalk(const PageTree& tree, const PageMapLimits& limits) : tree_(&tree) {
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
    const size_t n = order_.size();
    end_.resize(n);
    for (size_t p = 0; p < n; ++p) {
      end_[p] = static_cast<uint32_t>(p + 1);
    }
    for (size_t p = n; p-- > 1;) {
      end_[parent_[p]] = std::max(end_[parent_[p]], end_[p]);
    }
    folded_.resize(n);
    folded_done_.assign(n, 0);
  }

  uint32_t size() const { return static_cast<uint32_t>(order_.size()); }
  const PageNode& node(uint32_t pos) const {
    return tree_->nodes[order_[pos]];
  }
  uint32_t index(uint32_t pos) const { return order_[pos]; }
  uint32_t parent(uint32_t pos) const { return parent_[pos]; }
  uint32_t end(uint32_t pos) const { return end_[pos]; }

  const std::string& FoldedName(uint32_t pos) {
    if (!folded_done_[pos]) {
      folded_done_[pos] = 1;
      folded_[pos] = FoldForComparison(node(pos).name);
    }
    return folded_[pos];
  }

  // Spends one unit of the match budget; false once it is used up.
  bool Spend() { return ++work_ <= kMaxMatchWork; }

 private:
  // Outlives the walk (made on the stack of the functions below).
  MOLT_RAW_PTR_EXCLUSION const PageTree* tree_;
  std::vector<uint32_t> order_;
  std::vector<uint32_t> parent_;
  std::vector<uint32_t> end_;
  std::vector<std::string> folded_;
  std::vector<uint8_t> folded_done_;
  size_t work_ = 0;
};

bool AnyOf(const std::vector<std::string>& choices, std::string_view value) {
  return std::find(choices.begin(), choices.end(), value) != choices.end();
}

bool HasClassToken(std::string_view classes,
                   const std::vector<std::string>& wanted) {
  size_t start = 0;
  while (start < classes.size()) {
    while (start < classes.size() && IsHtmlSpace(classes[start])) {
      ++start;
    }
    size_t end = start;
    while (end < classes.size() && !IsHtmlSpace(classes[end])) {
      ++end;
    }
    if (end > start && AnyOf(wanted, classes.substr(start, end - start))) {
      return true;
    }
    start = end;
  }
  return false;
}

bool Matches(const SkillSelector& s, uint32_t pos, PageWalk& walk) {
  if (!walk.Spend()) {
    return false;
  }
  const PageNode& node = walk.node(pos);
  if (node.Has(kStateInvisible)) {
    return false;
  }
  if (s.roles &&
      !(s.roles & (uint64_t{1} << static_cast<uint32_t>(node.role)))) {
    return false;
  }
  if (!s.tags.empty() &&
      !AnyOf(s.tags, LowerAscii(node.html_tag, kMaxSkillStringBytes + 1))) {
    return false;
  }
  if (!s.ids.empty() && !AnyOf(s.ids, node.html_id)) {
    return false;
  }
  if (!s.classes.empty() && !HasClassToken(node.class_name, s.classes)) {
    return false;
  }
  if (!s.input_types.empty() &&
      !AnyOf(s.input_types,
             LowerAscii(node.input_type, kMaxSkillStringBytes + 1))) {
    return false;
  }
  if (s.min_level > 0 &&
      (node.heading_level < s.min_level || node.heading_level > s.max_level)) {
    return false;
  }
  if (!s.url_has.empty() || !s.url_not.empty()) {
    const std::string url = LowerAscii(node.url, kMaxUrlScanBytes);
    auto holds = [&url](const std::string& part) {
      return url.find(part) != std::string::npos;
    };
    if (!s.url_has.empty() &&
        std::none_of(s.url_has.begin(), s.url_has.end(), holds)) {
      return false;
    }
    if (std::any_of(s.url_not.begin(), s.url_not.end(), holds)) {
      return false;
    }
  }
  if (!s.names.empty() || !s.name_has.empty()) {
    const std::string& name = walk.FoldedName(pos);
    if (!s.names.empty() && !AnyOf(s.names, name)) {
      return false;
    }
    if (!s.name_has.empty() &&
        std::none_of(s.name_has.begin(), s.name_has.end(),
                     [&name](const std::string& words) {
                       return HasWords(name, words);
                     })) {
      return false;
    }
  }
  if (!s.inside.empty()) {
    bool found = false;
    uint32_t at = walk.parent(pos);
    for (size_t hop = 0; hop < kMaxSkillInsideHops && at != kNoPos;
         ++hop, at = walk.parent(at)) {
      if (Matches(s.inside[0], at, walk)) {
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }
  if (!s.has.empty()) {
    const uint32_t stop = static_cast<uint32_t>(
        std::min<size_t>(walk.end(pos), pos + 1 + kMaxSkillHasNodes));
    bool found = false;
    for (uint32_t p = pos + 1; p < stop && !found; ++p) {
      found = Matches(s.has[0], p, walk);
    }
    if (!found) {
      return false;
    }
  }
  return true;
}

// The first node in [begin, end) a selector of |part| finds (the first
// selector that finds one wins), or kNoPos.
uint32_t FindFirst(const SkillPart& part,
                   PageWalk& walk,
                   uint32_t begin,
                   uint32_t end) {
  for (const SkillSelector& selector : part) {
    for (uint32_t p = begin; p < end; ++p) {
      if (Matches(selector, p, walk)) {
        return p;
      }
    }
  }
  return kNoPos;
}

// Every node in [begin, end) the first selector of |part| that finds any
// finds, in order, at most |max|; nodes inside an earlier one aren't
// counted.
std::vector<uint32_t> FindAll(const SkillPart& part,
                              PageWalk& walk,
                              uint32_t begin,
                              uint32_t end,
                              size_t max) {
  std::vector<uint32_t> found;
  for (const SkillSelector& selector : part) {
    for (uint32_t p = begin; p < end && found.size() < max; ++p) {
      if (Matches(selector, p, walk)) {
        found.push_back(p);
        p = walk.end(p) - 1;
      }
    }
    if (!found.empty()) {
      break;
    }
  }
  return found;
}

// What a skill found on a page, as walk positions (kNoPos: nothing).
struct Found {
  uint32_t search_field = kNoPos;
  uint32_t search_submit = kNoPos;
  std::vector<uint32_t> result_entries;
  uint32_t media = kNoPos;
  uint32_t skip_ad = kNoPos;
  uint32_t captions = kNoPos;
  uint32_t fullscreen = kNoPos;
  uint32_t next_page = kNoPos;
  bool next_page_scrolls = false;
};

// Each result: the largest subtree around its title, below the results'
// scope, that holds no other title.
std::vector<uint32_t> EntriesAround(const std::vector<uint32_t>& titles,
                                    uint32_t scope,
                                    PageWalk& walk) {
  std::vector<uint32_t> entries;
  for (size_t i = 0; i < titles.size(); ++i) {
    uint32_t entry = titles[i];
    for (size_t hop = 0; hop < kMaxSkillInsideHops; ++hop) {
      const uint32_t up = walk.parent(entry);
      if (up == kNoPos || up == 0 || up == scope ||
          (i > 0 && up <= titles[i - 1]) ||
          (i + 1 < titles.size() && walk.end(up) > titles[i + 1])) {
        break;
      }
      entry = up;
    }
    entries.push_back(entry);
  }
  return entries;
}

Found FindParts(const SiteSkill& skill, PageWalk& walk) {
  Found found;
  const uint32_t n = walk.size();
  found.search_field = FindFirst(skill.search_field, walk, 0, n);
  found.search_submit = FindFirst(skill.search_submit, walk, 0, n);
  if (!skill.result_title.empty()) {
    uint32_t begin = 0;
    uint32_t end = n;
    const uint32_t scope = FindFirst(skill.results_within, walk, 0, n);
    if (scope != kNoPos) {
      begin = scope + 1;
      end = walk.end(scope);
    }
    const std::vector<uint32_t> titles =
        FindAll(skill.result_title, walk, begin, end, kMaxSkillResults);
    if (titles.size() >= 2) {
      found.result_entries = EntriesAround(titles, scope, walk);
    }
  }
  found.media = FindFirst(skill.media, walk, 0, n);
  found.skip_ad = FindFirst(skill.skip_ad, walk, 0, n);
  found.captions = FindFirst(skill.captions, walk, 0, n);
  found.fullscreen = FindFirst(skill.fullscreen, walk, 0, n);
  found.next_page = FindFirst(skill.next_page, walk, 0, n);
  found.next_page_scrolls = skill.next_page_scrolls;
  return found;
}

struct HandleHash {
  size_t operator()(const PageNodeHandle& h) const {
    return std::hash<int64_t>()((static_cast<int64_t>(h.tree) << 32) ^
                                static_cast<uint32_t>(h.node));
  }
};

// Finds the item a part names: the node's own, else the first of its
// subtree's (a selector may name a control's wrapper), of a kind |fits|.
class ItemFinder {
 public:
  ItemFinder(const PageMap& map, PageWalk& walk) : map_(&map), walk_(&walk) {
    for (size_t i = 0; i < map.items.size(); ++i) {
      if (map.InPage(map.items[i].handle)) {
        items_.emplace(map.items[i].handle, i);
      }
    }
  }

  template <typename Fits>
  PageNodeHandle Find(uint32_t pos, Fits fits) {
    if (pos == kNoPos) {
      return PageNodeHandle();
    }
    const uint32_t stop = static_cast<uint32_t>(
        std::min<size_t>(walk_->end(pos), pos + 1 + kMaxSkillHasNodes));
    for (uint32_t p = pos; p < stop; ++p) {
      auto it = items_.find(walk_->node(p).handle);
      if (it != items_.end() && fits(map_->items[it->second])) {
        return map_->items[it->second].handle;
      }
    }
    return PageNodeHandle();
  }

 private:
  // Both outlive the finder (BuildPageMapWithSkills() makes it on its
  // stack).
  MOLT_RAW_PTR_EXCLUSION const PageMap* map_;
  MOLT_RAW_PTR_EXCLUSION PageWalk* walk_;
  std::unordered_map<PageNodeHandle, size_t, HandleHash> items_;
};

bool IsPressable(const PageItem& item) {
  return !item.disabled &&
         (item.kind == ItemKind::kButton || item.kind == ItemKind::kClickable ||
          item.kind == ItemKind::kSwitch || item.kind == ItemKind::kCheckBox);
}

// Test-only host and skill (ScopedSkillHostForTesting).
struct TestHost {
  std::string host;
  std::string skill_id;
};
std::optional<TestHost>& TestSkillHost() {
  static std::optional<TestHost>* host = new std::optional<TestHost>();
  return *host;
}

}  // namespace

SkillSelector::SkillSelector() = default;
SkillSelector::~SkillSelector() = default;
SkillSelector::SkillSelector(const SkillSelector&) = default;
SkillSelector& SkillSelector::operator=(const SkillSelector&) = default;
SkillSelector::SkillSelector(SkillSelector&&) = default;
SkillSelector& SkillSelector::operator=(SkillSelector&&) = default;

SiteSkill::SiteSkill() = default;
SiteSkill::~SiteSkill() = default;
SiteSkill::SiteSkill(const SiteSkill&) = default;
SiteSkill& SiteSkill::operator=(const SiteSkill&) = default;
SiteSkill::SiteSkill(SiteSkill&&) = default;
SiteSkill& SiteSkill::operator=(SiteSkill&&) = default;

SkillParseResult::SkillParseResult() = default;
SkillParseResult::~SkillParseResult() = default;
SkillParseResult::SkillParseResult(const SkillParseResult&) = default;
SkillParseResult& SkillParseResult::operator=(const SkillParseResult&) =
    default;
SkillParseResult::SkillParseResult(SkillParseResult&&) = default;
SkillParseResult& SkillParseResult::operator=(SkillParseResult&&) = default;

SkillReport::SkillReport() = default;
SkillReport::~SkillReport() = default;
SkillReport::SkillReport(const SkillReport&) = default;
SkillReport& SkillReport::operator=(const SkillReport&) = default;
SkillReport::SkillReport(SkillReport&&) = default;
SkillReport& SkillReport::operator=(SkillReport&&) = default;

SkillReadingScope::SkillReadingScope() = default;
SkillReadingScope::~SkillReadingScope() = default;
SkillReadingScope::SkillReadingScope(const SkillReadingScope&) = default;
SkillReadingScope& SkillReadingScope::operator=(const SkillReadingScope&) =
    default;
SkillReadingScope::SkillReadingScope(SkillReadingScope&&) = default;
SkillReadingScope& SkillReadingScope::operator=(SkillReadingScope&&) = default;

std::string_view SkillPartName(SkillPartBit part) {
  switch (part) {
    case kPartSearch:
      return "search";
    case kPartSearchSubmit:
      return "search_submit";
    case kPartResults:
      return "results";
    case kPartMedia:
      return "media";
    case kPartSkipAd:
      return "skip_ad";
    case kPartCaptions:
      return "captions";
    case kPartFullscreen:
      return "fullscreen";
    case kPartNextPage:
      return "next_page";
    case kPartContent:
      return "content";
  }
  return "";
}

std::string SiteSkill::Label() const {
  return id + "@" + std::to_string(version);
}

bool SiteSkill::AppliesTo(std::string_view host) const {
  const std::string folded = LowerAscii(host, 256);
  for (const std::string& except : except_hosts) {
    if (HostIsOrIsUnder(folded, except)) {
      return false;
    }
  }
  for (const std::string& exact : hosts) {
    if (folded == exact || folded == "www." + exact) {
      return true;
    }
  }
  for (const std::string& domain : domains) {
    if (HostIsOrIsUnder(folded, domain)) {
      return true;
    }
  }
  return false;
}

SkillParseResult ParseSiteSkills(std::string_view json) {
  SkillParseResult result;
  if (json.size() > kMaxSkillsJsonBytes) {
    result.errors.push_back("the file is too large");
    return result;
  }
  JsonLimits limits;
  limits.max_bytes = kMaxSkillsJsonBytes;
  limits.max_depth = 10;
  limits.max_values = 16384;
  const std::optional<JsonValue> root = ParseJson(json, limits);
  if (!root || !root->is_object()) {
    result.errors.push_back("not a JSON object");
    return result;
  }
  const JsonValue* format = root->Find("format");
  const JsonValue* skills = root->Find("skills");
  if (!format || !format->is_number() ||
      format->text != std::to_string(kSiteSkillsFormat)) {
    result.errors.push_back("an unknown format");
    return result;
  }
  if (!skills || !skills->is_array()) {
    result.errors.push_back("no skills");
    return result;
  }
  for (const auto& [key, member] : root->members) {
    if (key != "format" && key != "skills" && key != "about") {
      result.errors.push_back("unknown key '" + key + "'");
      return result;
    }
  }
  for (size_t i = 0; i < skills->items.size(); ++i) {
    if (result.skills.size() >= kMaxSkills) {
      result.errors.push_back("too many skills");
      break;
    }
    std::string error;
    SiteSkill skill;
    if (!SkillParser(&error).Skill(skills->items[i], &skill)) {
      result.errors.push_back("skill " + std::to_string(i + 1) +
                              (skill.id.empty() ? "" : " (" + skill.id + ")") +
                              ": " + error);
      continue;
    }
    const bool duplicate =
        std::any_of(result.skills.begin(), result.skills.end(),
                    [&](const SiteSkill& s) { return s.id == skill.id; });
    if (duplicate) {
      result.errors.push_back("skill " + skill.id + ": a duplicate id");
      continue;
    }
    result.skills.push_back(std::move(skill));
  }
  return result;
}

const std::vector<SiteSkill>& BuiltInSiteSkills() {
  static const std::vector<SiteSkill>* const skills =
      new std::vector<SiteSkill>(
          ParseSiteSkills(BuiltInSiteSkillsJson()).skills);
  return *skills;
}

const SiteSkill* BuiltInSkillForHost(std::string_view host) {
  const std::optional<TestHost>& test = TestSkillHost();
  for (const SiteSkill& skill : BuiltInSiteSkills()) {
    if (test && LowerAscii(host, 256) == test->host) {
      if (skill.id == test->skill_id) {
        return &skill;
      }
      continue;
    }
    if (skill.AppliesTo(host)) {
      return &skill;
    }
  }
  return nullptr;
}

PageMap BuildPageMapWithSkills(const PageTree& tree,
                               const PageMapLimits& limits,
                               const SiteSkill* skill,
                               const SiteSkill* hint,
                               std::string_view path,
                               SkillReport* report) {
  if (!skill && !hint) {
    return BuildPageMap(tree, limits);
  }
  PageWalk walk(tree, limits);
  Found none;
  const Found from_skill = skill ? FindParts(*skill, walk) : none;
  const Found from_hint = hint ? FindParts(*hint, walk) : none;

  PageMapHints hints;
  const bool skill_results = from_skill.result_entries.size() >= 2;
  const std::vector<uint32_t>& entries =
      skill_results ? from_skill.result_entries : from_hint.result_entries;
  for (uint32_t pos : entries) {
    hints.result_entries.push_back(walk.index(pos));
  }
  if (const SiteSkill* named = skill_results ? skill : hint) {
    hints.results_label = named->results_label;
  }
  PageMap map = BuildPageMap(tree, limits, hints);

  ItemFinder items(map, walk);
  SiteParts& site = map.site;
  uint32_t skill_bits = 0;
  uint32_t hint_bits = 0;
  // Each part from the skill when it names an item that fits, else the
  // hint's.
  auto pick = [&](uint32_t Found::* part, uint32_t bit, auto fits) {
    PageNodeHandle handle = items.Find(from_skill.*part, fits);
    if (handle.valid()) {
      skill_bits |= bit;
      return handle;
    }
    handle = items.Find(from_hint.*part, fits);
    if (handle.valid()) {
      hint_bits |= bit;
    }
    return handle;
  };
  site.search_field = pick(&Found::search_field, kPartSearch,
                           [](const PageItem& item) {
                             return item.kind == ItemKind::kTextField &&
                                    item.writable &&
                                    item.credential == CredentialKind::kNone;
                           });
  site.search_submit =
      pick(&Found::search_submit, kPartSearchSubmit, IsPressable);
  site.media = pick(&Found::media, kPartMedia, [](const PageItem& item) {
    return item.kind == ItemKind::kMedia;
  });
  site.skip_ad = pick(&Found::skip_ad, kPartSkipAd, IsPressable);
  site.captions = pick(&Found::captions, kPartCaptions, IsPressable);
  site.fullscreen = pick(&Found::fullscreen, kPartFullscreen, IsPressable);
  site.next_page =
      pick(&Found::next_page, kPartNextPage, [](const PageItem& item) {
        return !item.disabled && (item.kind == ItemKind::kLink ||
                                  item.kind == ItemKind::kButton ||
                                  item.kind == ItemKind::kClickable);
      });
  if (!site.next_page.valid()) {
    if (from_skill.next_page_scrolls) {
      site.next_page_scrolls = true;
      skill_bits |= kPartNextPage;
    } else if (from_hint.next_page_scrolls) {
      site.next_page_scrolls = true;
      hint_bits |= kPartNextPage;
    }
  }
  site.results = !map.lists.empty() && map.lists[0].from_site;
  if (site.results) {
    (skill_results ? skill_bits : hint_bits) |= kPartResults;
  }
  if (skill_bits) {
    site.source = skill->Label();
  } else if (hint_bits) {
    site.source = hint->Label();
  }

  if (report) {
    report->skill = skill ? skill->Label() : std::string();
    report->from_skill = skill_bits;
    report->from_hint = hint_bits;
    report->results = site.results ? map.lists[0].entries.size() : 0;
    report->missing = 0;
    if (skill) {
      for (const SiteSkill::Page& page : skill->pages) {
        if (path.substr(0, page.path.size()) != page.path) {
          continue;
        }
        uint32_t found = skill_bits;
        if ((page.expect & kPartContent) &&
            FindFirst(skill->content, walk, 0, walk.size()) != kNoPos) {
          found |= kPartContent;
        }
        report->missing = page.expect & ~found;
        break;
      }
    }
  }
  return map;
}

SkillReadingScope ReadingScopeFor(const SiteSkill& skill,
                                  const PageTree& tree,
                                  const PageMapLimits& limits) {
  SkillReadingScope scope;
  PageWalk walk(tree, limits);
  const uint32_t root = FindFirst(skill.content, walk, 0, walk.size());
  if (root != kNoPos) {
    scope.root = walk.node(root).handle;
  }
  // Every selector of the skip part counts.
  for (const SkillSelector& selector : skill.reading_skip) {
    for (uint32_t p = 0; p < walk.size(); ++p) {
      if (scope.skip.size() >= kMaxSkillSkips) {
        return scope;
      }
      if (Matches(selector, p, walk)) {
        scope.skip.push_back(walk.node(p).handle);
        p = walk.end(p) - 1;
      }
    }
  }
  return scope;
}

ScopedSkillHostForTesting::ScopedSkillHostForTesting(
    std::string host,
    std::string_view skill_id) {
  TestSkillHost() = TestHost{LowerAscii(host, 256), std::string(skill_id)};
}

ScopedSkillHostForTesting::~ScopedSkillHostForTesting() {
  TestSkillHost().reset();
}

}  // namespace voice
}  // namespace molt_ai
