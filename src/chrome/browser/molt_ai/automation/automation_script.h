// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Script data model for MoltBrowser web automation.
// A Script captures a recorded user workflow that can be replayed
// on-demand, scheduled via cron, or triggered by browser events.
//
// Wire format: pretty-printed JSON saved to
//   ~/.moltbrowser/automations/<id>.molt
//
// See chrome/browser/molt_ai/automation/README.md for the full spec.

#ifndef CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_SCRIPT_H_
#define CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_SCRIPT_H_

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "base/values.h"

namespace molt_ai {
namespace automation {

// All action verbs an automation script can express. Mirrored to JSON as
// lowercase snake_case (e.g. WAIT_FOR -> "wait_for").
enum class StepType {
  // Navigation & interaction
  NAVIGATE,
  CLICK,
  TYPE,
  SCROLL,
  KEYPRESS,

  // Synchronization
  WAIT,        // sleep N ms
  WAIT_FOR,    // wait until selector matches

  // Data flow
  EXTRACT,     // CSS query -> store as variable
  AI_EXTRACT,  // LLM extracts schema-typed JSON from current page
  AI_DECIDE,   // LLM evaluates a prompt; result drives branching

  // Control flow
  IF,
  ELSE,
  END_IF,
  LOOP,
  END_LOOP,
  ASSERT,

  // Side effects
  NOTIFY,      // OS notification on top-right (per user spec)
  SCREENSHOT,
  RUN_JS,      // arbitrary JS in the active frame (Trusted only)

  // Sentinel
  UNKNOWN,
};

// One step of an automation script.
struct Step {
  StepType type = StepType::UNKNOWN;

  // CSS selector (for CLICK/TYPE/EXTRACT/WAIT_FOR/SCREENSHOT) or URL
  // (for NAVIGATE) or arbitrary string (for KEYPRESS, NOTIFY title, etc.)
  std::string target;

  // Additional candidate selectors tried in order if `target` fails.
  // Recorded by the recorder (data-testid -> id -> name -> aria-label -> path)
  // to give scripts resilience against page redesigns.
  std::vector<std::string> selector_fallbacks;

  // Action-specific value: text to type, JS code, prompt for AI step,
  // notification body, etc.
  std::string value;

  // For EXTRACT / AI_EXTRACT: name of the variable to store the result in.
  // Variables are referenced later as {{name}} in prompt or value fields.
  std::string store_as;

  // Optional JS expression to transform extracted text (e.g. "parseInt").
  std::string transform;

  // Timeout for steps that wait (WAIT_FOR, navigation, AI_DECIDE) and
  // also a hard cap on every other step type — the runner arms a
  // OneShotTimer that fails the step if its dispatched callback hasn't
  // fired by this deadline. Defaults to 5s.
  int timeout_ms = 5000;

  // For LOOP / IF: max iterations or label, etc.
  int max_iterations = 100;

  // Number of times to retry on transient failure (selector miss,
  // network blip). 0 = no retry. Each retry uses exponential backoff
  // starting at 250ms.
  int retries = 0;

  // Free-form extra fields (e.g. "schema" for AI_EXTRACT, "url" for
  // OPEN_TAB if/when added). Stored as a Value so it round-trips JSON
  // without needing a struct field per action.
  base::Value extra;

  // Human-readable description shown in the UI timeline.
  std::string description;
};

// How a script is launched.
enum class TriggerType {
  MANUAL,     // user clicks Run
  CRON,       // standard cron expression
  INTERVAL,   // every N seconds
  AT,         // one-shot ISO timestamp
  ON_EVENT,   // browser_startup, etc.
};

struct Trigger {
  TriggerType type = TriggerType::MANUAL;
  // For CRON: 5-field cron string. For INTERVAL: "<seconds>". For AT:
  // ISO-8601 string. For ON_EVENT: event name.
  std::string expression;
  // IANA timezone name. Empty = browser local time.
  std::string timezone;
  // Unix seconds; 0 = never expires.
  int64_t until_unix = 0;
  // Last time this trigger fired (unix seconds), used by scheduler for
  // missed-run catch-up.
  int64_t last_fired_unix = 0;
};

// Trust level granted to the script. Higher trust = fewer approval prompts.
// Per user spec, all scripts use the user's main profile (login persists)
// regardless of trust level.
enum class TrustLevel {
  CASUAL,    // initial: read-only DOM, navigate within whitelist, no submit
  APPROVED,  // user explicitly approved a sensitive action once
  TRUSTED,   // 5+ successful runs, user marked Trusted; can run headless
  ADMIN,     // user toggled in settings; can use AI freely + chain scripts
};

struct SecurityPolicy {
  // Domains the script may interact with. Auto-derived during recording
  // (set of all hostnames visited). User can edit.
  std::vector<std::string> domain_whitelist;

