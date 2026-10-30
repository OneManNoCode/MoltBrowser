// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/html_fixture_tree.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "chrome/services/molt_ai/inference/engine/raw_ptr_exclusion.h"

namespace molt_ai {
namespace voice {
namespace test {

namespace {

constexpr float kLineHeight = 24;
constexpr float kWidth = 1000;
constexpr float kViewport = 800;

struct DomNode {
  bool text = false;
  std::string tag;
  std::vector<std::pair<std::string, std::string>> attrs;
  std::string data;
  std::vector<size_t> children;

  const std::string* Attr(std::string_view name) const {
    for (const auto& [key, value] : attrs) {
      if (key == name) {
        return &value;
      }
    }
    return nullptr;
  }
  bool HasAttr(std::string_view name) const { return Attr(name) != nullptr; }
  std::string AttrOr(std::string_view name, std::string fallback = {}) const {
    const std::string* value = Attr(name);
    return value ? *value : fallback;
  }
};

bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

std::string Lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

void AppendUtf8(uint32_t cp, std::string* out) {
  if (cp < 0x80) {
    out->push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

std::string Decode(std::string_view s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '&') {
      out.push_back(s[i]);
      continue;
    }
    const size_t semi = s.find(';', i);
    if (semi == std::string_view::npos || semi - i > 10) {
      out.push_back('&');
      continue;
    }
    const std::string_view name = s.substr(i + 1, semi - i - 1);
    if (name == "amp") {
      out.push_back('&');
    } else if (name == "lt") {
      out.push_back('<');
    } else if (name == "gt") {
      out.push_back('>');
    } else if (name == "quot") {
      out.push_back('"');
    } else if (name == "apos") {
      out.push_back('\'');
    } else if (name == "nbsp") {
      out.push_back(' ');
    } else if (!name.empty() && name[0] == '#') {
      uint32_t cp = 0;
      const bool hex = name.size() > 1 && (name[1] == 'x' || name[1] == 'X');
      for (char c : name.substr(hex ? 2 : 1)) {
        const int digit = c >= '0' && c <= '9'   ? c - '0'
                          : c >= 'a' && c <= 'f' ? c - 'a' + 10
                          : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                 : -1;
        if (digit < 0 || (!hex && digit > 9)) {
          break;
        }
        cp = cp * (hex ? 16 : 10) + static_cast<uint32_t>(digit);
      }
      AppendUtf8(cp, &out);
    } else {
      out.push_back('&');
      continue;
    }
    i = semi;
  }
  return out;
}

std::string Collapse(std::string_view s) {
  std::string out;
  bool space = false;
  for (char c : s) {
    if (IsSpace(c)) {
      space = !out.empty();
      continue;
    }
    if (space) {
      out.push_back(' ');
      space = false;
    }
    out.push_back(c);
  }
  return out;
}

bool IsVoid(std::string_view tag) {
  for (std::string_view v : {"area", "base", "br", "col", "embed", "hr", "img",
                             "input", "link", "meta", "source", "track",
                             "wbr"}) {
    if (tag == v) {
      return true;
    }
  }
  return false;
}

bool IsInline(std::string_view tag) {
  for (std::string_view v :
       {"a", "span", "b", "i", "em", "strong", "code", "small", "sup", "sub",
        "abbr", "cite", "q", "mark", "time", "label", "img", "br", "s", "u",
        "bdi", "kbd", "var"}) {
    if (tag == v) {
      return true;
    }
  }
  return false;
}

std::vector<DomNode> ParseHtml(std::string_view html, std::string* title) {
  std::vector<DomNode> nodes(1);
  nodes[0].tag = "#document";
  std::vector<size_t> stack = {0};
  size_t i = 0;
  auto add = [&](DomNode node) {
    nodes.push_back(std::move(node));
    nodes[stack.back()].children.push_back(nodes.size() - 1);
    return nodes.size() - 1;
  };
  while (i < html.size()) {
    if (html[i] != '<') {
      const size_t next = html.find('<', i);
      const size_t end = next == std::string_view::npos ? html.size() : next;
      DomNode text;
      text.text = true;
      text.data = Decode(html.substr(i, end - i));
      add(std::move(text));
      i = end;
      continue;
    }
    if (html.substr(i, 4) == "<!--") {
      const size_t end = html.find("-->", i);
      i = end == std::string_view::npos ? html.size() : end + 3;
      continue;
    }
    if (html.substr(i, 2) == "<!") {
      const size_t end = html.find('>', i);
      i = end == std::string_view::npos ? html.size() : end + 1;
      continue;
    }
    if (html.substr(i, 2) == "</") {
      const size_t end = html.find('>', i);
      const std::string name =
          Lower(Collapse(html.substr(i + 2, end - i - 2)));
      i = end == std::string_view::npos ? html.size() : end + 1;
      for (size_t k = stack.size(); k-- > 1;) {
        if (nodes[stack[k]].tag == name) {
          stack.resize(k);
          break;
        }
      }
      continue;
    }
    // A start tag.
    size_t j = i + 1;
    while (j < html.size() && !IsSpace(html[j]) && html[j] != '>' &&
           html[j] != '/') {
      ++j;
    }
    DomNode element;
    element.tag = Lower(html.substr(i + 1, j - i - 1));
    bool self_closing = false;
    while (j < html.size() && html[j] != '>') {
      if (IsSpace(html[j])) {
        ++j;
        continue;
      }
      if (html[j] == '/') {
        self_closing = true;
        ++j;
        continue;
      }
      size_t k = j;
      while (k < html.size() && !IsSpace(html[k]) && html[k] != '=' &&
             html[k] != '>' && html[k] != '/') {
        ++k;
      }
      std::string key = Lower(html.substr(j, k - j));
      std::string value;
      if (k < html.size() && html[k] == '=') {
        ++k;
        if (k < html.size() && (html[k] == '"' || html[k] == '\'')) {
          const char quote = html[k];
          const size_t close = html.find(quote, k + 1);
          value = Decode(html.substr(k + 1, close - k - 1));
          k = close == std::string_view::npos ? html.size() : close + 1;
        } else {
          size_t e = k;
          while (e < html.size() && !IsSpace(html[e]) && html[e] != '>') {
            ++e;
          }
          value = Decode(html.substr(k, e - k));
          k = e;
        }
      }
      element.attrs.emplace_back(std::move(key), std::move(value));
      j = k;
    }
    i = j < html.size() ? j + 1 : html.size();
    const std::string tag = element.tag;
    if (tag == "script" || tag == "style" || tag == "template" ||
        tag == "title") {
      const size_t end = html.find("</" + tag, i);
      const size_t stop = end == std::string_view::npos ? html.size() : end;
      if (tag == "title") {
        *title = Collapse(Decode(html.substr(i, stop - i)));
      }
      const size_t close = html.find('>', stop);
      i = close == std::string_view::npos ? html.size() : close + 1;
      continue;
    }
    const size_t index = add(std::move(element));
    if (!self_closing && !IsVoid(tag)) {
      stack.push_back(index);
    }
  }
  return nodes;
}

class Converter {
 public:
  explicit Converter(const std::vector<DomNode>& dom) : dom_(dom) {}

