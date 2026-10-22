// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/agents/agent_actions.h"

#include <algorithm>
#include <iterator>
#include <utility>

#include "base/json/json_reader.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"

namespace molt_ai {
namespace agent_actions {

namespace {

using agent_safety::FieldKind;

// The reply's nesting: object > "fields" list > field object. A deeper
// reply is refused by the reader itself.
constexpr size_t kMaxReplyJsonDepth = 4;

constexpr ActionType kAllActions[] = {
    ActionType::kNavigate, ActionType::kSearch,   ActionType::kClick,
    ActionType::kType,     ActionType::kFillForm, ActionType::kScroll,
    ActionType::kObserve,  ActionType::kDone,     ActionType::kError,
};

// ---- Text checks ------------------------------------------------------------

// Characters in valid UTF-8 (what base::JSONReader returns): every byte
// that isn't a continuation byte starts one.
size_t CountChars(std::string_view s) {
  size_t n = 0;
  for (char c : s) {
    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80)
      ++n;
  }
  return n;
}

// |s| cut to at most |max_chars| characters, at a character boundary.
std::string TruncateChars(std::string_view s, size_t max_chars) {
  size_t chars = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
      if (chars == max_chars)
        return std::string(s.substr(0, i));
      ++chars;
    }
  }
  return std::string(s);
}

bool IsAsciiSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}

bool IsBlank(std::string_view s) {
  return std::all_of(s.begin(), s.end(), IsAsciiSpace);
}

// C0 controls and DEL. |allow_newlines| keeps \n and \t (typed text and
// results may span lines).
bool HasControlChars(std::string_view s, bool allow_newlines) {
  for (char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (allow_newlines && (c == '\n' || c == '\t'))
      continue;
    if (c < 0x20 || c == 0x7F)
      return true;
  }
  return false;
}

// JSON string literal for the schema text. Everything quoted here is our
// own ASCII (key names, action names, "e12"), but escape anyway.
std::string Quote(std::string_view s) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out = "\"";
  for (char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += ch;
    } else if (c < 0x20 || c == 0x7F) {
      out += "\\u00";
      out += kHex[c >> 4];
      out += kHex[c & 0xF];
    } else {
      out += ch;
    }
  }
  out += '"';
  return out;
}

// ---- Schema pieces ----------------------------------------------------------

std::string StringSchema(size_t min_chars, size_t max_chars) {
  std::string s = "{\"type\":\"string\"";
  if (min_chars > 0)
    s += ",\"minLength\":" + base::NumberToString(min_chars);
  s += ",\"maxLength\":" + base::NumberToString(max_chars) + "}";
  return s;
}

std::string EnumSchema(const std::vector<std::string>& values) {
  std::string s = "{\"enum\":[";
  for (size_t i = 0; i < values.size(); ++i) {
    if (i)
      s += ',';
    s += Quote(values[i]);
  }
  return s + "]}";
}

using Properties = std::vector<std::pair<std::string_view, std::string>>;

// Every property required, in this order, and no others.
std::string ObjectSchema(const Properties& properties) {
  std::string s = "{\"type\":\"object\",\"properties\":{";
  std::string required;
  for (size_t i = 0; i < properties.size(); ++i) {
    if (i) {
      s += ',';
      required += ',';
    }
    s += Quote(properties[i].first) + ":" + properties[i].second;
    required += Quote(properties[i].first);
  }
  return s + "},\"required\":[" + required +
         "],\"additionalProperties\":false}";
}

// The keys of each action's object, in schema order. "reason" first: the
// model states why before it commits to the action.
std::vector<std::string_view> KeysFor(ActionType type) {
  switch (type) {
    case ActionType::kNavigate:
      return {"reason", "action", "url"};
    case ActionType::kSearch:
      return {"reason", "action", "query"};
    case ActionType::kClick:
      return {"reason", "action", "target"};
    case ActionType::kType:
      return {"reason", "action", "target", "text"};
    case ActionType::kFillForm:
      return {"reason", "action", "fields"};
    case ActionType::kScroll:
      return {"reason", "action", "direction"};
    case ActionType::kObserve:
      return {"reason", "action"};
    case ActionType::kDone:
    case ActionType::kError:
      return {"reason", "action", "result"};
  }
  return {};
}

