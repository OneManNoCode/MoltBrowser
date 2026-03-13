// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#ifndef MOLT_AI_RUNTIME_PROMPT_ROUTER_H_
#define MOLT_AI_RUNTIME_PROMPT_ROUTER_H_

#include <string>
#include <vector>

#include "chrome/browser/molt_ai/runtime/browser_ai_runtime.h"

namespace molt_ai {

// Task classification for routing decisions
enum class TaskType {
  PAGE_SUMMARY,
  QA_ABOUT_PAGE,
  AGENT_TASK,
  FORM_FILL,
  COMPLEX_REASONING,
  CODE_GENERATION,
  AI_COUNCIL,
  TRANSLATION,
  SEARCH,
  DATA_EXTRACTION,
  UNKNOWN
};

// Routing rule definition
struct RoutingRule {
  TaskType task_type;
  RouteTarget default_route;
  std::string preferred_model;
  int max_context_tokens;
  float priority;  // Higher = more priority for local routing
};

// ============================================================
// PromptRouter — Intelligent prompt routing engine
// ============================================================
// Determines whether a prompt should be handled locally,
// sent to cloud APIs, or handled in hybrid mode.
//
// Routing Decision Matrix (from V4 spec):
//   Page Summarization → LOCAL
//   Q&A about page    → LOCAL
//   Agent tasks       → LOCAL
//   Form fill         → LOCAL
//   Complex reasoning → CLOUD
//   Code generation   → CLOUD
//   AI Council        → HYBRID
//   Offline           → LOCAL
//
// HARD RULE: PII detected → force LOCAL
// ============================================================
class PromptRouter {
 public:
  PromptRouter();
  ~PromptRouter();

  // Classify a prompt into a task type
  TaskType ClassifyTask(const std::string& prompt) const;

  // Get the routing decision for a task
  RouteTarget GetRoute(TaskType task_type, bool is_offline = false) const;

  // Get the preferred model for a task type
  std::string GetPreferredModel(TaskType task_type) const;

  // Check if prompt contains PII (forces local routing)
  bool ContainsPII(const std::string& prompt) const;

  // Get all routing rules
  std::vector<RoutingRule> GetRoutingRules() const;

  // Override routing for specific task type
  void SetRouteOverride(TaskType task_type, RouteTarget route);

 private:
  std::vector<RoutingRule> rules_;
  std::vector<std::string> pii_patterns_;

  void InitializeDefaultRules();
  void InitializePIIPatterns();
};

}  // namespace molt_ai

#endif  // MOLT_AI_RUNTIME_PROMPT_ROUTER_H_
