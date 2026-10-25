// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/automation_locators.h"

#include <algorithm>
#include <cstdint>
#include <utility>

#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "chrome/browser/molt_ai/agents/hidden_chars.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace molt_ai {
namespace automation {
namespace locators {

namespace {

constexpr char kReplacement[] = "\xEF\xBF\xBD";  // U+FFFD
constexpr char kEllipsis[] = "\xE2\x80\xA6";     // U+2026

// ---- Text --------------------------------------------------------------------

// The length of the valid UTF-8 sequence at |s|[i], or 0.
size_t Utf8At(std::string_view s, size_t i) {
  const unsigned char c = static_cast<unsigned char>(s[i]);
  if (c < 0x80)
    return 1;
  size_t n = 0;
  uint32_t cp = 0;
  if (c >= 0xC2 && c <= 0xDF) {
    n = 2;
    cp = c & 0x1F;
  } else if (c >= 0xE0 && c <= 0xEF) {
    n = 3;
    cp = c & 0x0F;
  } else if (c >= 0xF0 && c <= 0xF4) {
    n = 4;
    cp = c & 0x07;
  } else {
    return 0;
  }
  if (i + n > s.size())
    return 0;
  for (size_t k = 1; k < n; ++k) {
    const unsigned char cc = static_cast<unsigned char>(s[i + k]);
    if ((cc & 0xC0) != 0x80)
      return 0;
    cp = (cp << 6) | (cc & 0x3F);
  }
  // Overlong forms, surrogates and code points past U+10FFFF.
  if ((n == 3 && cp < 0x800) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
      (cp >= 0xD800 && cp <= 0xDFFF)) {
    return 0;
  }
  return n;
}

// Valid UTF-8 on one line: no C0 control, DEL or C1 control, and at most
// |max_chars| characters.
bool IsCleanLine(std::string_view s, size_t max_chars) {
  size_t chars = 0;
  for (size_t i = 0; i < s.size();) {
    const size_t n = Utf8At(s, i);
    if (!n)
      return false;
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (n == 1 && (c < 0x20 || c == 0x7F))
      return false;
    if (n == 2 && c == 0xC2 &&
        static_cast<unsigned char>(s[i + 1]) <= 0x9F) {
      return false;  // U+0080-U+009F
    }
    if (++chars > max_chars)
      return false;
    i += n;
  }
  return true;
}

bool HasTemplateBraces(std::string_view s) {
  return s.find("{{") != std::string_view::npos ||
         s.find("}}") != std::string_view::npos;
}

bool IsLowerAsciiWord(std::string_view s, size_t max_chars) {
  return !s.empty() && s.size() <= max_chars &&
         std::all_of(s.begin(), s.end(), [](char c) {
           return c >= 'a' && c <= 'z';
         });
}

// A tag name as the capture script reports it: [a-z][a-z0-9-]{0,31}.
bool IsTag(std::string_view s) {
  if (s.empty() || s.size() > kMaxTagChars || s[0] < 'a' || s[0] > 'z')
    return false;
  return std::all_of(s.begin(), s.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
  });
}

// A type class (TypeClass()) or "": [a-z-]{0,24}.
bool IsTypeClass(std::string_view s) {
  return s.size() <= kMaxTypeChars &&
         std::all_of(s.begin(), s.end(), [](char c) {
           return (c >= 'a' && c <= 'z') || c == '-';
         });
}

bool IsToken(std::string_view s) {
  return !s.empty() && s.size() <= kMaxTokenChars &&
         std::all_of(s.begin(), s.end(), [](char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9');
         });
}

bool IsHttpOrigin(std::string_view s) {
  if (s.empty() || s.size() > kMaxOriginBytes)
    return false;
  const GURL url{std::string(s)};
  if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS())
    return false;
  return url::Origin::Create(url).Serialize() == s;
}

bool IsAttribute(std::string_view name, bool* strong) {
  for (std::string_view a : kStrongAttributes) {
    if (a == name) {
      *strong = true;
      return true;
    }
  }
  for (std::string_view a : kWeakAttributes) {
    if (a == name) {
      *strong = false;
      return true;
    }
  }
  return false;
}

// A structural path: one line, at most kMaxPathBytes, at most
// kMaxPathSteps steps joined by " > ", starting at "html" or an "#id".
bool IsPath(std::string_view s) {
  if (s.empty() || s.size() > kMaxPathBytes || !IsCleanLine(s, kMaxPathBytes))
    return false;
  if (!(s.starts_with("html") || s.starts_with("#")))
    return false;
  size_t steps = 1;
  for (size_t at = s.find(" > "); at != std::string_view::npos;
       at = s.find(" > ", at + 3)) {
    if (++steps > kMaxPathSteps)
      return false;
  }
  return true;
}

std::string JsString(std::string_view s) {
  std::string out;
  base::JSONWriter::Write(base::Value(s), &out);
  return out;
}

std::string_view ByName(By by) {
  switch (by) {
    case By::kRole:
      return "role";
    case By::kLabel:
      return "label";
    case By::kAttribute:
      return "attr";
    case By::kText:
      return "text";
    case By::kPath:
      return "path";
    case By::kSelector:
      return "selector";
  }
  return "";
}

// One stored locator; nullopt if it fails a check. kSelector (the
// runner's own entry) is never read.
std::optional<Locator> ParseLocator(const base::Value& v) {
  if (!v.is_dict())
    return std::nullopt;
  const base::DictValue& d = v.GetDict();
  const std::string* by = d.FindString("by");
  const std::string* value = d.FindString("value");
  if (!by || !value || value->empty() || HasTemplateBraces(*value))
    return std::nullopt;
  Locator l;
  l.value = *value;
  if (*by == "role") {
    const std::string* role = d.FindString("role");
    if (!role || !IsLowerAsciiWord(*role, kMaxRoleChars) ||
        !IsCleanLine(*value, kMaxLocatorTextChars)) {
      return std::nullopt;
    }
    l.by = By::kRole;
    l.role = *role;
  } else if (*by == "label" || *by == "text") {
    if (!IsCleanLine(*value, kMaxLocatorTextChars))
      return std::nullopt;
    l.by = *by == "label" ? By::kLabel : By::kText;
  } else if (*by == "attr") {
    const std::string* name = d.FindString("name");
    bool strong = false;
    if (!name || !IsAttribute(*name, &strong) ||
        !IsCleanLine(*value, kMaxLocatorTextChars)) {
      return std::nullopt;
    }
    l.by = By::kAttribute;
    l.name = *name;
  } else if (*by == "path") {
    if (!IsPath(*value))
      return std::nullopt;
    l.by = By::kPath;
  } else {
    return std::nullopt;
  }
  // Exactly the keys of its kind.
  const size_t keys = l.by == By::kRole || l.by == By::kAttribute ? 3u : 2u;
  if (d.size() != keys)
    return std::nullopt;
  return l;
}

base::Value LocatorToValue(const Locator& l) {
  base::DictValue d;
  d.Set("by", ByName(l.by));
  if (l.by == By::kRole)
    d.Set("role", l.role);
  if (l.by == By::kAttribute)
    d.Set("name", l.name);
  d.Set("value", l.value);
  return base::Value(std::move(d));
}

// The list of a block, checked: good entries in order, none twice, at most
// kMaxLocators.
std::vector<Locator> ParseList(const base::ListValue& list) {
  std::vector<Locator> out;
  // Entries looked at: the list itself may be long; only its start counts.
  const size_t look = std::min(list.size(), 4 * kMaxLocators);
  for (size_t i = 0; i < look && out.size() < kMaxLocators; ++i) {
    std::optional<Locator> l = ParseLocator(list[i]);
    if (l && std::find(out.begin(), out.end(), *l) == out.end())
      out.push_back(std::move(*l));
  }
  return out;
}

bool ParseKind(const std::string* tag,
               const std::string* type,
               const std::string* role,
               ElementKind* out) {
  if (!tag || !IsTag(*tag) || !type || !IsTypeClass(*type) || !role ||
      !(role->empty() || IsLowerAsciiWord(*role, kMaxRoleChars))) {
    return false;
  }
  // The type class is the tag's own: an <input>'s, or none.
  if ((*tag == "input") == type->empty())
    return false;
  out->tag = *tag;
  out->type = *type;
  out->role = *role;
  return true;
}

// The browser's own word for a kind; "element" for anything else.
std::string RoleWord(std::string_view role) {
  if (role == "button")
    return "button";
  if (role == "link")
    return "link";
  if (role == "checkbox")
    return "checkbox";
  if (role == "radio")
    return "option";
  if (role == "textbox" || role == "searchbox")
    return "text field";
  if (role == "combobox" || role == "listbox")
    return "list";
  if (role == "tab")
    return "tab";
  if (role == "menuitem")
    return "menu item";
  return std::string();
}

// The JS library: see LibraryScript(). One complete script on its own (a
// var, so it can run again in the same isolated world).
constexpr char kLibraryJs[] = R"JS(var __moltLocatorLib = (function() {
  'use strict';
  var MAX_TEXT = 120, MAX_PATH = 512, MAX_STEPS = 12, MAX_SCAN = 5000,
      MAX_MATCH = 8, MAX_CAND = 32;
  var STRONG = ['id', 'name', 'data-testid', 'data-test', 'data-cy', 'data-qa'];
  var WEAK = ['aria-label', 'placeholder', 'title', 'alt'];
  function norm(s) { return String(s == null ? '' : s).replace(/\s+/g, ' ').trim(); }
  function fit(s) { s = norm(s); return s.length > MAX_TEXT ? '' : s; }
  function attr(el, n) {
    try { var v = el.getAttribute(n); return v == null ? '' : String(v); }
    catch (e) { return ''; }
  }
  function tagOf(el) { return String(el.tagName || '').toLowerCase(); }
  function inputType(el) {
    return tagOf(el) === 'input' ? String(el.type || 'text').toLowerCase() : '';
  }
  function typeClass(el) {
    if (tagOf(el) !== 'input') return '';
    var t = inputType(el);
    if (t === '' || t === 'text' || t === 'search' || t === 'email' ||
        t === 'url' || t === 'tel' || t === 'number') return 'text';
    if (t === 'submit' || t === 'button' || t === 'reset' || t === 'image')
      return 'button';
    return /^[a-z-]{1,24}$/.test(t) ? t : 'text';
  }
  function roleOf(el) {
    var r = attr(el, 'role').toLowerCase().split(/\s+/)[0] || '';
    if (/^[a-z]{1,32}$/.test(r)) return r;
    var t = tagOf(el);
    if (t === 'a' || t === 'area') return el.hasAttribute('href') ? 'link' : '';
    if (t === 'button' || t === 'summary') return 'button';
    if (t === 'select') return (el.multiple || el.size > 1) ? 'listbox' : 'combobox';
    if (t === 'textarea') return 'textbox';
    if (t === 'input') {
      var it = inputType(el);
      if (it === 'checkbox' || it === 'radio') return it;
      if (it === 'range') return 'slider';
      if (it === 'search') return 'searchbox';
      if (it === 'number') return 'spinbutton';
      var c = typeClass(el);
      if (c === 'button') return 'button';
      if (c === 'text') return 'textbox';
      return '';
    }
    if (el.isContentEditable) return 'textbox';
    return '';
  }
  function isField(el) {
    var t = tagOf(el);
    return (t === 'input' && typeClass(el) !== 'button') || t === 'textarea' ||
           t === 'select';
  }
  function labelledBy(el) {
    var ids = attr(el, 'aria-labelledby').split(/\s+/).slice(0, 8), out = [];
    for (var i = 0; i < ids.length; i++) {
      if (!ids[i]) continue;
      var n = document.getElementById(ids[i]);
      if (n) out.push(n.textContent);
    }
    return norm(out.join(' '));
  }
  function labelOf(el) {
    if (!isField(el)) return '';
    try {
      var ls = el.labels, parts = [];
      if (!ls) return '';
      for (var i = 0; i < ls.length && i < 4; i++) parts.push(ls[i].textContent);
      return norm(parts.join(' '));
    } catch (e) { return ''; }
  }
  // A non-field's visible text (an input button's is its value); a
  // field's value is never read.
  function textOf(el) {
    if (isField(el)) return '';
    if (tagOf(el) === 'input') return norm(el.value);
    return norm(el.innerText != null ? el.innerText : el.textContent);
  }
  function imgAlt(el) {
    try {
      var img = el.querySelector && el.querySelector('img[alt]');
      return img ? norm(attr(img, 'alt')) : '';
    } catch (e) { return ''; }
  }
  function nameOf(el) {
    var n = labelledBy(el) || norm(attr(el, 'aria-label'));
    if (n) return n;
    if (isField(el))
      return labelOf(el) || norm(attr(el, 'placeholder')) || norm(attr(el, 'title'));
    return textOf(el) || norm(attr(el, 'title')) || norm(attr(el, 'alt')) || imgAlt(el);
  }
  function goodId(v) {
    return typeof v === 'string' && /^[A-Za-z][A-Za-z0-9_-]{0,63}$/.test(v) &&
           !/[0-9]{3,}/.test(v);
  }
  function uniqueId(node) {
    try { return document.getElementById(node.id) === node; } catch (e) { return false; }
  }
  function pathOf(el) {
    var parts = [], node = el;
    while (node && node.nodeType === 1 && parts.length < MAX_STEPS) {
      var t = tagOf(node);
      if (!/^[a-z][a-z0-9-]*$/.test(t)) return '';
      if (node !== el && goodId(node.id) && uniqueId(node)) {
        parts.unshift('#' + node.id);
        break;
      }
      if (t === 'html') { parts.unshift('html'); break; }
      var idx = 1, sib = node.previousElementSibling;
      while (sib) { if (sib.tagName === node.tagName) idx++; sib = sib.previousElementSibling; }
      parts.unshift(t + ':nth-of-type(' + idx + ')');
      node = node.parentElement;
    }
    if (!parts.length || (parts[0] !== 'html' && parts[0].charAt(0) !== '#')) return '';
    var p = parts.join(' > ');
    return p.length <= MAX_PATH ? p : '';
  }
  function capture(el) {
    if (!el || el.nodeType !== 1) return null;
    var list = [], strong = 0;
    function add(o) { if (list.length < 8) list.push(o); }
    var role = roleOf(el), name = fit(nameOf(el)), label = fit(labelOf(el));
    if (role && name) add({by: 'role', role: role, value: name});
    if (label) add({by: 'label', value: label});
    for (var i = 0; i < STRONG.length && strong < 4; i++) {
      var a = STRONG[i];
      var v = a === 'id' ? (goodId(el.id) ? el.id : '') : fit(attr(el, a));
      if (v) { add({by: 'attr', name: a, value: v}); strong++; }
    }
    var text = fit(textOf(el));
    if (text) add({by: 'text', value: text});
    var path = pathOf(el);
    if (path) add({by: 'path', value: path});
    for (var j = 0; j < WEAK.length; j++) {
      var w = fit(attr(el, WEAK[j]));
      if (w) add({by: 'attr', name: WEAK[j], value: w});
    }
    return {origin: String(location.origin), tag: tagOf(el).slice(0, 33),
            type: typeClass(el), role: role, input_type: inputType(el).slice(0, 25),
            autocomplete: attr(el, 'autocomplete').slice(0, 65), list: list};
  }
  function visible(el) {
    try { var r = el.getBoundingClientRect(); return r.width > 0 && r.height > 0; }
    catch (e) { return false; }
  }
  var ROLE_SEL = {
    button: 'button,summary,input[type=button],input[type=submit],input[type=reset],input[type=image]',
    link: 'a[href],area[href]', textbox: 'input,textarea,[contenteditable]',
    searchbox: 'input[type=search]', checkbox: 'input[type=checkbox]',
    radio: 'input[type=radio]', combobox: 'select', listbox: 'select',
    spinbutton: 'input[type=number]', slider: 'input[type=range]'};
  var CLICKABLE = 'a,area,button,summary,label,input[type=button],input[type=submit],' +
      'input[type=reset],input[type=image],[role],[onclick],[tabindex]';
  // The visible elements of |sel| that |ok| accepts, at most MAX_MATCH + 1
  // (one more shows there are too many), looking at MAX_SCAN at most.
  function scan(sel, ok) {
    var out = [], nodes;
    try { nodes = document.querySelectorAll(sel); } catch (e) { return out; }
    var n = Math.min(nodes.length, MAX_SCAN);
    for (var i = 0; i < n && out.length <= MAX_MATCH; i++) {
      var el = nodes[i];
      try { if (visible(el) && ok(el)) out.push(el); } catch (e) {}
    }
    return out;
  }
  function lower(s) { return norm(s).toLowerCase(); }
  function find(l, hint) {
    var want = lower(l.value);
    switch (l.by) {
      case 'selector': {
        var wl = lower(hint);
        return scan(l.value, function(el) {
          if (!wl) return true;
          var t = lower(el.textContent || el.value || attr(el, 'aria-label'));
          return !t || t === wl || t.indexOf(wl) >= 0 || wl.indexOf(t) >= 0;
        });
      }
      case 'path':
        return scan(l.value, function() { return true; });
      case 'role': {
        if (!/^[a-z]{1,32}$/.test(l.role)) return [];
        var sel = '[role="' + l.role + '"]' + (ROLE_SEL[l.role] ? ',' + ROLE_SEL[l.role] : '');
        return scan(sel, function(el) { return roleOf(el) === l.role && lower(nameOf(el)) === want; });
      }
      case 'label':
        return scan('input,select,textarea', function(el) { return lower(labelOf(el)) === want; });
      case 'attr': {
        if (STRONG.indexOf(l.name) < 0 && WEAK.indexOf(l.name) < 0) return [];
        return scan('[' + l.name + ']', function(el) { return attr(el, l.name) === l.value; });
      }
      case 'text':
        return scan(CLICKABLE, function(el) { return !isField(el) && lower(textOf(el)) === want; });
    }
    return [];
  }
  function probe(id, order, hint) {
    var els = [], cands = [], matches = [], over = [];
    for (var i = 0; i < order.length; i++) {
      var found = find(order[i], hint), idx = [], full = false;
      for (var k = 0; k < found.length && k < MAX_MATCH; k++) {
        var at = els.indexOf(found[k]);
        if (at < 0) {
          // No room to report it: this locator then identifies nothing.
          if (els.length >= MAX_CAND) { full = true; continue; }
          at = els.length;
          els.push(found[k]);
          var el = found[k];
          cands.push({tag: tagOf(el).slice(0, 33), type: typeClass(el), role: roleOf(el),
                      input_type: inputType(el).slice(0, 25),
                      autocomplete: attr(el, 'autocomplete').slice(0, 65),
                      label: nameOf(el).slice(0, 121)});
        }
        idx.push(at);
      }
      over.push(found.length > MAX_MATCH || full);
      matches.push(idx);
    }
    globalThis.__moltLocate = {id: id, els: els, pinned: null, token: null};
    return {origin: String(location.origin), candidates: cands, matches: matches, over: over};
  }
  function pin(id, index, token) {
    var L = globalThis.__moltLocate;
    if (!L || L.id !== id || !/^[A-Za-z0-9]{1,64}$/.test(token)) return '';
    var el = L.els[index];
    if (!el || !el.isConnected) return '';
    var old = document.querySelectorAll('[data-molt-pick]');
    for (var i = 0; i < old.length; i++) old[i].removeAttribute('data-molt-pick');
    el.setAttribute('data-molt-pick', token);
    L.pinned = el;
    L.token = token;
    return '[data-molt-pick="' + token + '"]';
  }
  return {capture: capture, probe: probe, pin: pin};
})();
)JS";

}  // namespace

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