// ---- Snapshot parsing -------------------------------------------------------

std::string_view FindStringOr(const base::DictValue& d,
                              std::string_view key,
                              std::string_view fallback = {}) {
  const std::string* s = d.FindString(key);
  return s ? std::string_view(*s) : fallback;
}

// The kinds the snapshot script reports; anything else reads as "button"
// (a generic clickable).
std::string NormalizeKind(std::string_view kind) {
  static constexpr std::string_view kKinds[] = {
      "link",   "button",   "input", "textarea",
      "select", "checkbox", "radio", "editable",
  };
  for (std::string_view k : kKinds) {
    if (kind == k)
      return std::string(k);
  }
  return "button";
}

// An <input> type as the DOM normalizes it: lower-case letters and '-',
// at most 24. Anything else (impossible from el.type) reads as "".
std::string NormalizeInputType(std::string_view type) {
  if (type.size() > 24)
    return std::string();
  for (char c : type) {
    if (!((c >= 'a' && c <= 'z') || c == '-'))
      return std::string();
  }
  return std::string(type);
}

// Page text for one prompt line: control characters and line breaks
// become spaces (an element can't start a line of its own), double quotes
// become single ones (a label can't close its quotes and fake another
// element), runs of spaces collapse, then the length cap.
std::string CleanLabel(std::string_view raw) {
  std::string out;
  out.reserve(std::min<size_t>(raw.size(), 4 * kMaxElementLabelChars));
  bool space = false;
  for (char ch : raw) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (c < 0x20 || c == 0x7F || c == ' ') {
      space = !out.empty();
      continue;
    }
    if (space) {
      out += ' ';
      space = false;
    }
    out += (ch == '"') ? '\'' : ch;
    if (out.size() > 4 * kMaxElementLabelChars)
      break;
  }
  return TruncateChars(out, kMaxElementLabelChars);
}

bool IsTextInputType(std::string_view type) {
  static constexpr std::string_view kTextTypes[] = {
      "",       "text", "search", "email",          "url",   "tel",
      "number", "date", "time",   "datetime-local", "month", "week",
  };
  return std::find(std::begin(kTextTypes), std::end(kTextTypes), type) !=
         std::end(kTextTypes);
}

// ---- Reply parsing ----------------------------------------------------------

ParsedAction Fail(ActionError error) {
  ParsedAction p;
  p.error = error;
  return p;
}

// Reads the string |key| into |out|: at most |max_chars| characters, at
// least one non-blank one if |required|, no control characters (\n and \t
// allowed with |multiline|).
ActionError ReadText(const base::DictValue& d,
                     std::string_view key,
                     size_t max_chars,
                     bool required,
                     bool multiline,
                     std::string* out) {
  const std::string* s = d.FindString(key);
  if (!s)
    return ActionError::kWrongKeys;
  if (CountChars(*s) > max_chars)
    return ActionError::kTooLong;
  if (required && IsBlank(*s))
    return ActionError::kEmpty;
  if (HasControlChars(*s, multiline))
    return ActionError::kBadCharacters;
  *out = *s;
  return ActionError::kNone;
}

// An element id from the observation (a text field if |typeable|).
ActionError ReadRef(const base::DictValue& d,
                    const Observation& observation,
                    bool typeable,
                    std::string* out) {
  const std::string* ref = d.FindString("target");
  if (!ref)
    return ActionError::kWrongKeys;
  const ObservedElement* element = observation.Find(*ref);
  if (!element)
    return ActionError::kUnknownElement;
  if (typeable && !element->typeable)
    return ActionError::kNotTypeable;
  *out = *ref;
  return ActionError::kNone;
}

