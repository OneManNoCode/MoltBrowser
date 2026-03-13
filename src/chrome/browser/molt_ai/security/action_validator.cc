// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/security/action_validator.h"

#include <algorithm>
#include <fstream>
#include <regex>
#include <sstream>

namespace molt_ai {

// ---- Hardcoded deny lists (from V4 spec) ----

const std::vector<std::string> ActionValidator::kSensitiveURLPatterns = {
    "chrome://settings",
    "chrome://extensions",
    "chrome://flags",
    "chrome://password-manager",
    "about:config",
    "file:///",
    "localhost",
    "127.0.0.1",
    "0.0.0.0",
    "192.168.",
    "10.0.",
    "172.16.",
};

const std::vector<std::string> ActionValidator::kSensitiveFormFields = {
    "password",
    "passwd",
    "pass",
    "credit_card",
    "card_number",
    "cvv",
    "cvc",
    "ssn",
    "social_security",
    "bank_account",
    "routing_number",
    "secret",
    "token",
    "api_key",
    "private_key",
};

const std::vector<std::string> ActionValidator::kPromptInjectionPatterns = {
    "ignore previous instructions",
    "ignore all previous",
    "disregard your instructions",
    "you are now",
    "new instructions:",
    "system prompt:",
    "override:",
    "admin mode:",
    "developer mode:",
    "jailbreak",
    "DAN mode",
    "ignore safety",
    "bypass security",
    "pretend you are",
};

ActionValidator::ActionValidator() {
  InitializeDefaultBlacklists();
}

ActionValidator::~ActionValidator() = default;

void ActionValidator::InitializeDefaultBlacklists() {
  // Block known malicious/dangerous domains
  blocked_domains_ = {
      "malware.example.com",
      // Additional domains added by security team
  };
}

ValidationResult ActionValidator::ValidateAction(
    const AgentAction& action,
    const std::string& current_url) const {
  ValidationResult result;
  result.is_allowed = true;
  result.risk_level = "safe";

  switch (action.type) {
    case ActionType::NAVIGATE: {
      // Validate target URL
      auto url_result = ValidateURL(action.target);
      if (!url_result.is_allowed) {
        return url_result;
      }

      // Check domain allowlist if configured
      std::string domain = ExtractDomain(action.target);
      if (!allowed_domains_.empty() && !IsDomainAllowed(domain)) {
        result.is_allowed = false;
        result.reason = "Domain not in allowlist: " + domain;
        result.risk_level = "blocked";
        return result;
      }
      break;
    }

    case ActionType::FILL_FORM: {
      // Check for sensitive field names
      std::string lower_target = action.target;
      std::transform(lower_target.begin(), lower_target.end(),
                     lower_target.begin(), ::tolower);

      for (const auto& sensitive : kSensitiveFormFields) {
        if (lower_target.find(sensitive) != std::string::npos) {
          result.is_allowed = false;
          result.reason = "Cannot fill sensitive form field: " + sensitive;
          result.risk_level = "blocked";
          return result;
        }
      }
      break;
    }

    case ActionType::TYPE_TEXT: {
      // Don't allow typing into password fields
      std::string lower_target = action.target;
      std::transform(lower_target.begin(), lower_target.end(),
                     lower_target.begin(), ::tolower);

      if (lower_target.find("password") != std::string::npos ||
          lower_target.find("type=\"password\"") != std::string::npos) {
        result.is_allowed = false;
        result.reason = "Cannot type into password fields";
        result.risk_level = "blocked";
        return result;
      }
      break;
    }

    case ActionType::CLICK: {
      // Rate limiting check
      result.risk_level = "low";
      break;
    }

    case ActionType::OPEN_TAB: {
      // Validate URL and check tab limit
      auto url_result = ValidateURL(action.target);
      if (!url_result.is_allowed) {
        return url_result;
      }
      result.risk_level = "low";
      break;
    }

    default:
      break;
  }

  return result;
}

ValidationResult ActionValidator::ValidateURL(const std::string& url) const {
  ValidationResult result;
  result.is_allowed = true;
  result.risk_level = "safe";

  std::string lower_url = url;
  std::transform(lower_url.begin(), lower_url.end(),
                 lower_url.begin(), ::tolower);

  // Check against sensitive URL patterns
  for (const auto& pattern : kSensitiveURLPatterns) {
    if (lower_url.find(pattern) != std::string::npos) {
      result.is_allowed = false;
      result.reason = "Blocked sensitive URL pattern: " + pattern;
      result.risk_level = "blocked";
      return result;
    }
  }

  // Check against blocked domains
  std::string domain = ExtractDomain(url);
  if (blocked_domains_.count(domain)) {
    result.is_allowed = false;
    result.reason = "Domain is blocked: " + domain;
    result.risk_level = "blocked";
    return result;
  }

  // Check for JavaScript URLs (potential XSS)
  if (lower_url.find("javascript:") == 0) {
    result.is_allowed = false;
    result.reason = "JavaScript URLs are not allowed";
    result.risk_level = "high";
    return result;
  }

  // Check for data URLs (potential payload delivery)
  if (lower_url.find("data:") == 0) {
    result.is_allowed = false;
    result.reason = "Data URLs are not allowed for navigation";
    result.risk_level = "high";
    return result;
  }

  return result;
}

ValidationResult ActionValidator::ValidateFormSubmission(
    const std::string& form_action,
    const std::vector<std::pair<std::string, std::string>>& fields) const {
  ValidationResult result;
  result.is_allowed = true;
  result.risk_level = "safe";

  // Validate form action URL
  auto url_result = ValidateURL(form_action);
  if (!url_result.is_allowed) {
    return url_result;
  }

  // Check for sensitive field names in the submission
  for (const auto& [name, value] : fields) {
    std::string lower_name = name;
    std::transform(lower_name.begin(), lower_name.end(),
                   lower_name.begin(), ::tolower);

    for (const auto& sensitive : kSensitiveFormFields) {
      if (lower_name.find(sensitive) != std::string::npos) {
        result.is_allowed = false;
        result.reason = "Form contains sensitive field: " + sensitive;
        result.risk_level = "blocked";
        return result;
      }
    }
  }

  return result;
}

bool ActionValidator::IsDomainAllowed(const std::string& domain) const {
  if (allowed_domains_.empty()) {
    return true;  // No whitelist = allow all (except blocked)
  }
  return allowed_domains_.count(domain) > 0;
}

void ActionValidator::AllowDomain(const std::string& domain) {
  allowed_domains_.insert(domain);
}

void ActionValidator::BlockDomain(const std::string& domain) {
  blocked_domains_.insert(domain);
}

bool ActionValidator::CheckResourceLimits() const {
  // TODO: Integration with system resource monitoring
  // Check current RAM and GPU usage against limits
  return true;
}

ResourceLimits ActionValidator::GetResourceLimits() const {
  return resource_limits_;
}

void ActionValidator::SetResourceLimits(const ResourceLimits& limits) {
  resource_limits_ = limits;
}

bool ActionValidator::DetectPromptInjection(const std::string& input) const {
  std::string lower = input;
  std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

  for (const auto& pattern : kPromptInjectionPatterns) {
    if (lower.find(pattern) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::string ActionValidator::SanitizePromptInput(
    const std::string& input) const {
  std::string sanitized = input;

  // Remove potential prompt injection patterns
  for (const auto& pattern : kPromptInjectionPatterns) {
    std::string lower = sanitized;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    size_t pos;
    while ((pos = lower.find(pattern)) != std::string::npos) {
      sanitized.erase(pos, pattern.length());
      lower.erase(pos, pattern.length());
    }
  }

  // Limit input length to prevent context window overflow
  constexpr size_t kMaxInputLength = 8192;
  if (sanitized.length() > kMaxInputLength) {
    sanitized = sanitized.substr(0, kMaxInputLength);
  }

  return sanitized;
}

bool ActionValidator::VerifyModelSignature(
    const std::string& file_path,
    const std::string& expected_hash) const {
  // TODO: Implement SHA256 hash verification
  // Read file in chunks, compute SHA256, compare with expected
  return true;
}

std::string ActionValidator::ExtractDomain(const std::string& url) const {
  // Simple domain extraction
  std::string domain = url;

  // Remove protocol
  size_t proto_end = domain.find("://");
  if (proto_end != std::string::npos) {
    domain = domain.substr(proto_end + 3);
  }

  // Remove path
  size_t path_start = domain.find('/');
  if (path_start != std::string::npos) {
    domain = domain.substr(0, path_start);
  }

  // Remove port
  size_t port_start = domain.find(':');
  if (port_start != std::string::npos) {
    domain = domain.substr(0, port_start);
  }

  return domain;
}

}  // namespace molt_ai