  PageTree Convert(const std::string& title) {
    tree_.title = title;
    PageNode root;
    root.handle = {0, next_id_++};
    root.role = PageRole::kRootWebArea;
    root.name = title;
    root.html_tag = "#document";
    tree_.nodes.push_back(std::move(root));
    for (size_t child : dom_[0].children) {
      Visit(child, 0);
    }
    Bound(0);
    return std::move(tree_);
  }

 private:
  bool Hidden(const DomNode& n) const {
    if (n.text) {
      return false;
    }
    if (n.tag == "head" || n.tag == "meta" || n.tag == "link" ||
        n.HasAttr("hidden") || n.AttrOr("aria-hidden") == "true" ||
        (n.tag == "input" && Lower(n.AttrOr("type")) == "hidden")) {
      return true;
    }
    const std::string style = Lower(n.AttrOr("style"));
    return style.find("display:none") != std::string::npos ||
           style.find("display: none") != std::string::npos;
  }

  // The visible text inside |index|, collapsed.
  std::string TextOf(size_t index) const {
    const DomNode& n = dom_[index];
    if (Hidden(n)) {
      return std::string();
    }
    if (n.text) {
      return n.data;
    }
    std::string out;
    for (size_t child : n.children) {
      out += " " + TextOf(child);
    }
    if (n.tag == "img") {
      out += " " + n.AttrOr("alt");
    }
    return out;
  }