// True if |d| has exactly |keys| (each once; base::DictValue keys are
// unique).
bool HasExactKeys(const base::DictValue& d,
                  const std::vector<std::string_view>& keys) {
  if (d.size() != keys.size())
    return false;
  return std::all_of(keys.begin(), keys.end(),
                     [&](std::string_view k) { return d.Find(k); });
}

ActionError ReadFields(const base::DictValue& d,
                       const Observation& observation,
                       std::vector<FormFieldValue>* out) {
  const base::ListValue* list = d.FindList("fields");
  if (!list)
    return ActionError::kWrongKeys;
  if (list->empty() || list->size() > kMaxFormFields)
    return ActionError::kTooManyFields;
  std::vector<FormFieldValue> fields;
  for (const base::Value& item : *list) {
    if (!item.is_dict())
      return ActionError::kWrongKeys;
    const base::DictValue& f = item.GetDict();
    if (!HasExactKeys(f, {"target", "text"}))
      return ActionError::kWrongKeys;
    FormFieldValue field;
    ActionError e = ReadRef(f, observation, /*typeable=*/true, &field.ref);
    if (e == ActionError::kNone) {
      e = ReadText(f, "text", kMaxTypedChars, /*required=*/false,
                   /*multiline=*/true, &field.text);
    }
    if (e != ActionError::kNone)
      return e;
    for (const FormFieldValue& seen : fields) {
      if (seen.ref == field.ref)
        return ActionError::kDuplicateField;
    }
    fields.push_back(std::move(field));
  }
  *out = std::move(fields);
  return ActionError::kNone;
}

// ---- Reply outcome ----------------------------------------------------------

constexpr char kCutOffNote[] =
    "Your previous reply was cut off before the JSON object was complete. "
    "Keep \"reason\" and the text fields shorter.";
constexpr char kNotJsonNote[] =
    "Your previous reply was not one complete JSON object in the required "
    "format.";
constexpr char kRetryTail[] =
    " Reply with one JSON object in the format above.";

}  // namespace

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

std::string_view ActionName(ActionType type) {
  switch (type) {
    case ActionType::kNavigate:
      return "NAVIGATE";
    case ActionType::kSearch:
      return "SEARCH";
    case ActionType::kClick:
      return "CLICK";
    case ActionType::kType:
      return "TYPE";
    case ActionType::kFillForm:
      return "FILL_FORM";
    case ActionType::kScroll:
      return "SCROLL";
    case ActionType::kObserve:
      return "OBSERVE";
    case ActionType::kDone:
      return "DONE";
    case ActionType::kError:
      return "ERROR";
  }
  return "";
}

