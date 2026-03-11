// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#ifndef MOLT_AI_SECURITY_ACTION_VALIDATOR_H_
#define MOLT_AI_SECURITY_ACTION_VALIDATOR_H_

#include <string>
#include <vector>
#include <unordered_set>

#include "src/molt_ai/agents/agent_engine.h"

namespace molt_ai {

// Validation result with reason
struct ValidationResult {
  bool is_allowed;
  std::string reason;
  std::string risk_level;  // "safe", "low", "medium", "high", "blocked"
};

// Resource usage limits (ResourceGovernor from V4 spec)
struct ResourceLimits {
  float max_ram_percent = 0.50f;    // 50% of system RAM
  float max_gpu_percent = 0.70f;    // 70% of GPU VRAM
  int max_concurrent_models = 2;
  int max_agent_iterations = 20;
  int max_tabs_per_agent = 5;
  int agent_rate_limit_ms = 1000;   // 1 request/sec per domain
  size_t max_download_bytes = 1073741824;  // 1GB max model download
};

// ============================================================
// ActionValidator — Security layer for AI agent actions
// ============================================================
// All AI agent operations must pass through this validator
// before execution.
//
// Security model:
//   AI Agent → ActionValidator → Browser Execution Engine
//
// Prevents:
//   - Unauthorized credential access
//   - Hidden UI manipulation
//   - Unsafe automation
//   - Resource exhaustion
//   - Data exfiltration
//   - Prompt injection attacks
// ============================================================
class ActionValidator {
 public:
  ActionValidator();
  ~ActionValidator();

  // ---- Action Validation ----

  // Validate an agent action before execution
  ValidationResult ValidateAction(const AgentAction& action,
                                   const std::string& current_url) const;

  // Validate a URL for navigation
  ValidationResult ValidateURL(const std::string& url) const;

  // Validate form data before submission
  ValidationResult ValidateFormSubmission(
      const std::string& form_action,
      const std::vector<std::pair<std::string, std::string>>& fields) const;

  // ---- Domain Control ----

  // Check if a domain is in the allowed list
  bool IsDomainAllowed(const std::string& domain) const;

  // Add domain to allowlist
  void AllowDomain(const std::string& domain);

  // Add domain to blocklist
  void BlockDomain(const std::string& domain);

  // ---- Resource Governance ----

  // Check if resource usage is within limits
  bool CheckResourceLimits() const;

  // Get current resource limits
  ResourceLimits GetResourceLimits() const;

  // Update resource limits
  void SetResourceLimits(const ResourceLimits& limits);

  // ---- Prompt Security ----

  // Check for prompt injection attempts
  bool DetectPromptInjection(const std::string& input) const;

  // Sanitize user input for LLM prompts
  std::string SanitizePromptInput(const std::string& input) const;

  // ---- Model Security ----

  // Verify model file integrity (SHA256 hash)
  bool VerifyModelSignature(const std::string& file_path,
                             const std::string& expected_hash) const;

 private:
  ResourceLimits resource_limits_;
  std::unordered_set<std::string> allowed_domains_;
  std::unordered_set<std::string> blocked_domains_;

  // Hardcoded deny list for sensitive actions (from V4 spec)
  static const std::vector<std::string> kSensitiveURLPatterns;
  static const std::vector<std::string> kSensitiveFormFields;
  static const std::vector<std::string> kPromptInjectionPatterns;

  void InitializeDefaultBlacklists();
  std::string ExtractDomain(const std::string& url) const;
};

}  // namespace molt_ai

#endif  // MOLT_AI_SECURITY_ACTION_VALIDATOR_H_