  // Action types that always require user approval unless trust >= TRUSTED.
  // Defaults: form_submit, payment, login.
  std::vector<std::string> require_approval_for;

  int max_runtime_seconds = 300;
  int max_navigations = 20;
  int64_t max_network_bytes = 200LL * 1024 * 1024;  // 200 MB

  TrustLevel trust = TrustLevel::CASUAL;
};

// One row in Stats::run_history. Capped to last N runs by the runner.
struct RunRecord {
  int64_t timestamp_unix = 0;
  bool success = false;
  int duration_ms = 0;
  int steps_executed = 0;
  int total_steps = 0;
  // Sum of prompt + generated tokens across every AI_DECIDE / AI_EXTRACT
  // step in this run. 0 if no AI steps fired.
  int ai_tokens = 0;
  // 1-line summary ("Completed", "Selector miss on step 4", etc.).
  std::string message;
};

// Run history aggregates surfaced in the manager UI.
struct Stats {
  int runs = 0;
  int successes = 0;
  int64_t last_run_unix = 0;
  int last_run_duration_ms = 0;
  // Short result string ("Cheapest: $720", "Submitted", "Selector failed").
  std::string last_result;
  // Day 6: 0-based index of the step that failed in the most recent run,
  // or -1 if the last run succeeded / the script hasn't run. Used by the
  // "Retry from failed step" button.
  int last_failed_step_index = -1;
  // Day 6: cumulative AI token usage across every run of this script.
  // Useful to spot scripts whose AI_DECIDE/AI_EXTRACT prompts grew too
  // expensive over time.
  int64_t ai_tokens_total = 0;
  // Day 6: ring buffer of the most recent run records (newest first).
  // Capped at kRunHistoryCap by the runner. Drives the per-script
  // sparkline / activity timeline in the manager UI.
  std::vector<RunRecord> run_history;
};

// Maximum run records the runner keeps in Stats::run_history. ~50 is
// enough to surface a useful time-series sparkline without bloating
// the .molt JSON file.
constexpr size_t kRunHistoryCap = 50;

// The full script. Equivalent to one .molt JSON file.
struct Script {
  // Stable opaque ID (e.g. "flight-price-check-bali"). Filename without .molt
  std::string id;
  // Human-readable name shown in the UI.
  std::string name;
  // Schema version. Bumped if we make a breaking change to the format.
  int version = 1;
  int64_t created_at_unix = 0;
  // Default model used for AI_DECIDE / AI_EXTRACT steps. Per user spec we
  // only use free local models — TinyLlama is the default since it ships
  // bundled with MoltBrowser.
  std::string ai_model = "tinyllama-1.1b";

  Trigger trigger;
  std::vector<Step> steps;
  SecurityPolicy security;
  Stats stats;

  // Whether the scheduler should consider this script for firing.
  bool enabled = true;

  // User-editable default values for {{variables}} referenced inside
  // step targets / values. Populated from the manager UI; the runner
  // seeds its variables_ map from this before the first step.
  std::map<std::string, std::string> default_variables;

  // -------- Serialization --------
  base::DictValue ToJSON() const;
  static std::optional<Script> FromJSON(const base::DictValue& d);

  // Pretty-printed JSON suitable for writing to disk.
  std::string ToJSONString() const;
  static std::optional<Script> FromJSONString(const std::string& json);
};

// String <-> enum helpers (used by the JSON layer and the UI).
std::string StepTypeToString(StepType t);
StepType StepTypeFromString(const std::string& s);

std::string TriggerTypeToString(TriggerType t);
TriggerType TriggerTypeFromString(const std::string& s);

std::string TrustLevelToString(TrustLevel t);
TrustLevel TrustLevelFromString(const std::string& s);

}  // namespace automation
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_SCRIPT_H_