Locator::Locator() = default;
Locator::~Locator() = default;
Locator::Locator(const Locator&) = default;
Locator& Locator::operator=(const Locator&) = default;
Locator::Locator(Locator&&) = default;
Locator& Locator::operator=(Locator&&) = default;

StepLocators::StepLocators() = default;
StepLocators::~StepLocators() = default;
StepLocators::StepLocators(const StepLocators&) = default;
StepLocators& StepLocators::operator=(const StepLocators&) = default;
StepLocators::StepLocators(StepLocators&&) = default;
StepLocators& StepLocators::operator=(StepLocators&&) = default;

Candidate::Candidate() = default;
Candidate::~Candidate() = default;
Candidate::Candidate(const Candidate&) = default;
Candidate& Candidate::operator=(const Candidate&) = default;
Candidate::Candidate(Candidate&&) = default;
Candidate& Candidate::operator=(Candidate&&) = default;

Probe::Probe() = default;
Probe::~Probe() = default;
Probe::Probe(const Probe&) = default;
Probe& Probe::operator=(const Probe&) = default;
Probe::Probe(Probe&&) = default;
Probe& Probe::operator=(Probe&&) = default;

// ---------------------------------------------------------------------------
// Kinds and strength
// ---------------------------------------------------------------------------

std::string TypeClass(std::string_view tag, std::string_view input_type) {
  if (tag != "input")
    return std::string();
  const std::string_view t = input_type;
  if (t.empty() || t == "text" || t == "search" || t == "email" ||
      t == "url" || t == "tel" || t == "number") {
    return "text";
  }
  if (t == "submit" || t == "button" || t == "reset" || t == "image")
    return "button";
  if (IsTypeClass(t) && !t.empty())
    return std::string(t);
  return "text";
}