std::optional<ActionType> ActionFromName(std::string_view name) {
  for (ActionType t : kAllActions) {
    if (ActionName(t) == name)
      return t;
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Observation
// ---------------------------------------------------------------------------

ObservedElement::ObservedElement() = default;
ObservedElement::~ObservedElement() = default;
ObservedElement::ObservedElement(const ObservedElement&) = default;
ObservedElement& ObservedElement::operator=(const ObservedElement&) = default;
ObservedElement::ObservedElement(ObservedElement&&) = default;
ObservedElement& ObservedElement::operator=(ObservedElement&&) = default;

Observation::Observation() = default;
Observation::~Observation() = default;
Observation::Observation(const Observation&) = default;
Observation& Observation::operator=(const Observation&) = default;
Observation::Observation(Observation&&) = default;
Observation& Observation::operator=(Observation&&) = default;

const ObservedElement* Observation::Find(std::string_view ref) const {
  for (const ObservedElement& e : elements) {
    if (e.ref == ref)
      return &e;
  }
  return nullptr;
}

bool Observation::HasTypeable() const {
  return std::any_of(elements.begin(), elements.end(),
                     [](const ObservedElement& e) { return e.typeable; });
}

std::string BuildElementSnapshotScript() {
  // Runs in the agent's isolated world: the page can shape the DOM it
  // reads (ids, labels, how many elements) but not this code. Labels and
  // selectors are capped here and again in ParseElementSnapshot. The
  // selector is the element's #id when that resolves to it, else an
  // nth-of-type path from <html> (at most 64 levels).
  return std::string(R"JS((() => {
  try {
    const MAX = )JS") +
         base::NumberToString(kMaxObservedElements) +
         R"JS(, SCAN = 4000;
    const q = 'a[href],area[href],button,input,textarea,select,summary,' +
        '[role="button"],[role="link"],[role="tab"],[role="menuitem"],' +
        '[role="checkbox"],[role="radio"],[role="switch"],[role="option"],' +
        '[role="combobox"],[role="searchbox"],[role="textbox"],' +
        '[contenteditable=""],[contenteditable="true"]';
    const nodes = document.querySelectorAll(q);
    const vh = window.innerHeight || 0, vw = window.innerWidth || 0;
    const kindOf = (el) => {
      const t = el.localName;
      if (t === 'input') {
        const ty = String(el.type || 'text').toLowerCase();
        if (ty === 'hidden') return '';
        if (ty === 'submit' || ty === 'button' || ty === 'reset' ||
            ty === 'image') return 'button';
        if (ty === 'checkbox' || ty === 'radio') return ty;
        return 'input';
      }
      if (t === 'textarea' || t === 'select') return t;
      if (t === 'a' || t === 'area') return 'link';
      if (t === 'button' || t === 'summary') return 'button';
      const r = String(el.getAttribute('role') || '').toLowerCase();
      if (r === 'link') return 'link';
      if (r === 'checkbox' || r === 'switch') return 'checkbox';
      if (r === 'radio') return 'radio';
      const ce = el.getAttribute('contenteditable');
      if (r === 'combobox' || r === 'searchbox' || r === 'textbox' ||
          el.isContentEditable || ce === '' || ce === 'true')
        return 'editable';
      return 'button';
    };
    const cands = [];
    const n = Math.min(nodes.length, SCAN);
    for (let i = 0; i < n; i++) {
      const el = nodes[i];
      const kind = kindOf(el);
      if (!kind || el.disabled) continue;
      const r = el.getBoundingClientRect();
      if (!(r.width > 0 && r.height > 0)) continue;
      const inView = r.bottom > 0 && r.right > 0 && r.top < vh && r.left < vw;
      const field = kind === 'input' || kind === 'textarea' ||
                    kind === 'select' || kind === 'editable';
      cands.push([inView ? 0 : field ? 1 : kind === 'link' ? 3 : 2, i, el,
                  kind]);
    }
    cands.sort((a, b) => a[0] - b[0] || a[1] - b[1]);
    const chosen = cands.slice(0, MAX).sort((a, b) => a[1] - b[1]);
    const path = (el) => {
      if (el.id && document.getElementById(el.id) === el)
        return '#' + CSS.escape(el.id);
      const parts = [];
      for (let e = el; e; e = e.parentElement) {
        if (parts.length >= 64) return '';
        let k = 1;
        for (let s = e.previousElementSibling; s; s = s.previousElementSibling)
          if (s.localName === e.localName) k++;
        parts.push(CSS.escape(e.localName) + ':nth-of-type(' + k + ')');
      }
      return parts.reverse().join(' > ');
    };
    const clip = (s) => String(s || '').slice(0, 300).replace(/\s+/g, ' ')
                            .trim().slice(0, 60);
    const labelOf = (el, kind) => {
      let s = el.getAttribute('aria-label');
      if (!s) {
        const by = String(el.getAttribute('aria-labelledby') || '').trim();
        const ln = by && document.getElementById(by.split(/\s+/)[0]);
        if (ln) s = ln.textContent;
      }
      if (!s && el.labels && el.labels.length) s = el.labels[0].textContent;
      if (!s) s = el.getAttribute('placeholder');
      if (!s && kind === 'button' && el.localName === 'input') s = el.value;
      if (!s && kind !== 'input' && kind !== 'textarea' && kind !== 'select')
        s = el.textContent;
      if (!s) s = el.getAttribute('title') || el.getAttribute('alt') ||
                  el.getAttribute('name');
      if (!s && el.querySelector) {
        const img = el.querySelector('img[alt]');
        if (img) s = img.getAttribute('alt');
      }
      return clip(s);
    };
    return {origin: String(location.origin), elements: chosen.map((c) => {
      const el = c[2], kind = c[3];
      const sel = path(el);
      return {sel: sel.length <= )JS" +
         base::NumberToString(kMaxSelectorBytes) + R"JS( ? sel : '', kind: kind,
              type: kind === 'input' ?
                  String(el.type || '').toLowerCase().slice(0, 24) : '',
              ac: String(el.getAttribute('autocomplete') || '').slice(0, 64),
              label: labelOf(el, kind)};
    })};
  } catch (e) {
    return {origin: '', elements: []};
  }
})())JS";
}