  PageRole RoleOf(const DomNode& n, bool* ignored_by_role) const {
    *ignored_by_role = false;
    const std::string role = Lower(n.AttrOr("role"));
    const std::string tag = n.tag;
    const std::string type = Lower(n.AttrOr("type", "text"));
    const bool text_input = tag == "textarea" ||
                            (tag == "input" &&
                             (type == "text" || type == "email" ||
                              type == "url" || type == "tel" ||
                              type == "search" || type == "password"));
    if (!role.empty()) {
      static const std::pair<std::string_view, PageRole> kRoles[] = {
          {"button", PageRole::kButton},
          {"link", PageRole::kLink},
          {"search", PageRole::kSearch},
          {"main", PageRole::kMain},
          {"navigation", PageRole::kNavigation},
          {"banner", PageRole::kBanner},
          {"contentinfo", PageRole::kContentInfo},
          {"complementary", PageRole::kComplementary},
          {"region", PageRole::kRegion},
          {"heading", PageRole::kHeading},
          {"list", PageRole::kList},
          {"listitem", PageRole::kListItem},
          {"article", PageRole::kArticle},
          {"feed", PageRole::kFeed},
          {"dialog", PageRole::kDialog},
          {"menu", PageRole::kMenu},
          {"menubar", PageRole::kMenu},
          {"menuitem", PageRole::kMenuItem},
          {"tab", PageRole::kTab},
          {"searchbox", PageRole::kSearchBox},
          {"textbox", PageRole::kTextField},
          {"checkbox", PageRole::kCheckBox},
          {"radio", PageRole::kRadio},
          {"switch", PageRole::kSwitch},
          {"slider", PageRole::kSlider},
          {"option", PageRole::kOption},
          {"listbox", PageRole::kListBox},
          {"grid", PageRole::kGrid},
          {"row", PageRole::kRow},
          {"cell", PageRole::kCell},
          {"img", PageRole::kImage},
      };
      if (role == "combobox") {
        return text_input ? PageRole::kComboBox : PageRole::kSelect;
      }
      if (role == "presentation" || role == "none") {
        *ignored_by_role = true;
        return PageRole::kGeneric;
      }
      for (const auto& [name, page_role] : kRoles) {
        if (role == name) {
          return page_role;
        }
      }
      return PageRole::kGeneric;
    }
    if (tag == "a") {
      return n.HasAttr("href") ? PageRole::kLink : PageRole::kGeneric;
    }
    if (tag == "button") {
      return PageRole::kButton;
    }
    if (tag == "textarea") {
      return PageRole::kTextArea;
    }
    if (tag == "input") {
      if (type == "search") {
        return PageRole::kSearchBox;
      }
      if (text_input) {
        return PageRole::kTextField;
      }
      if (type == "checkbox") {
        return PageRole::kCheckBox;
      }
      if (type == "radio") {
        return PageRole::kRadio;
      }
      if (type == "range" || type == "number") {
        return PageRole::kSlider;
      }
      return PageRole::kButton;
    }
    if (tag == "select") {
      return PageRole::kSelect;
    }
    if (tag == "option") {
      return PageRole::kOption;
    }
    if (tag.size() == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6') {
      return PageRole::kHeading;
    }
    static const std::pair<std::string_view, PageRole> kTags[] = {
        {"ul", PageRole::kList},          {"ol", PageRole::kList},
        {"li", PageRole::kListItem},      {"main", PageRole::kMain},
        {"nav", PageRole::kNavigation},   {"header", PageRole::kBanner},
        {"footer", PageRole::kContentInfo},
        {"aside", PageRole::kComplementary},
        {"form", PageRole::kForm},        {"search", PageRole::kSearch},
        {"article", PageRole::kArticle},  {"table", PageRole::kTable},
        {"tr", PageRole::kRow},           {"td", PageRole::kCell},
        {"th", PageRole::kColumnHeader},  {"video", PageRole::kVideo},
        {"audio", PageRole::kAudio},      {"img", PageRole::kImage},
        {"p", PageRole::kParagraph},      {"time", PageRole::kTime},
    };
    for (const auto& [name, page_role] : kTags) {
      if (tag == name) {
        return page_role;
      }
    }
    if (tag == "section" && n.HasAttr("aria-label")) {
      return PageRole::kRegion;
    }
    return PageRole::kGeneric;
  }