bool KindMatches(const ElementKind& recorded, const ElementKind& live) {
  return recorded.tag == live.tag && recorded.type == live.type &&
         recorded.role == live.role;
}

bool IsStrong(const Locator& locator) {
  switch (locator.by) {
    case By::kRole:
    case By::kLabel:
    case By::kSelector:
      return true;
    case By::kAttribute: {
      bool strong = false;
      return IsAttribute(locator.name, &strong) && strong;
    }
    case By::kText:
    case By::kPath:
      return false;
  }
  return false;
}

std::string Describe(const Locator& locator) {
  switch (locator.by) {
    case By::kRole:
      return "its role and name";
    case By::kLabel:
      return "its label";
    case By::kAttribute: {
      bool strong = false;
      return IsAttribute(locator.name, &strong) ? "its " + locator.name
                                                : "an attribute";
    }
    case By::kText:
      return "its text";
    case By::kPath:
      return "its position on the page";
    case By::kSelector:
      return "the recorded selector";
  }
  return "";
}

// ---------------------------------------------------------------------------
// Stored form
// ---------------------------------------------------------------------------

base::Value ToValue(const StepLocators& locators) {
  base::DictValue d;
  d.Set("origin", locators.origin);
  base::DictValue element;
  element.Set("tag", locators.element.tag);
  element.Set("type", locators.element.type);
  element.Set("role", locators.element.role);
  d.Set("element", std::move(element));
  // A credential field only; absent is an ordinary one.
  if (agent_safety::IsSensitive(locators.field))
    d.Set("field", agent_safety::FieldKindToString(locators.field));
  base::ListValue list;
  for (const Locator& l : locators.list)
    list.Append(LocatorToValue(l));
  d.Set("list", std::move(list));
  return base::Value(std::move(d));
}

