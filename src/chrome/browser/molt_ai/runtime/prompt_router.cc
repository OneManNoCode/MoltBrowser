// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/runtime/prompt_router.h"

#include <algorithm>
#include <regex>

namespace molt_ai {

PromptRouter::PromptRouter() {
  InitializeDefaultRules();
  InitializePIIPatterns();
}

PromptRouter::~PromptRouter() = default;

void PromptRouter::InitializeDefaultRules() {
  rules_ = {
      {TaskType::PAGE_SUMMARY, RouteTarget::LOCAL, "phi-3.5-mini", 2048, 1.0f},
      {TaskType::QA_ABOUT_PAGE, RouteTarget::LOCAL, "llama3.1-8b", 4096, 0.9f},
      {TaskType::AGENT_TASK, RouteTarget::LOCAL, "llama3.1-8b", 4096, 0.8f},
      {TaskType::FORM_FILL, RouteTarget::LOCAL, "tinyllama-1.1b", 1024, 1.0f},
      {TaskType::DATA_EXTRACTION, RouteTarget::LOCAL, "qwen2.5-7b", 2048, 0.9f},
      {TaskType::TRANSLATION, RouteTarget::LOCAL, "qwen2.5-7b", 2048, 0.8f},
      {TaskType::SEARCH, RouteTarget::LOCAL, "phi-3.5-mini", 1024, 0.7f},
      {TaskType::COMPLEX_REASONING, RouteTarget::CLOUD, "llama3.1-8b", 8192, 0.3f},
      {TaskType::CODE_GENERATION, RouteTarget::CLOUD, "qwen2.5-7b", 8192, 0.4f},
      {TaskType::AI_COUNCIL, RouteTarget::HYBRID, "llama3.1-8b", 4096, 0.5f},
      {TaskType::UNKNOWN, RouteTarget::LOCAL, "llama3.1-8b", 4096, 0.6f},
  };
}

void PromptRouter::InitializePIIPatterns() {
  pii_patterns_ = {
      "password",
      "passwd",
      "ssn",
      "social security",
      "credit card",
      "card number",
      "bank account",
      "routing number",
      "passport",
      "driver.?s? license",
      "date of birth",
      "dob",
      "\\b\\d{3}[-.]?\\d{2}[-.]?\\d{4}\\b",  // SSN pattern
      "\\b\\d{4}[- ]?\\d{4}[- ]?\\d{4}[- ]?\\d{4}\\b",  // Credit card
      "\\b[A-Z]{1,2}\\d{6,9}\\b",  // Passport
  };
}

TaskType PromptRouter::ClassifyTask(const std::string& prompt) const {
  std::string lower_prompt = prompt;
  std::transform(lower_prompt.begin(), lower_prompt.end(),
                 lower_prompt.begin(), ::tolower);

  // Simple keyword-based classification
  // TODO: Replace with LLM-based classification for production
  if (lower_prompt.find("summarize") != std::string::npos ||
      lower_prompt.find("summary") != std::string::npos ||
      lower_prompt.find("tldr") != std::string::npos ||
      lower_prompt.find("what is this page about") != std::string::npos) {
    return TaskType::PAGE_SUMMARY;
  }

  if (lower_prompt.find("fill") != std::string::npos &&
      lower_prompt.find("form") != std::string::npos) {
    return TaskType::FORM_FILL;
  }

  if (lower_prompt.find("extract") != std::string::npos ||
      lower_prompt.find("scrape") != std::string::npos ||
      lower_prompt.find("get data") != std::string::npos) {
    return TaskType::DATA_EXTRACTION;
  }

  if (lower_prompt.find("translate") != std::string::npos) {
    return TaskType::TRANSLATION;
  }

  if (lower_prompt.find("search") != std::string::npos ||
      lower_prompt.find("find") != std::string::npos ||
      lower_prompt.find("look up") != std::string::npos) {
    return TaskType::SEARCH;
  }

  if (lower_prompt.find("code") != std::string::npos ||
      lower_prompt.find("function") != std::string::npos ||
      lower_prompt.find("program") != std::string::npos ||
      lower_prompt.find("implement") != std::string::npos) {
    return TaskType::CODE_GENERATION;
  }

  if (lower_prompt.find("browse") != std::string::npos ||
      lower_prompt.find("navigate") != std::string::npos ||
      lower_prompt.find("click") != std::string::npos ||
      lower_prompt.find("agent") != std::string::npos ||
      lower_prompt.find("automate") != std::string::npos) {
    return TaskType::AGENT_TASK;
  }

  if (lower_prompt.find("debate") != std::string::npos ||
      lower_prompt.find("council") != std::string::npos ||
      lower_prompt.find("compare models") != std::string::npos) {
    return TaskType::AI_COUNCIL;
  }

  if (lower_prompt.find("analyze") != std::string::npos ||
      lower_prompt.find("explain") != std::string::npos ||
      lower_prompt.find("reason") != std::string::npos ||
      lower_prompt.find("think") != std::string::npos) {
    return TaskType::COMPLEX_REASONING;
  }

  // Default: treat as Q&A about current page context
  return TaskType::QA_ABOUT_PAGE;
}

RouteTarget PromptRouter::GetRoute(TaskType task_type, bool is_offline) const {
  // Offline mode: always route locally
  if (is_offline) {
    return RouteTarget::LOCAL;
  }

  for (const auto& rule : rules_) {
    if (rule.task_type == task_type) {
      return rule.default_route;
    }
  }

  return RouteTarget::LOCAL;  // Safe default
}

std::string PromptRouter::GetPreferredModel(TaskType task_type) const {
  for (const auto& rule : rules_) {
    if (rule.task_type == task_type) {
      return rule.preferred_model;
    }
  }
  return "llama3.1-8b";
}

bool PromptRouter::ContainsPII(const std::string& prompt) const {
  std::string lower = prompt;
  std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

  for (const auto& pattern : pii_patterns_) {
    // First try simple string match (handles keyword patterns)
    if (lower.find(pattern) != std::string::npos) {
      return true;
    }
    // For regex patterns (containing special chars), use std::regex
    // std::regex constructor is noexcept(false), so only use for
    // patterns that look like regexes (contain backslash or brackets)
    if (pattern.find('\\') != std::string::npos ||
        pattern.find('[') != std::string::npos) {
      std::regex re(pattern, std::regex::icase | std::regex::nosubs);
      if (std::regex_search(lower, re)) {
        return true;
      }
    }
  }
  return false;
}

std::vector<RoutingRule> PromptRouter::GetRoutingRules() const {
  return rules_;
}

void PromptRouter::SetRouteOverride(TaskType task_type, RouteTarget route) {
  for (auto& rule : rules_) {
    if (rule.task_type == task_type) {
      rule.default_route = route;
      return;
    }
  }
}

}  // namespace molt_ai
