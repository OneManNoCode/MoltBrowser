// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/html_fixture_tree.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "chrome/services/molt_ai/inference/engine/raw_ptr_exclusion.h"

namespace molt_ai {
namespace voice {
namespace test {

namespace {

constexpr size_t kMaxDepth = 200;
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
    // Elements deeper than kMaxDepth hold nothing (the converter recurses;
    // molt_page_reading_fuzzer feeds it any input).
    if (!self_closing && !IsVoid(tag) && stack.size() < kMaxDepth) {
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
    FindLabels(0, std::string::npos);
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

  // A control a <label> names (HTML's labelable elements, those voice mode
  // reads: input but a hidden one, select, textarea, button, meter,
  // progress, output).
  static bool Labelable(const DomNode& n) {
    return !n.text && (n.tag == "input" || n.tag == "select" ||
                       n.tag == "textarea" || n.tag == "button" ||
                       n.tag == "meter" || n.tag == "progress" ||
                       n.tag == "output");
  }

  // Which controls labels name, as Blink's accessible name does: a
  // <label for=id> names the element with that id, a label around a
  // control (without for=) the first one inside it; the name is the
  // label's text without the control's own. Ids are found first.
  void FindLabels(size_t index, size_t label) {
    const DomNode& n = dom_[index];
    if (n.text) {
      return;
    }
    if (const std::string* id = n.Attr("id")) {
      ids_.emplace(*id, index);
    }
    if (n.tag == "label") {
      label = index;
      if (n.HasAttr("for")) {
        pending_for_.emplace_back(index, n.AttrOr("for"));
        label = std::string::npos;
      }
    } else if (label != std::string::npos && Labelable(n) &&
               !wrapped_.count(label)) {
      wrapped_.emplace(label, index);
      label = std::string::npos;
    }
    for (size_t child : n.children) {
      FindLabels(child, label);
    }
    if (index != 0) {
      return;
    }
    for (const auto& [at, id] : pending_for_) {
      const auto found = ids_.find(id);
      if (found != ids_.end() && Labelable(dom_[found->second])) {
        labels_.emplace(found->second, Collapse(TextOf(at)));
      }
    }
    for (const auto& [at, control] : wrapped_) {
      labels_.emplace(control, Collapse(TextOf(at, control)));
    }
  }

  // The visible text inside |index|, collapsed, without |except|'s.
  std::string TextOf(size_t index, size_t except = std::string::npos) const {
    if (index == except) {
      return std::string();
    }
    const DomNode& n = dom_[index];
    if (Hidden(n)) {
      return std::string();
    }
    if (n.text) {
      return n.data;
    }
    std::string out;
    for (size_t child : n.children) {
      out += " " + TextOf(child, except);
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

  // Blink includes an element with any aria-* attribute
  // (AXNodeObject::ShouldIncludeBasedOnSemantics()).
  static bool HasAriaAttribute(const DomNode& n) {
    for (const auto& [key, value] : n.attrs) {
      if (key.size() > 5 && key.compare(0, 5, "aria-") == 0) {
        return true;
      }
    }
    return false;
  }

  // And an element a link within the page could go to: <a name>, or any
  // element with an id but a <label> or <optgroup> (or SVG, which these
  // fixtures don't have). Blink's IsPotentialInPageLinkTarget().
  static bool IsPotentialInPageLinkTarget(const DomNode& n) {
    if (n.tag == "a" && n.HasAttr("name")) {
      return true;
    }
    return n.HasAttr("id") && n.tag != "label" && n.tag != "optgroup";
  }

  // Whether a block's children are all inline (text and inline elements)
  // and there is some: LayoutBlockFlow::ChildrenInline(), which Blink
  // includes the block for. A block with a block child wraps its inline
  // runs in anonymous blocks, and is ignored.
  bool HoldsTextDirectly(const DomNode& n) const {
    bool any = false;
    for (size_t child : n.children) {
      const DomNode& c = dom_[child];
      if (c.text) {
        any = any || !Collapse(c.data).empty();
        continue;
      }
      if (Hidden(c)) {
        continue;
      }
      if (!IsInline(c.tag)) {
        return false;
      }
      any = true;
    }
    return any;
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
                  HasAriaAttribute(n) || !n.AttrOr("title").empty() ||
                  IsPotentialInPageLinkTarget(n) ||
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
    if (n.tag == "select") {
      // Its value: the selected option's text, else the first's.
      std::vector<size_t> options;
      OptionsIn(index, &options);
      size_t chosen = options.empty() ? std::string::npos : options[0];
      for (size_t option : options) {
        if (dom_[option].HasAttr("selected")) {
          chosen = option;
          break;
        }
      }
      if (chosen != std::string::npos) {
        node.value = Collapse(TextOf(chosen));
      }
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
    // The name: aria-labelledby's text, aria-label, a <label>, else from
    // the contents or the attributes below.
    const auto label_text = labels_.find(index);
    const std::string labelled_by =
        Collapse(LabelledByText(n.AttrOr("aria-labelledby")));
    if (!labelled_by.empty()) {
      node.name = labelled_by;
    } else if (const std::string* label = n.Attr("aria-label")) {
      node.name = *label;
    } else if (label_text != labels_.end() && !label_text->second.empty()) {
      node.name = label_text->second;
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
    if (n.tag == "select") {
      // A collapsed select's options, as Blink has them: under its
      // pop-up (a menu list popup, here a generic node), invisible until
      // it opens.
      PageNode popup;
      popup.handle = {0, next_id_++};
      popup.role = PageRole::kGeneric;
      popup.states = kStateInvisible;
      const uint32_t under = Append(std::move(popup), me);
      std::vector<size_t> options;
      OptionsIn(index, &options);
      for (size_t option : options) {
        PageNode child;
        child.handle = {0, next_id_++};
        child.role = PageRole::kOption;
        child.name = Collapse(TextOf(option));
        child.states = kStateInvisible;
        if (dom_[option].HasAttr("disabled")) {
          child.states |= kStateDisabled;
        }
        if (dom_[option].HasAttr("selected")) {
          child.states |= kStateSelected;
        }
        Append(std::move(child), under);
      }
      return;
    }
    if (n.tag == "textarea") {
      return;
    }
    for (size_t child : n.children) {
      Visit(child, me);
    }
  }

  // The <option>s inside |index| (a select, through its optgroups), in
  // order.
  void OptionsIn(size_t index, std::vector<size_t>* options) const {
    for (size_t child : dom_[index].children) {
      const DomNode& c = dom_[child];
      if (c.text || Hidden(c)) {
        continue;
      }
      if (c.tag == "option") {
        options->push_back(child);
      } else if (c.tag == "optgroup") {
        OptionsIn(child, options);
      }
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
  // The text of the elements an aria-labelledby names, in its order.
  std::string LabelledByText(const std::string& list) const {
    std::string out;
    size_t at = 0;
    while (at < list.size()) {
      while (at < list.size() && IsSpace(list[at])) {
        ++at;
      }
      size_t end = at;
      while (end < list.size() && !IsSpace(list[end])) {
        ++end;
      }
      if (end > at) {
        const auto found = ids_.find(list.substr(at, end - at));
        if (found != ids_.end()) {
          out += " " + TextOf(found->second);
        }
      }
      at = end;
    }
    return out;
  }

  MOLT_RAW_PTR_EXCLUSION const std::vector<DomNode>& dom_;
  // FindLabels(): element ids, labels' for=, labels around a control, and
  // the name each labelled control gets (by DOM index).
  std::map<std::string, size_t> ids_;
  std::vector<std::pair<size_t, std::string>> pending_for_;
  std::map<size_t, size_t> wrapped_;
  std::map<size_t, std::string> labels_;
  PageTree tree_;
  int32_t next_id_ = 1;
  float line_ = 0;
};

}  // namespace

PageTree PageTreeFromHtml(std::string_view html) {
  std::string title;
  const std::vector<DomNode> dom = ParseHtml(html, &title);
  PageTree tree = Converter(dom).Convert(title);
  // The document's language, as the accessibility tree gives it: <html
  // lang>.
  for (size_t child : dom[0].children) {
    if (!dom[child].text && dom[child].tag == "html") {
      tree.language = dom[child].AttrOr("lang");
      break;
    }
  }
  return tree;
}

}  // namespace test
}  // namespace voice
}  // namespace molt_ai