std::optional<StepLocators> Parse(const base::Value& value) {
  if (!value.is_dict())
    return std::nullopt;
  const base::DictValue& d = value.GetDict();
  const std::string* origin = d.FindString("origin");
  const base::DictValue* element = d.FindDict("element");
  const base::Value* field = d.Find("field");
  const base::ListValue* list = d.FindList("list");
  if (!origin || !IsHttpOrigin(*origin) || !element || !list)
    return std::nullopt;
  StepLocators out;
  out.origin = *origin;
  if (element->size() != 3 ||
      !ParseKind(element->FindString("tag"), element->FindString("type"),
                 element->FindString("role"), &out.element)) {
    return std::nullopt;
  }
  // A credential field's kind, as the browser writes it; absent for an
  // ordinary field. Anything else refuses the block.
  if (field) {
    if (!field->is_string())
      return std::nullopt;
    out.field = agent_safety::FieldKindFromString(field->GetString());
    if (!agent_safety::IsSensitive(out.field) ||
        agent_safety::FieldKindToString(out.field) != field->GetString()) {
      return std::nullopt;
    }
  }
  out.list = ParseList(*list);
  if (out.list.empty())
    return std::nullopt;
  return out;
}

std::optional<StepLocators> FromCaptureReport(const base::Value& report) {
  if (!report.is_dict())
    return std::nullopt;
  const base::DictValue& d = report.GetDict();
  const std::string* origin = d.FindString("origin");
  const std::string* input_type = d.FindString("input_type");
  const std::string* autocomplete = d.FindString("autocomplete");
  const base::ListValue* list = d.FindList("list");
  if (!origin || !IsHttpOrigin(*origin) || !input_type ||
      input_type->size() > kMaxTypeChars || !autocomplete ||
      autocomplete->size() > kMaxAutocompleteChars + 1 || !list) {
    return std::nullopt;
  }
  StepLocators out;
  out.origin = *origin;
  if (!ParseKind(d.FindString("tag"), d.FindString("type"),
                 d.FindString("role"), &out.element)) {
    return std::nullopt;
  }
  // The type class is the browser's, from the raw type.
  if (out.element.type != TypeClass(out.element.tag, *input_type))
    return std::nullopt;
  out.field = agent_safety::ClassifyField(*input_type, *autocomplete);
  out.list = ParseList(*list);
  if (out.list.empty())
    return std::nullopt;
  return out;
}