Observation ParseElementSnapshot(const base::Value& result,
                                 std::string* origin) {
  Observation observation;
  origin->clear();
  if (!result.is_dict())
    return observation;
  const base::DictValue& d = result.GetDict();
  std::string_view o = FindStringOr(d, "origin");
  if (o.size() <= 512 && !HasControlChars(o, /*allow_newlines=*/false))
    *origin = std::string(o);
  const base::ListValue* list = d.FindList("elements");
  if (!list)
    return observation;
  for (const base::Value& item : *list) {
    if (observation.elements.size() >= kMaxObservedElements)
      break;
    if (!item.is_dict())
      continue;
    const base::DictValue& e = item.GetDict();
    std::string_view sel = FindStringOr(e, "sel");
    if (sel.empty() || sel.size() > kMaxSelectorBytes ||
        HasControlChars(sel, /*allow_newlines=*/false)) {
      continue;
    }
    ObservedElement element;
    element.selector = std::string(sel);
    element.kind = NormalizeKind(FindStringOr(e, "kind"));
    if (element.kind == "input")
      element.input_type = NormalizeInputType(FindStringOr(e, "type"));
    std::string_view ac = FindStringOr(e, "ac");
    if (ac.size() > 64)
      ac = std::string_view();
    element.field_kind = agent_safety::ClassifyField(element.input_type, ac);
    element.label = CleanLabel(FindStringOr(e, "label"));
    element.typeable =
        element.field_kind != FieldKind::kPassword &&
        (element.kind == "textarea" ||
         (element.kind == "input" && IsTextInputType(element.input_type)));
    element.ref = "e" + base::NumberToString(observation.elements.size() + 1);
    observation.elements.push_back(std::move(element));
  }
  return observation;
}

std::string DescribeElement(const ObservedElement& element) {
  std::string s = element.kind;
  if (!element.input_type.empty())
    s += "[" + element.input_type + "]";
  if (!element.label.empty())
    s += " \"" + element.label + "\"";
  if (element.field_kind == FieldKind::kPassword)
    s += " (password field: the agent never types here)";
  else if (element.typeable)
    s += " (text field)";
  return s;
}

std::string DescribeElements(const Observation& observation) {
  if (observation.elements.empty())
    return "(none)\n";
  std::string out;
  for (const ObservedElement& e : observation.elements)
    out += e.ref + " " + DescribeElement(e) + "\n";
  return out;
}

// ---------------------------------------------------------------------------
// Action
// ---------------------------------------------------------------------------

AgentAction::AgentAction() = default;
AgentAction::~AgentAction() = default;
AgentAction::AgentAction(const AgentAction&) = default;
AgentAction& AgentAction::operator=(const AgentAction&) = default;
AgentAction::AgentAction(AgentAction&&) = default;
AgentAction& AgentAction::operator=(AgentAction&&) = default;

ParsedAction::ParsedAction() = default;
ParsedAction::~ParsedAction() = default;
ParsedAction::ParsedAction(ParsedAction&&) = default;
ParsedAction& ParsedAction::operator=(ParsedAction&&) = default;

