// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/automation_script.h"

#include <utility>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"

namespace molt_ai {
namespace automation {

namespace {

// Read an optional int field from a Dict, falling back to the default.
int GetInt(const base::DictValue& d, const std::string& key, int fallback) {
  if (auto v = d.FindInt(key))
    return *v;
  return fallback;
}

int64_t GetInt64(const base::DictValue& d,
                 const std::string& key,
                 int64_t fallback) {
  // base::Value JSON ints round-trip as `int` for values that fit in 32 bits;
  // larger timestamps may come back as Double.
  if (auto v = d.FindInt(key))
    return static_cast<int64_t>(*v);
  if (auto v = d.FindDouble(key))
    return static_cast<int64_t>(*v);
  return fallback;
}

std::string GetString(const base::DictValue& d,
                      const std::string& key,
                      const std::string& fallback) {
  if (auto* s = d.FindString(key))
    return *s;
  return fallback;
}

bool GetBool(const base::DictValue& d,
             const std::string& key,
             bool fallback) {
  if (auto v = d.FindBool(key))
    return *v;
  return fallback;
}

std::vector<std::string> GetStringList(const base::DictValue& d,
                                        const std::string& key) {
  std::vector<std::string> out;
  if (const base::ListValue* l = d.FindList(key)) {
    for (const auto& v : *l) {
      if (v.is_string())
        out.push_back(v.GetString());
    }
  }
  return out;
}

}  // namespace

// -----------------------------------------------------------------------------
// Enum string maps
// -----------------------------------------------------------------------------

std::string StepTypeToString(StepType t) {
  switch (t) {
    case StepType::NAVIGATE:    return "navigate";
    case StepType::CLICK:       return "click";
    case StepType::TYPE:        return "type";
    case StepType::SCROLL:      return "scroll";
    case StepType::KEYPRESS:    return "keypress";
    case StepType::WAIT:        return "wait";
    case StepType::WAIT_FOR:    return "wait_for";
    case StepType::EXTRACT:     return "extract";
    case StepType::AI_EXTRACT:  return "ai_extract";
    case StepType::AI_DECIDE:   return "ai_decide";
    case StepType::IF:          return "if";
    case StepType::ELSE:        return "else";
    case StepType::END_IF:      return "end_if";
    case StepType::LOOP:        return "loop";
    case StepType::END_LOOP:    return "end_loop";
    case StepType::ASSERT:      return "assert";
    case StepType::NOTIFY:      return "notify";
    case StepType::SCREENSHOT:  return "screenshot";
    case StepType::RUN_JS:      return "run_js";
    case StepType::RUN_SCRIPT:  return "run_script";
    case StepType::UNKNOWN:     return "unknown";
  }
  return "unknown";
}

StepType StepTypeFromString(const std::string& s) {
  if (s == "navigate")   return StepType::NAVIGATE;
  if (s == "click")      return StepType::CLICK;
  if (s == "type")       return StepType::TYPE;
  if (s == "scroll")     return StepType::SCROLL;
  if (s == "keypress")   return StepType::KEYPRESS;
  if (s == "wait")       return StepType::WAIT;
  if (s == "wait_for")   return StepType::WAIT_FOR;
  if (s == "extract")    return StepType::EXTRACT;
  if (s == "ai_extract") return StepType::AI_EXTRACT;
  if (s == "ai_decide")  return StepType::AI_DECIDE;
  if (s == "if")         return StepType::IF;
  if (s == "else")       return StepType::ELSE;
  if (s == "end_if")     return StepType::END_IF;
  if (s == "loop")       return StepType::LOOP;
  if (s == "end_loop")   return StepType::END_LOOP;
  if (s == "assert")     return StepType::ASSERT;
  if (s == "notify")     return StepType::NOTIFY;
  if (s == "screenshot") return StepType::SCREENSHOT;
  if (s == "run_js")     return StepType::RUN_JS;
  if (s == "run_script") return StepType::RUN_SCRIPT;
  return StepType::UNKNOWN;
}

std::string TriggerTypeToString(TriggerType t) {
  switch (t) {
    case TriggerType::MANUAL:    return "manual";
    case TriggerType::CRON:      return "cron";
    case TriggerType::INTERVAL:  return "interval";
    case TriggerType::AT:        return "at";
    case TriggerType::ON_EVENT:  return "on_event";
  }
  return "manual";
}

TriggerType TriggerTypeFromString(const std::string& s) {
  if (s == "cron")     return TriggerType::CRON;
  if (s == "interval") return TriggerType::INTERVAL;
  if (s == "at")       return TriggerType::AT;
  if (s == "on_event") return TriggerType::ON_EVENT;
  return TriggerType::MANUAL;
}

std::string TrustLevelToString(TrustLevel t) {
  switch (t) {
    case TrustLevel::CASUAL:   return "casual";
    case TrustLevel::APPROVED: return "approved";
    case TrustLevel::TRUSTED:  return "trusted";
    case TrustLevel::ADMIN:    return "admin";
  }
  return "casual";
}

TrustLevel TrustLevelFromString(const std::string& s) {
  if (s == "approved") return TrustLevel::APPROVED;
  if (s == "trusted")  return TrustLevel::TRUSTED;
  if (s == "admin")    return TrustLevel::ADMIN;
  return TrustLevel::CASUAL;
}

// -----------------------------------------------------------------------------
// Step <-> JSON
// -----------------------------------------------------------------------------

namespace {

base::DictValue StepToDict(const Step& s) {
  base::DictValue d;
  d.Set("type", StepTypeToString(s.type));
  if (!s.target.empty())
    d.Set("target", s.target);
  if (!s.selector_fallbacks.empty()) {
    base::ListValue l;
    for (const auto& sel : s.selector_fallbacks) l.Append(sel);
    d.Set("selector_fallbacks", std::move(l));
  }
  if (!s.value.empty())
    d.Set("value", s.value);
  if (!s.store_as.empty())
    d.Set("store_as", s.store_as);
  if (!s.transform.empty())
    d.Set("transform", s.transform);
  if (s.timeout_ms != 5000)
    d.Set("timeout_ms", s.timeout_ms);
  if (s.max_iterations != 100)
    d.Set("max_iterations", s.max_iterations);
  if (s.retries != 0)
    d.Set("retries", s.retries);
  if (!s.extra.is_none())
    d.Set("extra", s.extra.Clone());
  if (!s.description.empty())
    d.Set("description", s.description);
  return d;
}

Step StepFromDict(const base::DictValue& d) {
  Step s;
  s.type = StepTypeFromString(GetString(d, "type", "unknown"));
  s.target = GetString(d, "target", "");
  s.selector_fallbacks = GetStringList(d, "selector_fallbacks");
  s.value = GetString(d, "value", "");
  s.store_as = GetString(d, "store_as", "");
  s.transform = GetString(d, "transform", "");
  s.timeout_ms = GetInt(d, "timeout_ms", 5000);
  s.max_iterations = GetInt(d, "max_iterations", 100);
  s.retries = GetInt(d, "retries", 0);
  if (const base::Value* extra = d.Find("extra"))
    s.extra = extra->Clone();
  s.description = GetString(d, "description", "");
  return s;
}

}  // namespace

// -----------------------------------------------------------------------------
// Trigger <-> JSON
// -----------------------------------------------------------------------------

namespace {

base::DictValue TriggerToDict(const Trigger& t) {
  base::DictValue d;
  d.Set("type", TriggerTypeToString(t.type));
  if (!t.expression.empty())
    d.Set("expression", t.expression);
  if (!t.timezone.empty())
    d.Set("timezone", t.timezone);
  if (t.until_unix > 0)
    d.Set("until_unix", static_cast<double>(t.until_unix));
  if (t.last_fired_unix > 0)
    d.Set("last_fired_unix", static_cast<double>(t.last_fired_unix));
  return d;
}

Trigger TriggerFromDict(const base::DictValue& d) {
  Trigger t;
  t.type = TriggerTypeFromString(GetString(d, "type", "manual"));
  t.expression = GetString(d, "expression", "");
  t.timezone = GetString(d, "timezone", "");
  t.until_unix = GetInt64(d, "until_unix", 0);
  t.last_fired_unix = GetInt64(d, "last_fired_unix", 0);
  return t;
}

}  // namespace

// -----------------------------------------------------------------------------
// SecurityPolicy <-> JSON
// -----------------------------------------------------------------------------

namespace {

base::DictValue SecurityToDict(const SecurityPolicy& p) {
  base::DictValue d;
  if (!p.domain_whitelist.empty()) {
    base::ListValue l;
    for (const auto& dom : p.domain_whitelist) l.Append(dom);
    d.Set("domain_whitelist", std::move(l));
  }
  if (!p.require_approval_for.empty()) {
    base::ListValue l;
    for (const auto& a : p.require_approval_for) l.Append(a);
    d.Set("require_approval_for", std::move(l));
  }
  d.Set("max_runtime_seconds", p.max_runtime_seconds);
  d.Set("max_navigations", p.max_navigations);
  d.Set("max_network_bytes", static_cast<double>(p.max_network_bytes));
  d.Set("trust", TrustLevelToString(p.trust));
  return d;
}

SecurityPolicy SecurityFromDict(const base::DictValue& d) {
  SecurityPolicy p;
  p.domain_whitelist = GetStringList(d, "domain_whitelist");
  p.require_approval_for = GetStringList(d, "require_approval_for");
  p.max_runtime_seconds = GetInt(d, "max_runtime_seconds", 300);
  p.max_navigations = GetInt(d, "max_navigations", 20);
  p.max_network_bytes = GetInt64(d, "max_network_bytes",
                                  200LL * 1024 * 1024);
  p.trust = TrustLevelFromString(GetString(d, "trust", "casual"));
  return p;
}

}  // namespace

// -----------------------------------------------------------------------------
// Stats <-> JSON
// -----------------------------------------------------------------------------

namespace {

base::DictValue StepResultToDict(const StepResult& r) {
  base::DictValue d;
  d.Set("i", r.index);
  d.Set("type", StepTypeToString(r.type));
  d.Set("ok", r.ok);
  d.Set("ms", r.duration_ms);
  if (!r.screenshot.empty()) d.Set("shot", r.screenshot);
  if (!r.description.empty()) d.Set("desc", r.description);
  if (!r.note.empty()) d.Set("note", r.note);
  return d;
}

StepResult StepResultFromDict(const base::DictValue& d) {
  StepResult r;
  r.index = GetInt(d, "i", 0);
  r.type = StepTypeFromString(GetString(d, "type", "unknown"));
  if (auto v = d.FindBool("ok")) r.ok = *v;
  r.duration_ms = GetInt(d, "ms", 0);
  r.screenshot = GetString(d, "shot", "");
  r.description = GetString(d, "desc", "");
  r.note = GetString(d, "note", "");
  return r;
}

base::DictValue RunRecordToDict(const RunRecord& r) {
  base::DictValue d;
  d.Set("ts", static_cast<double>(r.timestamp_unix));
  d.Set("ok", r.success);
  d.Set("dur_ms", r.duration_ms);
  d.Set("steps_executed", r.steps_executed);
  d.Set("total_steps", r.total_steps);
  if (r.ai_tokens > 0) d.Set("ai_tokens", r.ai_tokens);
  if (!r.message.empty()) d.Set("message", r.message);
  if (!r.steps.empty()) {
    base::ListValue l;
    for (const auto& sr : r.steps) l.Append(StepResultToDict(sr));
    d.Set("steps", std::move(l));
  }
  return d;
}

RunRecord RunRecordFromDict(const base::DictValue& d) {
  RunRecord r;
  r.timestamp_unix = GetInt64(d, "ts", 0);
  if (auto v = d.FindBool("ok")) r.success = *v;
  r.duration_ms = GetInt(d, "dur_ms", 0);
  r.steps_executed = GetInt(d, "steps_executed", 0);
  r.total_steps = GetInt(d, "total_steps", 0);
  r.ai_tokens = GetInt(d, "ai_tokens", 0);
  r.message = GetString(d, "message", "");
  if (const base::ListValue* l = d.FindList("steps")) {
    for (const auto& v : *l) {
      if (v.is_dict()) r.steps.push_back(StepResultFromDict(v.GetDict()));
    }
  }
  return r;
}

base::DictValue StatsToDict(const Stats& s) {
  base::DictValue d;
  d.Set("runs", s.runs);
  d.Set("successes", s.successes);
  d.Set("last_run_unix", static_cast<double>(s.last_run_unix));
  d.Set("last_run_duration_ms", s.last_run_duration_ms);
  if (!s.last_result.empty())
    d.Set("last_result", s.last_result);
  // Day 6: extended observability fields. Only emitted when nonzero so
  // older .molt files stay terse.
  if (s.last_failed_step_index >= 0)
    d.Set("last_failed_step_index", s.last_failed_step_index);
  if (s.ai_tokens_total > 0)
    d.Set("ai_tokens_total", static_cast<double>(s.ai_tokens_total));
  if (!s.run_history.empty()) {
    base::ListValue hist;
    for (const auto& r : s.run_history) hist.Append(RunRecordToDict(r));
    d.Set("run_history", std::move(hist));
  }
  return d;
}

Stats StatsFromDict(const base::DictValue& d) {
  Stats s;
  s.runs = GetInt(d, "runs", 0);
  s.successes = GetInt(d, "successes", 0);
  s.last_run_unix = GetInt64(d, "last_run_unix", 0);
  s.last_run_duration_ms = GetInt(d, "last_run_duration_ms", 0);
  s.last_result = GetString(d, "last_result", "");
  s.last_failed_step_index = GetInt(d, "last_failed_step_index", -1);
  s.ai_tokens_total = GetInt64(d, "ai_tokens_total", 0);
  if (const base::ListValue* hist = d.FindList("run_history")) {
    for (const auto& v : *hist) {
      if (v.is_dict()) s.run_history.push_back(RunRecordFromDict(v.GetDict()));
    }
  }
  return s;
}

}  // namespace

// -----------------------------------------------------------------------------
// Script <-> JSON
// -----------------------------------------------------------------------------

base::DictValue Script::ToJSON() const {
  base::DictValue d;
  d.Set("id", id);
  d.Set("name", name);
  d.Set("version", version);
  d.Set("created_at_unix", static_cast<double>(created_at_unix));
  d.Set("ai_model", ai_model);
  d.Set("enabled", enabled);
  d.Set("trigger", TriggerToDict(trigger));

  base::ListValue step_list;
  for (const auto& s : steps) step_list.Append(StepToDict(s));
  d.Set("steps", std::move(step_list));

  d.Set("security", SecurityToDict(security));
  d.Set("stats", StatsToDict(stats));

  // User-editable {{variable}} defaults.
  base::DictValue vars;
  for (const auto& kv : default_variables)
    vars.Set(kv.first, kv.second);
  d.Set("default_variables", std::move(vars));
  return d;
}

std::optional<Script> Script::FromJSON(const base::DictValue& d) {
  Script s;
  s.id = GetString(d, "id", "");
  if (s.id.empty()) {
    // id is the only mandatory field
    return std::nullopt;
  }
  s.name = GetString(d, "name", s.id);
  s.version = GetInt(d, "version", 1);
  s.created_at_unix = GetInt64(d, "created_at_unix", 0);
  s.ai_model = GetString(d, "ai_model", "tinyllama-1.1b");
  s.enabled = GetBool(d, "enabled", true);

  if (const base::DictValue* trig = d.FindDict("trigger"))
    s.trigger = TriggerFromDict(*trig);

  if (const base::ListValue* steps = d.FindList("steps")) {
    for (const auto& v : *steps) {
      if (v.is_dict())
        s.steps.push_back(StepFromDict(v.GetDict()));
    }
  }

  if (const base::DictValue* sec = d.FindDict("security"))
    s.security = SecurityFromDict(*sec);

  if (const base::DictValue* stats = d.FindDict("stats"))
    s.stats = StatsFromDict(*stats);

  if (const base::DictValue* vars = d.FindDict("default_variables")) {
    for (const auto kv : *vars) {
      if (kv.second.is_string())
        s.default_variables[kv.first] = kv.second.GetString();
    }
  }

  return s;
}

std::string Script::ToJSONString() const {
  std::string out;
  base::JSONWriter::WriteWithOptions(
      base::Value(ToJSON()),
      base::JSONWriter::OPTIONS_PRETTY_PRINT,
      &out);
  return out;
}

std::optional<Script> Script::FromJSONString(const std::string& json) {
  auto parsed = base::JSONReader::Read(json,
                                        base::JSON_ALLOW_TRAILING_COMMAS);
  if (!parsed || !parsed->is_dict())
    return std::nullopt;
  return FromJSON(parsed->GetDict());
}

}  // namespace automation
}  // namespace molt_ai