  bool HoldsTextDirectly(const DomNode& n) const {
    for (size_t child : n.children) {
      const DomNode& c = dom_[child];
      if (c.text ? !Collapse(c.data).empty()
                 : (IsInline(c.tag) && !Hidden(c))) {
        return true;
      }
    }
    return false;
  }

  static bool NamedFromContents(PageRole role) {
    switch (role) {
      case PageRole::kLink:
      case PageRole::kButton:
      case PageRole::kHeading:
      case PageRole::kOption:
      case PageRole::kCell:
      case PageRole::kColumnHeader:
      case PageRole::kTab:
      case PageRole::kMenuItem:
      case PageRole::kCheckBox:
      case PageRole::kRadio:
      case PageRole::kSwitch:
        return true;
      default:
        return false;
    }
  }

  void Visit(size_t index, uint32_t parent) {
    const DomNode& n = dom_[index];
    if (Hidden(n)) {
      return;
    }
    if (n.text) {
      const std::string text = Collapse(n.data);
      if (text.empty()) {
        return;
      }
      PageNode node;
      node.handle = {0, next_id_++};
      node.role = PageRole::kStaticText;
      node.name = text;
      Line(&node, 1);
      Append(std::move(node), parent);
      return;
    }
    if (n.tag == "html" || n.tag == "body") {
      for (size_t child : n.children) {
        Visit(child, parent);
      }
      return;
    }
    bool ignored_by_role = false;
    const PageRole role = RoleOf(n, &ignored_by_role);
    const bool custom = n.tag.find('-') != std::string::npos;
    const bool focusable_tag =
        (n.tag == "a" && n.HasAttr("href")) || n.tag == "button" ||
        n.tag == "input" || n.tag == "select" || n.tag == "textarea";
    const bool focusable = focusable_tag || n.HasAttr("tabindex");
    bool ignored = ignored_by_role;
    if (role == PageRole::kGeneric && !ignored) {
      const bool block = !IsInline(n.tag);
      ignored = !(custom || focusable || n.HasAttr("role") ||
                  n.HasAttr("aria-label") ||
                  (block && HoldsTextDirectly(n)));
    }

    PageNode node;
    node.handle = {0, next_id_++};
    node.role = role;
    uint32_t states = 0;
    if (ignored) {
      states |= kStateIgnored;
    } else {
      node.html_tag = n.tag;
      node.html_id = n.AttrOr("id");
      node.class_name = n.AttrOr("class");
    }
    const std::string type = Lower(n.AttrOr("type", "text"));
    if (n.tag == "input") {
      node.input_type = type;
      node.autocomplete = n.AttrOr("autocomplete");
      node.value = n.AttrOr("value");
      node.placeholder = n.AttrOr("placeholder");
    }
    if (n.tag == "textarea") {
      node.value = Collapse(TextOf(index));
      node.placeholder = n.AttrOr("placeholder");
      states |= kStateMultiline;
    }
    if (n.tag == "a") {
      node.url = n.AttrOr("href");
    }
    if (role == PageRole::kHeading) {
      node.heading_level = n.tag.size() == 2 && n.tag[0] == 'h'
                               ? n.tag[1] - '0'
                               : std::max(1, std::atoi(
                                                 n.AttrOr("aria-level", "2")
                                                     .c_str()));
    }
    // The name.
    if (const std::string* label = n.Attr("aria-label")) {
      node.name = *label;
    } else if (NamedFromContents(role) ||
               (n.tag == "input" && role == PageRole::kButton)) {
      node.name = n.tag == "input" ? n.AttrOr("value") : Collapse(TextOf(index));
    } else if (n.tag == "img") {
      node.name = n.AttrOr("alt");
    }
    if (node.name.empty()) {
      node.name = n.AttrOr("title");
    }
    if (node.name.empty() &&
        (role == PageRole::kTextField || role == PageRole::kSearchBox ||
         role == PageRole::kComboBox || role == PageRole::kTextArea)) {
      node.name = n.AttrOr("placeholder");
    }
    // States.
    if (focusable) {
      states |= kStateFocusable;
    }
    if (focusable_tag || n.HasAttr("data-clickable")) {
      states |= kStateHasDefaultAction;
    }
    if (role == PageRole::kTextField || role == PageRole::kSearchBox ||
        role == PageRole::kComboBox || role == PageRole::kTextArea) {
      states |= kStateEditable;
    }
    if (n.HasAttr("disabled")) {
      states |= kStateDisabled;
    }
    if (n.HasAttr("readonly")) {
      states |= kStateReadOnly;
    }
    if (n.HasAttr("required")) {
      states |= kStateRequired;
    }
    if (n.HasAttr("checked") || n.AttrOr("aria-pressed") == "true" ||
        n.AttrOr("aria-checked") == "true") {
      states |= kStateChecked;
    }
    if (n.AttrOr("aria-expanded") == "true") {
      states |= kStateExpanded;
    } else if (n.AttrOr("aria-expanded") == "false") {
      states |= kStateCollapsed;
    }
    if (n.tag == "input" && type == "password") {
      states |= kStateProtected;
    }
    node.states = states;
    const bool leaf = n.tag == "input" || n.tag == "img" ||
                      n.tag == "video" || n.tag == "audio" ||
                      n.tag == "textarea" || n.tag == "select";
    if (leaf) {
      Line(&node, n.tag == "video" ? 10 : 1);
    }
    const uint32_t me = Append(std::move(node), parent);
    if (n.HasAttr("autofocus")) {
      tree_.nodes[me].states |= kStateFocused;
      tree_.focus = tree_.nodes[me].handle;
    }
    if (n.tag == "textarea" || n.tag == "select") {
      return;
    }
    for (size_t child : n.children) {
      Visit(child, me);
    }
  }