// ---------------------------------------------------------------------------
// Scripts
// ---------------------------------------------------------------------------

std::string LibraryScript() {
  static_assert(kMaxLocatorTextChars == 120, "update MAX_TEXT");
  static_assert(kMaxPathBytes == 512 && kMaxPathSteps == 12,
                "update MAX_PATH, MAX_STEPS");
  static_assert(kMaxMatchesPerLocator == 8 && kMaxProbeCandidates == 32,
                "update MAX_MATCH, MAX_CAND");
  return kLibraryJs;
}

std::string BuildCaptureScript(std::string_view selector) {
  return std::string(kLibraryJs) +
         base::StringPrintf(
             R"JS((() => {
  try {
    const el = document.querySelector(%s);
    return el ? __moltLocatorLib.capture(el) : null;
  } catch (e) {
    return null;
  }
})())JS",
             JsString(selector).c_str());
}

std::vector<Locator> ProbeOrder(std::string_view selector,
                                const StepLocators& stored) {
  std::vector<Locator> order;
  if (!selector.empty()) {
    Locator l;
    l.by = By::kSelector;
    l.value = std::string(selector);
    order.push_back(std::move(l));
  }
  for (const Locator& l : stored.list)
    order.push_back(l);
  return order;
}

std::string BuildProbeScript(std::string_view probe_id,
                             const std::vector<Locator>& order,
                             std::string_view text_hint) {
  base::ListValue list;
  for (const Locator& l : order) {
    base::DictValue d;
    d.Set("by", ByName(l.by));
    d.Set("role", l.role);
    d.Set("name", l.name);
    d.Set("value", l.value);
    list.Append(std::move(d));
  }
  std::string order_json;
  base::JSONWriter::Write(list, &order_json);
  return std::string(kLibraryJs) +
         base::StringPrintf(
             R"JS((() => {
  try {
    return __moltLocatorLib.probe(%s, %s, %s);
  } catch (e) {
    return null;
  }
})())JS",
             JsString(probe_id).c_str(), order_json.c_str(),
             JsString(text_hint).c_str());
}

