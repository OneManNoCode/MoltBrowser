// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_routines.h"

#include <algorithm>
#include <set>
#include <utility>

#include "chrome/browser/molt_ai/automation/agent_run_draft.h"
#include "chrome/browser/molt_ai/automation/automation_security.h"
#include "chrome/browser/molt_ai/automation/automation_voice_steps.h"
#include "chrome/browser/molt_ai/security/activation_policy.h"
#include "chrome/browser/molt_ai/voice/spoken_text.h"
#include "chrome/browser/molt_ai/voice/voice_prompts.h"
#include "url/gurl.h"

namespace molt_ai {
namespace voice {

namespace {

using automation::Script;
using automation::Step;
using automation::StepType;
using K = VoiceCommandKind;

// The wire format's keys (extra["voice"]).
constexpr char kCommand[] = "command";
constexpr char kSite[] = "site";
constexpr char kAddress[] = "address";
constexpr char kWords[] = "words";
constexpr char kNewTab[] = "new_tab";
constexpr char kQuery[] = "query";
constexpr char kScope[] = "scope";
constexpr char kWholeQuery[] = "whole_query";
constexpr char kText[] = "text";
constexpr char kVerb[] = "verb";
constexpr char kBy[] = "by";
constexpr char kOrdinal[] = "ordinal";
constexpr char kFromEnd[] = "from_end";
constexpr char kByWords[] = "by_words";
constexpr char kFromWords[] = "from_words";
constexpr char kRoles[] = "roles";
constexpr char kAmount[] = "amount";
constexpr char kCount[] = "count";
constexpr char kSeconds[] = "seconds";
constexpr char kVariable[] = "variable";
constexpr char kPlanned[] = "planned";

constexpr int kMaxOrdinal = 1000;
constexpr int kMaxScrollCount = 20;
constexpr int kMaxSkipSeconds = 24 * 3600;
constexpr int kMaxRoles = 1 << 16;
constexpr size_t kMaxChoiceWordBytes = 64;
constexpr size_t kMaxVariableBytes = 24;

std::optional<K> KindFromName(std::string_view name) {
  for (size_t i = 0; i < kVoiceCommandKindCount; ++i) {
    const auto kind = static_cast<K>(i);
    if (VoiceCommandKindName(kind) == name) {
      return kind;
    }
  }
  return std::nullopt;
}

StepType StepTypeFor(K kind) {
  switch (kind) {
    case K::kOpenSite:
      return StepType::NAVIGATE;
    case K::kChoose:
      return StepType::CLICK;
    case K::kSearch:
    case K::kType:
      return StepType::TYPE;
    case K::kScrollDown:
    case K::kScrollUp:
    case K::kScrollLeft:
    case K::kScrollRight:
    case K::kScrollToTop:
    case K::kScrollToBottom:
      return StepType::SCROLL;
    default:
      return StepType::UNKNOWN;
  }
}

bool HasTemplate(std::string_view text) {
  return text.find("{{") != std::string_view::npos ||
         text.find("}}") != std::string_view::npos;
}

// Words the user said, as the grammar cleans them.
bool ValidText(const std::string& text, size_t cap) {
  return !text.empty() && text.size() <= cap && IsValidUtf8(text) &&
         CleanSpokenText(text, cap) == text && !HasTemplate(text);
}

// The grammar's host-like text: letters, digits, '.', '-', '_' and '/'.
bool ValidSite(const std::string& site) {
  if (site.empty() || site.size() > kMaxSiteBytes) {
    return false;
  }
  for (char c : site) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '.' || c == '-' || c == '_' || c == '/';
    if (!ok) {
      return false;
    }
  }
  return true;
}

bool ValidWord(const std::string& word) {
  return !word.empty() && word.size() <= kMaxChoiceWordBytes &&
         IsValidUtf8(word) && word.find(' ') == std::string::npos &&
         FoldForComparison(word) == word;
}

bool ValidVariable(const std::string& name) {
  if (name.empty() || name.size() > kMaxVariableBytes || name[0] < 'a' ||
      name[0] > 'z') {
    return false;
  }
  for (char c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
      return false;
    }
  }
  return true;
}

bool ValidRoutineUrl(const std::string& url) {
  if (url.empty() || url.size() > kMaxRoutineUrlBytes || HasTemplate(url)) {
    return false;
  }
  const GURL g(url);
  return g.is_valid() && g.SchemeIsHTTPOrHTTPS() && !g.host().empty();
}

constexpr std::pair<SearchScope, std::string_view> kScopes[] = {
    {SearchScope::kAuto, "auto"},
    {SearchScope::kCurrentSite, "site"},
    {SearchScope::kDefaultEngine, "web"},
    {SearchScope::kNamedSite, "named"},
};

constexpr std::pair<ChoiceVerb, std::string_view> kVerbs[] = {
    {ChoiceVerb::kDefault, "default"}, {ChoiceVerb::kOpen, "open"},
    {ChoiceVerb::kPlay, "play"},       {ChoiceVerb::kClick, "click"},
    {ChoiceVerb::kSelect, "select"},   {ChoiceVerb::kRead, "read"},
    {ChoiceVerb::kDescribe, "describe"},
};

constexpr std::pair<ScrollAmount, std::string_view> kAmounts[] = {
    {ScrollAmount::kDefault, "default"},
    {ScrollAmount::kPage, "page"},
    {ScrollAmount::kHalfPage, "half"},
};

template <typename E, size_t N>
std::string_view NameOf(const std::pair<E, std::string_view> (&table)[N],
                        E value) {
  for (const auto& [e, name] : table) {
    if (e == value) {
      return name;
    }
  }
  return table[0].second;
}

template <typename E, size_t N>
std::optional<E> ValueOf(const std::pair<E, std::string_view> (&table)[N],
                         const std::string* name) {
  if (!name) {
    return std::nullopt;
  }
  for (const auto& [e, n] : table) {
    if (n == *name) {
      return e;
    }
  }
  return std::nullopt;
}

base::ListValue WordsValue(const std::vector<std::string>& words) {
  base::ListValue list;
  for (const std::string& word : words) {
    list.Append(word);
  }
  return list;
}

// A list of valid words, at most |*budget| in all; false for anything else.
bool ReadWords(const base::DictValue& d,
               const char* key,
               size_t* budget,
               std::vector<std::string>* out) {
  const base::Value* value = d.Find(key);
  if (!value) {
    return true;
  }
  if (!value->is_list()) {
    return false;
  }
  for (const base::Value& word : value->GetList()) {
    if (!word.is_string() || !ValidWord(word.GetString()) || *budget == 0) {
      return false;
    }
    --*budget;
    out->push_back(word.GetString());
  }
  return true;
}

std::optional<bool> OptionalBool(const base::DictValue& d, const char* key) {
  const base::Value* value = d.Find(key);
  if (!value) {
    return false;
  }
  if (!value->is_bool()) {
    return std::nullopt;
  }
  return value->GetBool();
}

// An int in [low, high], or |fallback| when absent; nullopt otherwise.
std::optional<int> OptionalInt(const base::DictValue& d,
                               const char* key,
                               int low,
                               int high,
                               int fallback) {
  const base::Value* value = d.Find(key);
  if (!value) {
    return fallback;
  }
  if (!value->is_int() || value->GetInt() < low || value->GetInt() > high) {
    return std::nullopt;
  }
  return value->GetInt();
}

// A text field: required (unless |optional|) and valid.
bool ReadText(const base::DictValue& d,
              const char* key,
              size_t cap,
              bool optional,
              std::string* out) {
  const base::Value* value = d.Find(key);
  if (!value) {
    return optional;
  }
  if (!value->is_string() || !ValidText(value->GetString(), cap)) {
    return false;
  }
  *out = value->GetString();
  return true;
}

std::string Capitalized(std::string text) {
  if (!text.empty() && text[0] >= 'a' && text[0] <= 'z') {
    text[0] = static_cast<char>(text[0] - 'a' + 'A');
  }
  return text;
}

bool TakesText(K kind) {
  return kind == K::kSearch || kind == K::kType || kind == K::kFindInPage;
}

// ---- The planner's steps ----------------------------------------------------

using A = PlannerStep::Action;

// extra["voice"]'s keys for the planner's step (with kText, kQuery,
// kVariable and kPlanned).
constexpr char kAgent[] = "agent";
constexpr char kKind[] = "kind";
constexpr char kLabel[] = "label";
constexpr char kOption[] = "option";
constexpr char kDirection[] = "direction";

constexpr std::pair<A, std::string_view> kAgentActions[] = {
    {A::kNavigate, "navigate"}, {A::kSearch, "search"}, {A::kClick, "click"},
    {A::kType, "type"},         {A::kSelect, "select"}, {A::kScroll, "scroll"},
};

// The kinds the agent's observation gives an element: the element
// snapshot's and the page map's (agent_actions.h, agent_page_map.h).
constexpr std::string_view kElementKinds[] = {
    "link",     "button",   "input",  "textarea",  "select",   "checkbox",
    "radio",    "editable", "switch", "option",    "menuitem", "tab",
    "treeitem", "slider",   "media",  "clickable",
};

// What the browser says when a step of the planner's can't be kept.
constexpr char kUnkeptSecretAddress[] =
    "a page the AI opened looks like it holds a password or a code.";
constexpr char kUnkeptSecretSearch[] =
    "a search the AI made looks like it holds a password or a code.";
constexpr char kUnkeptCredentialField[] =
    "the AI typed into a password, card or code field, which I never keep.";
constexpr char kUnkeptNoName[] =
    "the AI used something on the page without a name, which I couldn't "
    "find again.";
constexpr char kUnkeptStep[] = "one of the AI's steps can't be repeated.";

StepType StepTypeForAgent(A action) {
  switch (action) {
    case A::kNavigate:
    case A::kSearch:
      return StepType::NAVIGATE;
    case A::kClick:
    case A::kSelect:
      return StepType::CLICK;
    case A::kType:
      return StepType::TYPE;
    case A::kScroll:
      return StepType::SCROLL;
    case A::kNone:
    case A::kUnkept:
      break;
  }
  return StepType::UNKNOWN;
}

bool ElementAction(A action) {
  return action == A::kClick || action == A::kType || action == A::kSelect;
}

size_t CodePoints(std::string_view text) {
  return static_cast<size_t>(std::count_if(
      text.begin(), text.end(),
      [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

// Page text or the model's, as the planner's step keeps it: valid UTF-8,
// at most |max_chars| code points, no control character (|breaks|: but a
// line break or a tab), nothing a workflow would read as a variable.
bool PlannerText(std::string_view text,
                 size_t max_chars,
                 bool allow_empty,
                 bool breaks) {
  if (text.empty()) {
    return allow_empty;
  }
  if (!IsValidUtf8(text) || CodePoints(text) > max_chars || HasTemplate(text)) {
    return false;
  }
  for (char ch : text) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if ((c < 0x20 || c == 0x7F) && !(breaks && (c == '\n' || c == '\t'))) {
      return false;
    }
  }
  return true;
}

bool KnownElementKind(std::string_view kind) {
  return std::find(std::begin(kElementKinds), std::end(kElementKinds), kind) !=
         std::end(kElementKinds);
}

// The planner's address: http(s), within the agent's cap.
bool ValidPlannerUrl(const std::string& url) {
  return url.size() <= kMaxPlannerUrlBytes && ValidRoutineUrl(url);
}

std::string HostOfUrl(const std::string& url) {
  return HintHost(GURL(url).host());
}

// The draft's quotes (agent_run_draft.cc): “...”, the text's own double
// quotes as single ones.
std::string Quoted(std::string_view shown) {
  std::string q(shown);
  std::replace(q.begin(), q.end(), '"', '\'');
  return "\xE2\x80\x9C" + q + "\xE2\x80\x9D";
}

// The studio's description of the planner's step, as a draft's.
std::string PlannerDescription(const RoutineStep& step) {
  const PlannerStep& a = step.agent;
  const std::string label = Quoted(automation::agent_draft::CleanDisplayText(
      a.label, kMaxPlannerLabelChars));
  switch (a.action) {
    case A::kNavigate:
      return "Open " + step.host;
    case A::kSearch:
      return "Search the web for " +
             Quoted(automation::agent_draft::CleanDisplayText(a.query, 40));
    case A::kClick:
      return "Click " + label;
    case A::kType:
      return step.variable.empty()
                 ? "Type " +
                       Quoted(automation::agent_draft::CleanDisplayText(a.text,
                                                                        40)) +
                       " in " + label
                 : "Type your " + label + " (asked at each run)";
    case A::kSelect:
      return "Choose " +
             Quoted(automation::agent_draft::CleanDisplayText(
                 a.option, kMaxPlannerOptionChars)) +
             " in " + label;
    case A::kScroll:
      return a.direction == "up" ? "Scroll up" : "Scroll down";
    case A::kNone:
    case A::kUnkept:
      break;
  }
  return std::string();
}

// The planner's step as extra["voice"] holds it.
base::DictValue AgentStepToDict(const RoutineStep& step) {
  const PlannerStep& a = step.agent;
  base::DictValue d;
  d.Set(kAgent, NameOf(kAgentActions, a.action));
  switch (a.action) {
    case A::kSearch:
      d.Set(kQuery, a.query);
      break;
    case A::kClick:
    case A::kType:
    case A::kSelect:
      d.Set(kKind, a.kind);
      d.Set(kLabel, a.label);
      if (a.action == A::kSelect) {
        d.Set(kOption, a.option);
      } else if (a.action == A::kType) {
        if (step.variable.empty()) {
          d.Set(kText, a.text);
        } else {
          d.Set(kVariable, step.variable);
        }
      }
      break;
    case A::kScroll:
      d.Set(kDirection, a.direction == "up" ? "up" : "down");
      break;
    case A::kNavigate:
    case A::kNone:
    case A::kUnkept:
      break;
  }
  d.Set(kPlanned, true);
  return d;
}

}  // namespace

BuiltRoutine::BuiltRoutine() = default;
BuiltRoutine::~BuiltRoutine() = default;
BuiltRoutine::BuiltRoutine(BuiltRoutine&&) = default;
BuiltRoutine& BuiltRoutine::operator=(BuiltRoutine&&) = default;

ReadRoutineResult::ReadRoutineResult() = default;
ReadRoutineResult::~ReadRoutineResult() = default;
ReadRoutineResult::ReadRoutineResult(ReadRoutineResult&&) = default;
ReadRoutineResult& ReadRoutineResult::operator=(ReadRoutineResult&&) =
    default;

base::DictValue VoiceCommandToDict(const RoutineStep& step) {
  const VoiceCommand& c = step.command;
  base::DictValue d;
  d.Set(kCommand, VoiceCommandKindName(c.kind));
  const bool withheld = !step.variable.empty();
  switch (c.kind) {
    case K::kOpenSite:
      d.Set(kSite, c.site);
      d.Set(kAddress, c.site_is_address);
      if (!c.site_words.empty()) {
        d.Set(kWords, c.site_words);
      }
      if (c.new_tab) {
        d.Set(kNewTab, true);
      }
      break;
    case K::kSearch:
      if (!withheld) {
        d.Set(kQuery, c.text);
      }
      d.Set(kScope, NameOf(kScopes, c.scope));
      if (c.scope == SearchScope::kNamedSite) {
        d.Set(kSite, c.site);
        d.Set(kAddress, c.site_is_address);
        if (!c.site_words.empty()) {
          d.Set(kWords, c.site_words);
        }
      }
      if (!withheld && !c.whole_query.empty()) {
        d.Set(kWholeQuery, c.whole_query);
      }
      break;
    case K::kFindInPage:
    case K::kType:
      if (!withheld) {
        d.Set(kText, c.text);
      }
      break;
    case K::kChoose:
      d.Set(kVerb, NameOf(kVerbs, c.verb));
      if (c.choice.kind == ChoiceRef::Kind::kPosition) {
        d.Set(kBy, "position");
        d.Set(kOrdinal, c.choice.ordinal);
        if (c.choice.from_end) {
          d.Set(kFromEnd, true);
        }
      } else {
        d.Set(kBy, "name");
        if (!c.choice.words.empty()) {
          d.Set(kWords, WordsValue(c.choice.words));
        }
        if (!c.choice.by_words.empty()) {
          d.Set(kByWords, WordsValue(c.choice.by_words));
        }
        if (!c.choice.from_words.empty()) {
          d.Set(kFromWords, WordsValue(c.choice.from_words));
        }
        if (c.choice.ordinal > 0) {
          d.Set(kOrdinal, c.choice.ordinal);
        }
      }
      if (c.choice.roles != 0) {
        d.Set(kRoles, static_cast<int>(c.choice.roles));
      }
      break;
    case K::kScrollDown:
    case K::kScrollUp:
    case K::kScrollLeft:
    case K::kScrollRight:
      d.Set(kAmount, NameOf(kAmounts, c.scroll));
      d.Set(kCount, c.count);
      break;
    case K::kSkipForward:
    case K::kSkipBack:
      d.Set(kSeconds, c.seconds);
      break;
    default:
      break;
  }
  if (withheld) {
    d.Set(kVariable, step.variable);
  }
  if (step.planned) {
    d.Set(kPlanned, true);
  }
  return d;
}

std::optional<RoutineStep> VoiceCommandFromDict(const base::DictValue& d) {
  const std::string* name = d.FindString(kCommand);
  const std::optional<K> kind = name ? KindFromName(*name) : std::nullopt;
  if (!kind || !IsRoutineKind(*kind)) {
    return std::nullopt;
  }
  RoutineStep step;
  VoiceCommand& c = step.command;
  c.kind = *kind;
  const std::optional<bool> planned = OptionalBool(d, kPlanned);
  if (!planned) {
    return std::nullopt;
  }
  step.planned = *planned;
  if (const base::Value* variable = d.Find(kVariable)) {
    if (!TakesText(c.kind) || !variable->is_string() ||
        !ValidVariable(variable->GetString())) {
      return std::nullopt;
    }
    step.variable = variable->GetString();
  }
  const bool withheld = !step.variable.empty();
  auto read_site = [&]() {
    const std::string* site = d.FindString(kSite);
    const std::optional<bool> address = OptionalBool(d, kAddress);
    if (!site || !ValidSite(*site) || !address) {
      return false;
    }
    c.site = *site;
    c.site_is_address = *address;
    return ReadText(d, kWords, kMaxCommandTextBytes, /*optional=*/true,
                    &c.site_words);
  };
  switch (c.kind) {
    case K::kOpenSite: {
      const std::optional<bool> new_tab = OptionalBool(d, kNewTab);
      if (!read_site() || !new_tab) {
        return std::nullopt;
      }
      c.new_tab = *new_tab;
      break;
    }
    case K::kSearch: {
      const std::optional<SearchScope> scope =
          ValueOf(kScopes, d.FindString(kScope));
      if (!scope) {
        return std::nullopt;
      }
      c.scope = *scope;
      if (withheld ? d.Find(kQuery) != nullptr
                   : !ReadText(d, kQuery, kMaxCommandTextBytes,
                               /*optional=*/false, &c.text)) {
        return std::nullopt;
      }
      if (c.scope == SearchScope::kNamedSite && !read_site()) {
        return std::nullopt;
      }
      if (!ReadText(d, kWholeQuery, kMaxUtteranceBytes, /*optional=*/true,
                    &c.whole_query) ||
          (withheld && !c.whole_query.empty())) {
        return std::nullopt;
      }
      break;
    }
    case K::kFindInPage:
    case K::kType:
      if (withheld ? d.Find(kText) != nullptr
                   : !ReadText(d, kText, kMaxCommandTextBytes,
                               /*optional=*/false, &c.text)) {
        return std::nullopt;
      }
      break;
    case K::kChoose: {
      const std::optional<ChoiceVerb> verb =
          ValueOf(kVerbs, d.FindString(kVerb));
      const std::string* by = d.FindString(kBy);
      const std::optional<int> roles =
          OptionalInt(d, kRoles, 0, kMaxRoles - 1, 0);
      if (!verb || !by || !roles) {
        return std::nullopt;
      }
      c.verb = *verb;
      ChoiceRef& ref = c.choice;
      ref.roles = static_cast<ChoiceRoleMask>(*roles);
      if (*by == "position") {
        const std::optional<int> ordinal =
            OptionalInt(d, kOrdinal, 1, kMaxOrdinal, 0);
        const std::optional<bool> from_end = OptionalBool(d, kFromEnd);
        if (!ordinal || *ordinal < 1 || !from_end) {
          return std::nullopt;
        }
        ref.kind = ChoiceRef::Kind::kPosition;
        ref.ordinal = *ordinal;
        ref.from_end = *from_end;
      } else if (*by == "name") {
        size_t budget = kMaxRoutineChoiceWords;
        const std::optional<int> ordinal =
            OptionalInt(d, kOrdinal, 0, kMaxOrdinal, 0);
        if (!ordinal || !ReadWords(d, kWords, &budget, &ref.words) ||
            !ReadWords(d, kByWords, &budget, &ref.by_words) ||
            !ReadWords(d, kFromWords, &budget, &ref.from_words) ||
            budget == kMaxRoutineChoiceWords) {
          return std::nullopt;
        }
        ref.kind = ChoiceRef::Kind::kDescription;
        ref.ordinal = *ordinal;
      } else {
        return std::nullopt;
      }
      break;
    }
    case K::kScrollDown:
    case K::kScrollUp:
    case K::kScrollLeft:
    case K::kScrollRight: {
      const std::optional<ScrollAmount> amount =
          d.Find(kAmount) ? ValueOf(kAmounts, d.FindString(kAmount))
                          : std::optional<ScrollAmount>(ScrollAmount::kDefault);
      const std::optional<int> count =
          OptionalInt(d, kCount, 1, kMaxScrollCount, 1);
      if (!amount || !count) {
        return std::nullopt;
      }
      c.scroll = *amount;
      c.count = *count;
      break;
    }
    case K::kSkipForward:
    case K::kSkipBack: {
      const std::optional<int> seconds =
          OptionalInt(d, kSeconds, 0, kMaxSkipSeconds, 0);
      if (!seconds) {
        return std::nullopt;
      }
      c.seconds = *seconds;
      break;
    }
    default:
      break;
  }
  return step;
}

automation::Step PlannerStepToWorkflowStep(const RoutineStep& step) {
  Step st;
  st.type = StepTypeForAgent(step.agent.action);
  if (step.agent.action == A::kNavigate || step.agent.action == A::kSearch) {
    st.target = step.url;
  }
  if (!step.variable.empty()) {
    st.value = "{{" + step.variable + "}}";
  } else if (step.agent.action == A::kScroll) {
    st.value = step.agent.direction == "up" ? "up" : "down";
  }
  st.description = PlannerDescription(step);
  base::DictValue extra;
  extra.Set(automation::kVoiceStepKey, AgentStepToDict(step));
  if (ElementAction(step.agent.action) || step.agent.action == A::kScroll) {
    if (!step.host.empty()) {
      extra.Set(automation::kStepHostKey, step.host);
    }
  }
  st.extra = base::Value(std::move(extra));
  return st;
}

std::optional<RoutineStep> PlannerStepFromWorkflowStep(
    const automation::Step& st) {
  const base::DictValue* extra = st.extra.GetIfDict();
  const base::DictValue* d =
      extra ? extra->FindDict(automation::kVoiceStepKey) : nullptr;
  if (!d) {
    return std::nullopt;
  }
  const std::optional<A> action = ValueOf(kAgentActions, d->FindString(kAgent));
  // Only the planner's: "planned" must say so.
  if (!action || d->FindBool(kPlanned) != std::optional<bool>(true) ||
      StepTypeForAgent(*action) != st.type) {
    return std::nullopt;
  }
  RoutineStep step;
  step.planned = true;
  PlannerStep& a = step.agent;
  a.action = *action;
  // The page acted on, for an element or a scroll; a navigation's is the
  // page it opens.
  if (const std::string* host = extra->FindString(automation::kStepHostKey)) {
    step.host = HintHost(*host);
    if (step.host.empty()) {
      return std::nullopt;
    }
  }
  auto text = [&](const char* key, size_t cap, bool allow_empty, bool breaks,
                  std::string* out) {
    const std::string* value = d->FindString(key);
    if (!value || !PlannerText(*value, cap, allow_empty, breaks)) {
      return false;
    }
    *out = *value;
    return true;
  };
  switch (a.action) {
    case A::kNavigate:
    case A::kSearch:
      if (!ValidPlannerUrl(st.target)) {
        return std::nullopt;
      }
      step.url = st.target;
      step.host = HostOfUrl(st.target);
      if (step.host.empty() ||
          (a.action == A::kSearch &&
           !text(kQuery, kMaxPlannerQueryChars, false, false, &a.query))) {
        return std::nullopt;
      }
      break;
    case A::kClick:
    case A::kType:
    case A::kSelect: {
      const std::string* kind = d->FindString(kKind);
      if (!kind || !KnownElementKind(*kind) || step.host.empty() ||
          !text(kLabel, kMaxPlannerLabelChars, false, false, &a.label)) {
        return std::nullopt;
      }
      a.kind = *kind;
      if (a.action == A::kSelect &&
          !text(kOption, kMaxPlannerOptionChars, false, false, &a.option)) {
        return std::nullopt;
      }
      if (a.action == A::kType) {
        const std::string* variable = d->FindString(kVariable);
        if (variable) {
          if (!ValidVariable(*variable) || d->Find(kText)) {
            return std::nullopt;
          }
          step.variable = *variable;
        } else if (!text(kText, kMaxPlannerTextChars, true, true, &a.text)) {
          return std::nullopt;
        }
      }
      break;
    }
    case A::kScroll: {
      const std::string* direction = d->FindString(kDirection);
      if (!direction || (*direction != "down" && *direction != "up")) {
        return std::nullopt;
      }
      a.direction = *direction;
      break;
    }
    case A::kNone:
    case A::kUnkept:
      return std::nullopt;
  }
  return step;
}

RoutineStep UnkeptPlannerStep(std::string why) {
  RoutineStep step;
  step.planned = true;
  step.agent.action = A::kUnkept;
  step.agent.why = why.empty() ? std::string(kUnkeptStep) : std::move(why);
  return step;
}

RoutineStep RoutineStepForPlanner(const PlannerStep& in,
                                  std::string_view goal,
                                  std::string_view page_host) {
  namespace draft = automation::agent_draft;
  if (in.action == A::kUnkept) {
    return UnkeptPlannerStep(in.why);
  }
  RoutineStep step;
  step.planned = true;
  PlannerStep& a = step.agent;
  a.action = in.action;
  // The secrets the user's request gave ("my PIN is 4321"), as a draft
  // reads its goal; a request too long to scan withholds every value.
  const std::vector<std::string> goal_secrets = draft::GoalSecretTokens(goal);
  const bool goal_unscanned = goal.size() > 4096 || goal_secrets.size() > 64;
  auto holds_goal_secret = [&](std::string_view text) {
    return std::any_of(goal_secrets.begin(), goal_secrets.end(),
                       [&](const std::string& secret) {
                         return text.find(secret) != std::string_view::npos;
                       });
  };
  auto looks_secret = [&](std::string_view text) {
    return draft::TextLooksSecret(text) ||
           draft::TextLooksSecret(draft::WithoutHiddenChars(text)) ||
           holds_goal_secret(text);
  };
  switch (in.action) {
    case A::kNavigate:
    case A::kSearch: {
      // The address as the browser writes it (ASCII), as a draft keeps
      // it: what the file holds reads back the same.
      const GURL url(in.url);
      const std::string spec = url.is_valid() ? url.spec() : std::string();
      if (!IsValidUtf8(in.url) || HasTemplate(in.url) ||
          !ValidPlannerUrl(spec)) {
        return UnkeptPlannerStep(kUnkeptStep);
      }
      if (draft::UrlLooksSecret(in.url) || draft::UrlLooksSecret(spec)) {
        return UnkeptPlannerStep(kUnkeptSecretAddress);
      }
      step.url = spec;
      step.host = HostOfUrl(spec);
      if (step.host.empty()) {
        return UnkeptPlannerStep(kUnkeptStep);
      }
      if (in.action == A::kSearch) {
        if (!PlannerText(in.query, kMaxPlannerQueryChars, false, false)) {
          return UnkeptPlannerStep(kUnkeptStep);
        }
        if (goal_unscanned || looks_secret(in.query)) {
          return UnkeptPlannerStep(kUnkeptSecretSearch);
        }
        a.query = in.query;
      }
      return step;
    }
    case A::kClick:
    case A::kType:
    case A::kSelect: {
      step.host = HintHost(in.host.empty() ? page_host : in.host);
      if (step.host.empty() || !KnownElementKind(in.kind)) {
        return UnkeptPlannerStep(kUnkeptStep);
      }
      if (in.label.empty()) {
        return UnkeptPlannerStep(kUnkeptNoName);
      }
      if (!PlannerText(in.label, kMaxPlannerLabelChars, false, false)) {
        return UnkeptPlannerStep(kUnkeptStep);
      }
      a.kind = in.kind;
      a.label = in.label;
      if (in.action == A::kSelect) {
        if (!PlannerText(in.option, kMaxPlannerOptionChars, false, false)) {
          return UnkeptPlannerStep(kUnkeptStep);
        }
        if (looks_secret(in.option)) {
          return UnkeptPlannerStep(kUnkeptSecretSearch);
        }
        a.option = in.option;
        return step;
      }
      if (in.action == A::kType) {
        std::string type;
        for (char c : in.input_type) {
          type.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a')
                                              : c);
        }
        if (in.credential_field || type == "password") {
          return UnkeptPlannerStep(kUnkeptCredentialField);
        }
        if (!PlannerText(in.text, kMaxPlannerTextChars, true, true)) {
          return UnkeptPlannerStep(kUnkeptStep);
        }
        // Withheld, as a draft withholds it: asked for at each run.
        const std::string plain_label = draft::WithoutHiddenChars(in.label);
        if (goal_unscanned || draft::LabelSuggestsSecret(in.label) ||
            draft::LabelSuggestsSecret(plain_label) || looks_secret(in.text)) {
          step.variable = draft::VariableNameFor(plain_label, {});
        } else {
          a.text = in.text;
        }
      }
      return step;
    }
    case A::kScroll:
      step.host = HintHost(in.host.empty() ? page_host : in.host);
      a.direction = in.direction == "up" ? "up" : "down";
      if (in.direction != "up" && in.direction != "down") {
        return UnkeptPlannerStep(kUnkeptStep);
      }
      return step;
    case A::kNone:
    case A::kUnkept:
      break;
  }
  return UnkeptPlannerStep(kUnkeptStep);
}

bool RoutineSitesConfirmed(const automation::Script& script) {
  if (script.security.sites_unconfirmed) {
    return false;
  }
  const bool planned =
      std::any_of(script.steps.begin(), script.steps.end(), [](const Step& st) {
        const base::DictValue* extra = st.extra.GetIfDict();
        const base::DictValue* voice =
            extra ? extra->FindDict(automation::kVoiceStepKey) : nullptr;
        return voice && voice->FindBool(kPlanned).value_or(false);
      });
  if (!planned) {
    return true;
  }
  std::vector<std::string> used =
      automation::AutomationSecurity::HostsReferencedBy(script.steps);
  std::vector<std::string> confirmed = script.security.domain_whitelist;
  for (std::string& host : confirmed) {
    std::transform(host.begin(), host.end(), host.begin(), [](char c) {
      return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
  }
  for (auto* list : {&used, &confirmed}) {
    std::sort(list->begin(), list->end());
    list->erase(std::unique(list->begin(), list->end()), list->end());
  }
  return !used.empty() && used == confirmed;
}

bool UnconfirmChangedRoutine(automation::Script& script) {
  if (script.security.sites_unconfirmed || RoutineSitesConfirmed(script)) {
    return false;
  }
  // A draft's rules again (agent_run_draft.h).
  script.security.sites_unconfirmed = true;
  script.security.domain_whitelist.clear();
  script.security.trust = automation::TrustLevel::CASUAL;
  return true;
}

BuiltRoutine BuildRoutine(std::string_view id,
                          std::string_view name,
                          const std::vector<RoutineStep>& steps,
                          int64_t now_unix) {
  BuiltRoutine out;
  if (steps.empty()) {
    out.refusal = "There's nothing to remember yet.";
    return out;
  }
  if (steps.size() > kMaxRoutineSteps) {
    out.refusal = "That's too many steps for one routine.";
    return out;
  }
  const std::string display = RoutineDisplayName(name);
  if (display.empty() || display != name || HasTemplate(display) ||
      automation::agent_draft::TextLooksSecret(display)) {
    out.refusal = "Choose another name for it.";
    return out;
  }
  // A step of the planner's that can't be repeated: nothing is kept.
  for (const RoutineStep& step : steps) {
    if (IsUnkeptStep(step)) {
      out.refusal =
          "I can't remember this: " +
          (step.agent.why.empty() ? std::string(kUnkeptStep) : step.agent.why);
      return out;
    }
  }
  // A routine said on a site starts there, as a recording does; one the
  // planner's run began on a page starts on that page, as a draft does.
  std::vector<RoutineStep> all;
  const RoutineStep& front = steps.front();
  const bool opens =
      IsAgentStep(front) &&
      (front.agent.action == A::kNavigate || front.agent.action == A::kSearch);
  if (!IsTaskStart(front) && !opens && !front.host.empty() &&
      steps.size() < kMaxRoutineSteps) {
    RoutineStep open;
    if (IsAgentStep(front) && ValidPlannerUrl(front.url) &&
        !automation::agent_draft::UrlLooksSecret(front.url) &&
        !HostOfUrl(front.url).empty()) {
      open.planned = true;
      open.agent.action = A::kNavigate;
      open.url = front.url;
      open.host = HostOfUrl(front.url);
    } else {
      open.command.kind = K::kOpenSite;
      open.command.site = front.host;
      open.command.site_is_address = true;
      open.url = "https://" + front.host + "/";
      open.planned = front.planned;
    }
    all.push_back(std::move(open));
  }
  all.insert(all.end(), steps.begin(), steps.end());
  Script script;
  script.id = std::string(id);
  script.name = display;
  script.created_at_unix = now_unix;
  std::set<std::string> taken;
  bool planned = false;
  for (const RoutineStep& said : all) {
    RoutineStep step = said;
    VoiceCommand& c = step.command;
    planned = planned || step.planned;
    if (IsAgentStep(step)) {
      // The planner's step, as RoutineStepForPlanner() kept it: its
      // withheld text gets a name of its own; it must read back as it is.
      if (!step.variable.empty()) {
        if (taken.count(step.variable)) {
          step.variable =
              automation::agent_draft::VariableNameFor(step.variable, taken);
        }
        step.agent.text.clear();
        taken.insert(step.variable);
        script.default_variables[step.variable] = std::string();
        ++out.withheld;
      }
      Step st = PlannerStepToWorkflowStep(step);
      if (!PlannerStepFromWorkflowStep(st)) {
        out.refusal = "One of the steps can't be kept.";
        return out;
      }
      script.steps.push_back(std::move(st));
      continue;
    }
    if (HasTemplate(c.text) || HasTemplate(c.whole_query) ||
        HasTemplate(c.site_words) || HasTemplate(step.url)) {
      out.refusal = "One of the steps holds text I can't keep.";
      return out;
    }
    if (c.kind == K::kOpenSite && !step.url.empty() &&
        automation::agent_draft::UrlLooksSecret(step.url)) {
      out.refusal =
          "One of the pages looks like it holds a password or a code, so I "
          "won't remember it.";
      return out;
    }
    if (c.kind == K::kOpenSite && !step.url.empty() &&
        !ValidRoutineUrl(step.url)) {
      step.url.clear();
    }
    // A value that looks secret is asked for at each run instead.
    if (TakesText(c.kind) && step.variable.empty() &&
        automation::agent_draft::TextLooksSecret(c.text)) {
      step.variable = automation::agent_draft::VariableNameFor(
          c.kind == K::kSearch ? "search" : "text", taken);
      c.text.clear();
      c.whole_query.clear();
    }
    if (!step.variable.empty()) {
      taken.insert(step.variable);
      ++out.withheld;
    }
    if (c.kind == K::kSearch &&
        automation::agent_draft::TextLooksSecret(c.whole_query)) {
      c.whole_query.clear();
    }
    if (c.kind == K::kChoose) {
      std::string words;
      for (const auto* list :
           {&c.choice.words, &c.choice.by_words, &c.choice.from_words}) {
        for (const std::string& word : *list) {
          words += word + " ";
        }
      }
      if (automation::agent_draft::TextLooksSecret(words)) {
        out.refusal =
            "One of the choices looks like a code, so I won't remember it.";
        return out;
      }
    }
    base::DictValue voice = VoiceCommandToDict(step);
    // Everything stored must read back.
    if (!VoiceCommandFromDict(voice)) {
      out.refusal = "One of the steps can't be kept.";
      return out;
    }
    Step st;
    st.type = StepTypeFor(c.kind);
    if (c.kind == K::kOpenSite) {
      st.target = step.url;
    }
    if (!step.variable.empty()) {
      st.value = "{{" + step.variable + "}}";
      script.default_variables[step.variable] = std::string();
    }
    st.description = Capitalized(DescribeRoutineStep(step));
    base::DictValue extra;
    extra.Set(automation::kVoiceStepKey, std::move(voice));
    if (!step.host.empty()) {
      extra.Set(automation::kStepHostKey, step.host);
    }
    st.extra = base::Value(std::move(extra));
    script.steps.push_back(std::move(st));
  }
  automation::SecurityPolicy& policy = script.security;
  policy.require_approval_for = {"form_submit", "payment", "login"};
  if (planned) {
    // The planner chose these steps: a draft's rules (agent_run_draft.h).
    policy.trust = automation::TrustLevel::CASUAL;
    policy.domain_whitelist.clear();
    policy.sites_unconfirmed = true;
  } else {
    // The user said each step, on these sites: as a recording.
    policy.trust = automation::TrustLevel::APPROVED;
    policy.domain_whitelist =
        automation::AutomationSecurity::HostsReferencedBy(script.steps);
  }
  out.script = std::move(script);
  return out;
}

ReadRoutineResult ReadRoutine(const automation::Script& script) {
  ReadRoutineResult result;
  if (script.steps.empty()) {
    result.why = "This routine has no steps.";
    return result;
  }
  if (script.steps.size() > kMaxRoutineSteps) {
    result.why = "This routine is too long to run by voice.";
    return result;
  }
  for (size_t i = 0; i < script.steps.size(); ++i) {
    const Step& st = script.steps[i];
    const std::string why = "Step " + std::to_string(i + 1) +
                            " of this routine isn't one I can do by voice.";
    const base::DictValue* extra = st.extra.GetIfDict();
    const base::DictValue* voice =
        extra ? extra->FindDict(automation::kVoiceStepKey) : nullptr;
    // The planner's step, as the web agent carried it out.
    if (voice && voice->Find(kAgent)) {
      std::optional<RoutineStep> agent = PlannerStepFromWorkflowStep(st);
      if (!agent) {
        result.steps.clear();
        result.why = why;
        return result;
      }
      result.steps.push_back(std::move(*agent));
      continue;
    }
    std::optional<RoutineStep> step =
        voice ? VoiceCommandFromDict(*voice) : std::nullopt;
    if (!step || StepTypeFor(step->command.kind) != st.type) {
      result.steps.clear();
      result.why = why;
      return result;
    }
    if (step->command.kind == K::kOpenSite && !st.target.empty()) {
      if (!ValidRoutineUrl(st.target)) {
        result.steps.clear();
        result.why = why;
        return result;
      }
      step->url = st.target;
    }
    if (const std::string* host =
            extra->FindString(automation::kStepHostKey)) {
      step->host = HintHost(*host);
    }
    result.steps.push_back(std::move(*step));
  }
  // Its sites confirmed, and still the sites its steps use.
  result.needs_sites_confirmed = !RoutineSitesConfirmed(script);
  result.ok = true;
  return result;
}

bool RoutineSitesSayable(const std::vector<std::string>& sites) {
  if (sites.empty()) {
    return false;
  }
  // As VoiceBrowserPrompts::ConfirmDraftSites() joins them: "a, b and c".
  size_t chars = 0;
  for (size_t i = 0; i < sites.size(); ++i) {
    std::string_view site = sites[i];
    if (site.rfind("www.", 0) == 0) {
      site.remove_prefix(4);
    }
    const std::string said = activation::SiteForSpeech(site);
    if (said.empty() || said != site) {
      return false;
    }
    chars += said.size() + (i == 0 ? 0 : i + 1 == sites.size() ? 5 : 2);
    if (chars > kMaxDialogSpokenChars) {
      return false;
    }
  }
  return true;
}

}  // namespace voice
}  // namespace molt_ai