  void Line(PageNode* node, int lines) {
    node->bounds = {0, line_ * kLineHeight, kWidth, kLineHeight * lines};
    line_ += lines;
    if (node->bounds.y >= kViewport) {
      node->states |= kStateOffscreen;
    }
  }

  uint32_t Append(PageNode node, uint32_t parent) {
    const uint32_t index = static_cast<uint32_t>(tree_.nodes.size());
    tree_.nodes.push_back(std::move(node));
    tree_.nodes[parent].children.push_back(index);
    return index;
  }

  // A container's bounds: around its children's.
  void Bound(uint32_t index) {
    const std::vector<uint32_t> children = tree_.nodes[index].children;
    float top = -1;
    float bottom = -1;
    for (uint32_t child : children) {
      Bound(child);
      const PageRect& b = tree_.nodes[child].bounds;
      if (b.empty()) {
        continue;
      }
      top = top < 0 ? b.y : std::min(top, b.y);
      bottom = std::max(bottom, b.y + b.height);
    }
    PageNode& node = tree_.nodes[index];
    if (node.bounds.empty() && top >= 0) {
      node.bounds = {0, top, kWidth, bottom - top};
      if (top >= kViewport) {
        node.states |= kStateOffscreen;
      }
    }
  }

  // The caller's, which outlives the converter.
  MOLT_RAW_PTR_EXCLUSION const std::vector<DomNode>& dom_;
  PageTree tree_;
  int32_t next_id_ = 1;
  float line_ = 0;
};

}  // namespace

PageTree PageTreeFromHtml(std::string_view html) {
  std::string title;
  const std::vector<DomNode> dom = ParseHtml(html, &title);
  return Converter(dom).Convert(title);
}

}  // namespace test
}  // namespace voice
}  // namespace molt_ai