std::optional<Probe> ParseProbe(const base::Value& report, size_t locators) {
  if (!report.is_dict())
    return std::nullopt;
  const base::DictValue& d = report.GetDict();
  const std::string* origin = d.FindString("origin");
  const base::ListValue* candidates = d.FindList("candidates");
  const base::ListValue* matches = d.FindList("matches");
  const base::ListValue* over = d.FindList("over");
  if (!origin || origin->size() > kMaxOriginBytes || !candidates ||
      candidates->size() > kMaxProbeCandidates || !matches ||
      matches->size() != locators || !over || over->size() != locators) {
    return std::nullopt;
  }
  Probe probe;
  probe.origin = *origin;
  for (const base::Value& c : *candidates) {
    if (!c.is_dict())
      return std::nullopt;
    const base::DictValue& cd = c.GetDict();
    Candidate cand;
    const std::string* input_type = cd.FindString("input_type");
    const std::string* autocomplete = cd.FindString("autocomplete");
    const std::string* label = cd.FindString("label");
    if (!ParseKind(cd.FindString("tag"), cd.FindString("type"),
                   cd.FindString("role"), &cand.kind) ||
        !input_type || input_type->size() > kMaxTypeChars + 1 ||
        !autocomplete || autocomplete->size() > kMaxAutocompleteChars + 1 ||
        !label || label->size() > 4 * (kMaxLocatorTextChars + 1)) {
      return std::nullopt;
    }
    cand.field = agent_safety::ClassifyField(*input_type, *autocomplete);
    cand.label = CleanLabel(*label, kMaxShownLabelChars);
    probe.candidates.push_back(std::move(cand));
  }
  for (size_t i = 0; i < locators; ++i) {
    if (!(*matches)[i].is_list() || !(*over)[i].is_bool())
      return std::nullopt;
    const base::ListValue& m = (*matches)[i].GetList();
    if (m.size() > kMaxMatchesPerLocator)
      return std::nullopt;
    std::vector<size_t> indices;
    for (const base::Value& v : m) {
      if (!v.is_int() || v.GetInt() < 0 ||
          static_cast<size_t>(v.GetInt()) >= probe.candidates.size()) {
        return std::nullopt;
      }
      const size_t index = static_cast<size_t>(v.GetInt());
      if (std::find(indices.begin(), indices.end(), index) != indices.end())
        return std::nullopt;
      indices.push_back(index);
    }
    probe.matches.push_back(std::move(indices));
    probe.overflow.push_back((*over)[i].GetBool());
  }
  return probe;
}

