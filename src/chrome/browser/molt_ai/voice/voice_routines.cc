// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_routines.h"

#include <set>
#include <utility>

#include "chrome/browser/molt_ai/automation/agent_run_draft.h"
#include "chrome/browser/molt_ai/automation/automation_security.h"
#include "chrome/browser/molt_ai/automation/automation_voice_steps.h"
#include "chrome/browser/molt_ai/voice/spoken_text.h"
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
  Script script;
  script.id = std::string(id);
  script.name = display;
  script.created_at_unix = now_unix;
  std::set<std::string> taken;
  bool planned = false;
  for (const RoutineStep& said : steps) {
    RoutineStep step = said;
    VoiceCommand& c = step.command;
    planned = planned || step.planned;
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
  result.needs_sites_confirmed = script.security.sites_unconfirmed;
  result.ok = true;
  return result;
}

}  // namespace voice
}  // namespace molt_ai