ModelReply::ModelReply() = default;
ModelReply::~ModelReply() = default;
ModelReply::ModelReply(const ModelReply&) = default;
ModelReply& ModelReply::operator=(const ModelReply&) = default;
ModelReply::ModelReply(ModelReply&&) = default;
ModelReply& ModelReply::operator=(ModelReply&&) = default;

ReplyOutcome::ReplyOutcome() = default;
ReplyOutcome::~ReplyOutcome() = default;
ReplyOutcome::ReplyOutcome(ReplyOutcome&&) = default;
ReplyOutcome& ReplyOutcome::operator=(ReplyOutcome&&) = default;

std::string BuildActionSchema(const Observation& observation) {
  std::vector<std::string> all_refs;
  std::vector<std::string> text_refs;
  for (const ObservedElement& e : observation.elements) {
    all_refs.push_back(e.ref);
    if (e.typeable)
      text_refs.push_back(e.ref);
  }
  const std::string reason = StringSchema(0, kMaxReasonChars);
  std::vector<std::string> branches;
  for (ActionType type : kAllActions) {
    Properties props = {
        {"reason", reason},
        {"action", "{\"const\":" + Quote(ActionName(type)) + "}"},
    };
    switch (type) {
      case ActionType::kNavigate:
        props.emplace_back("url", StringSchema(1, kMaxUrlChars));
        break;
      case ActionType::kSearch:
        props.emplace_back("query", StringSchema(1, kMaxQueryChars));
        break;
      case ActionType::kClick:
        if (all_refs.empty())
          continue;
        props.emplace_back("target", EnumSchema(all_refs));
        break;
      case ActionType::kType:
        if (text_refs.empty())
          continue;
        props.emplace_back("target", EnumSchema(text_refs));
        props.emplace_back("text", StringSchema(0, kMaxTypedChars));
        break;
      case ActionType::kFillForm: {
        if (text_refs.empty())
          continue;
        const size_t max_items = std::min(kMaxFormFields, text_refs.size());
        props.emplace_back(
            "fields",
            "{\"type\":\"array\",\"items\":" +
                ObjectSchema({{"target", EnumSchema(text_refs)},
                              {"text", StringSchema(0, kMaxTypedChars)}}) +
                ",\"minItems\":1,\"maxItems\":" +
                base::NumberToString(max_items) + "}");
        break;
      }
      case ActionType::kScroll:
        props.emplace_back("direction", EnumSchema({"down", "up"}));
        break;
      case ActionType::kObserve:
        break;
      case ActionType::kDone:
      case ActionType::kError:
        props.emplace_back("result", StringSchema(1, kMaxResultChars));
        break;
    }
    branches.push_back(ObjectSchema(props));
  }
  std::string schema = "{\"anyOf\":[";
  for (size_t i = 0; i < branches.size(); ++i) {
    if (i)
      schema += ',';
    schema += branches[i];
  }
  return schema + "]}";
}

std::string_view ActionErrorText(ActionError error) {
  switch (error) {
    case ActionError::kNone:
      return "no error";
    case ActionError::kNotJsonObject:
      return "it was not one JSON object";
    case ActionError::kUnknownAction:
      return "\"action\" was not one of the listed actions";
    case ActionError::kWrongKeys:
      return "its keys or value types did not match that action";
    case ActionError::kTooLong:
      return "a value was too long";
    case ActionError::kEmpty:
      return "a required value was empty";
    case ActionError::kBadCharacters:
      return "a value contained control characters";
    case ActionError::kUnknownElement:
      return "it named an element that is not in ELEMENTS";
    case ActionError::kNotTypeable:
      return "it typed into an element that is not a text field";
    case ActionError::kDuplicateField:
      return "it filled the same field twice";
    case ActionError::kTooManyFields:
      return "FILL_FORM needs 1 to 8 fields";
    case ActionError::kBadDirection:
      return "SCROLL takes \"down\" or \"up\"";
  }
  return "";
}