bool Eligible(const StepLocators& recorded, const Candidate& candidate) {
  if (!KindMatches(recorded.element, candidate.kind))
    return false;
  // A credential field only for a step on one of the same kind; and the
  // other way round, a credential step only on such a field.
  return candidate.field == recorded.field;
}

Decision Decide(const std::vector<Locator>& order,
                const Probe& probe,
                const StepLocators& recorded,
                std::string_view page_origin) {
  Decision none;
  if (probe.origin != recorded.origin || page_origin != recorded.origin ||
      probe.matches.size() != order.size() ||
      probe.overflow.size() != order.size()) {
    return none;
  }
  // The element each locator found alone, among the eligible ones.
  std::vector<std::optional<size_t>> unique(order.size());
  for (size_t i = 0; i < order.size(); ++i) {
    if (probe.overflow[i])
      continue;  // too many: it identifies nothing
    std::optional<size_t> found;
    bool several = false;
    for (size_t c : probe.matches[i]) {
      if (c >= probe.candidates.size() ||
          !Eligible(recorded, probe.candidates[c])) {
        continue;
      }
      if (found && *found != c)
        several = true;
      found = c;
    }
    if (found && !several)
      unique[i] = found;
  }
  for (size_t i = 0; i < order.size(); ++i) {
    if (!unique[i])
      continue;
    Decision d;
    d.candidate = *unique[i];
    d.locator = i;
    if (!IsStrong(order[i])) {
      d.strength = Strength::kWeak;
      return d;
    }
    d.strength = Strength::kStrong;
    for (size_t j = i + 1; j < order.size(); ++j) {
      if (unique[j] && IsStrong(order[j]) && *unique[j] != *unique[i]) {
        d.strength = Strength::kConflict;
        break;
      }
    }
    return d;
  }
  return none;
}