ParsedAction ParseAction(std::string_view json,
                         const Observation& observation) {
  std::optional<base::DictValue> dict = base::JSONReader::ReadDict(
      json, base::JSON_PARSE_RFC, kMaxReplyJsonDepth);
  if (!dict)
    return Fail(ActionError::kNotJsonObject);
  const std::string* name = dict->FindString("action");
  std::optional<ActionType> type = name ? ActionFromName(*name) : std::nullopt;
  if (!type)
    return Fail(ActionError::kUnknownAction);
  if (!HasExactKeys(*dict, KeysFor(*type)))
    return Fail(ActionError::kWrongKeys);

  AgentAction a;
  a.type = *type;
  ActionError e = ReadText(*dict, "reason", kMaxReasonChars,
                           /*required=*/false, /*multiline=*/false, &a.reason);
  if (e == ActionError::kNone) {
    switch (a.type) {
      case ActionType::kNavigate:
        e = ReadText(*dict, "url", kMaxUrlChars, /*required=*/true,
                     /*multiline=*/false, &a.url);
        break;
      case ActionType::kSearch:
        e = ReadText(*dict, "query", kMaxQueryChars, /*required=*/true,
                     /*multiline=*/false, &a.query);
        break;
      case ActionType::kClick:
        e = ReadRef(*dict, observation, /*typeable=*/false, &a.ref);
        break;
      case ActionType::kType:
        e = ReadRef(*dict, observation, /*typeable=*/true, &a.ref);
        if (e == ActionError::kNone) {
          e = ReadText(*dict, "text", kMaxTypedChars, /*required=*/false,
                       /*multiline=*/true, &a.text);
        }
        break;
      case ActionType::kFillForm:
        e = ReadFields(*dict, observation, &a.fields);
        break;
      case ActionType::kScroll: {
        const std::string* d = dict->FindString("direction");
        if (!d)
          e = ActionError::kWrongKeys;
        else if (*d != "down" && *d != "up")
          e = ActionError::kBadDirection;
        else
          a.direction = *d;
        break;
      }
      case ActionType::kObserve:
        break;
      case ActionType::kDone:
      case ActionType::kError:
        e = ReadText(*dict, "result", kMaxResultChars, /*required=*/true,
                     /*multiline=*/true, &a.result);
        break;
    }
  }
  if (e != ActionError::kNone)
    return Fail(e);
  ParsedAction parsed;
  parsed.action = std::move(a);
  return parsed;
}

ReplyOutcome InterpretReply(const ModelReply& reply,
                            const Observation& observation,
                            int bad_in_a_row,
                            int bad_total) {
  ReplyOutcome out;
  if (reply.cancelled) {
    out.message = "Stopped.";
    return out;
  }
  if (reply.constraint_unsupported) {
    out.message =
        "The on-device model couldn't use the agent's action format, so the "
        "agent stopped.";
    return out;
  }
  std::string note;
  if (reply.invalid_output) {
    note = reply.truncated ? kCutOffNote : kNotJsonNote;
  } else if (!reply.success) {
    out.message = reply.error_message.empty()
                      ? std::string("The on-device model didn't answer.")
                      : "The on-device model failed: " + reply.error_message;
    return out;
  } else {
    ParsedAction parsed = ParseAction(reply.text, observation);
    if (parsed.action) {
      out.decision = ReplyDecision::kAct;
      out.action = std::move(*parsed.action);
      return out;
    }
    note = "Your previous reply was not a usable action: " +
           std::string(ActionErrorText(parsed.error)) + ".";
  }
  if (bad_in_a_row + 1 >= kMaxAttemptsPerStep ||
      bad_total + 1 >= kMaxBadRepliesPerRun) {
    out.message =
        "The model's replies couldn't be used as browser actions, so the "
        "agent stopped. Try a larger model or a simpler task.";
    return out;
  }
  out.decision = ReplyDecision::kRetry;
  out.message = note + kRetryTail;
  return out;
}

}  // namespace agent_actions
}  // namespace molt_ai