std::string BuildPinScript(std::string_view probe_id,
                           size_t index,
                           std::string_view token) {
  return base::StringPrintf(
      R"JS((() => {
  try {
    return __moltLocatorLib.pin(%s, %s, %s);
  } catch (e) {
    return '';
  }
})())JS",
      JsString(probe_id).c_str(), base::NumberToString(index).c_str(),
      JsString(IsToken(token) ? token : std::string_view()).c_str());
}

std::string PinSelector(std::string_view token) {
  if (!IsToken(token))
    return std::string();
  return "[data-molt-pick=\"" + std::string(token) + "\"]";
}

std::string PinnedCheck(std::string_view token) {
  if (!IsToken(token))
    return "false";
  return "(globalThis.__moltLocate && globalThis.__moltLocate.token === " +
         JsString(token) + " && globalThis.__moltLocate.pinned === el)";
}

// ---------------------------------------------------------------------------
// Text for the user
// ---------------------------------------------------------------------------

std::string CleanLabel(std::string_view text, size_t max_chars) {
  // One entry per character shown.
  std::vector<std::string> shown;
  bool space = false;
  for (size_t i = 0; i < text.size() && shown.size() <= max_chars + 1;) {
    const size_t n = Utf8At(text, i);
    std::string piece;
    if (!n) {
      piece = kReplacement;
      i += 1;
    } else {
      const unsigned char c = static_cast<unsigned char>(text[i]);
      const bool c0 = n == 1 && (c <= 0x20 || c == 0x7F);
      const bool c1 = n == 2 && c == 0xC2 &&
                      static_cast<unsigned char>(text[i + 1]) <= 0x9F;
      if (c0 || c1) {
        space = !shown.empty();
        i += n;
        continue;
      }
      if (const size_t h = hidden_chars::HiddenCharAt(text, i)) {
        piece = kReplacement;
        i += h;
      } else {
        piece = c == '"' ? std::string("'") : std::string(text.substr(i, n));
        i += n;
      }
    }
    if (space)
      shown.emplace_back(" ");
    space = false;
    shown.push_back(std::move(piece));
  }
  if (max_chars == 0)
    return std::string();
  if (shown.size() > max_chars) {
    shown.resize(max_chars - 1);
    while (!shown.empty() && shown.back() == " ")
      shown.pop_back();
    shown.emplace_back(kEllipsis);
  }
  std::string out;
  for (const std::string& piece : shown)
    out += piece;
  return out;
}

std::string KindWord(const ElementKind& kind) {
  std::string word = RoleWord(kind.role);
  if (!word.empty())
    return word;
  if (kind.tag == "input") {
    if (kind.type == "password")
      return "password field";
    if (kind.type == "text")
      return "text field";
    return "field";
  }
  if (kind.tag == "textarea")
    return "text box";
  if (kind.tag == "select")
    return "list";
  if (kind.tag == "a")
    return "link";
  if (kind.tag == "img")
    return "image";
  return "element";
}

std::string WeakMatchQuestion(std::string_view step_description,
                              const Candidate& candidate,
                              const Locator& found_by,
                              Strength strength) {
  const std::string step = CleanLabel(step_description, 80);
  std::string q = step.empty() ? std::string("This step")
                               : "The step \xE2\x80\x9C" + step +
                                     "\xE2\x80\x9D";
  q += strength == Strength::kConflict
           ? " found different elements in different ways: the page has "
             "changed. "
           : " couldn't find its element as recorded: the page has changed. ";
  q += "The closest match is the " + KindWord(candidate.kind);
  if (!candidate.label.empty())
    q += " \xE2\x80\x9C" + candidate.label + "\xE2\x80\x9D";
  q += ", found by " + Describe(found_by) + ". Use it?";
  return q;
}

}  // namespace locators
}  // namespace automation
}  // namespace molt_ai
