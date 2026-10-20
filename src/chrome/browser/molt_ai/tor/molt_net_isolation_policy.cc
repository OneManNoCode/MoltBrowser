// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/molt_net_isolation_policy.h"

namespace molt_ai {
namespace tor {

ExtensionOrigin ClassifyExtension(bool known, bool is_component_location) {
  if (!known) {
    return ExtensionOrigin::kUnknown;
  }
  return is_component_location ? ExtensionOrigin::kComponent
                               : ExtensionOrigin::kInstalled;
}

bool IsKeptOutOfMoltNet(ExtensionOrigin origin) {
  switch (origin) {
    case ExtensionOrigin::kComponent:
      return false;
    case ExtensionOrigin::kUnknown:
    case ExtensionOrigin::kInstalled:
      return true;
  }
  // Out-of-range values (a bad cast) are treated like an unknown extension.
  return true;
}

CertDecisionMemory::CertDecisionMemory() = default;
CertDecisionMemory::~CertDecisionMemory() = default;

void CertDecisionMemory::Allow(const std::string& host,
                               const std::string& decision_key) {
  allowed_[host].insert(decision_key);
}

bool CertDecisionMemory::IsAllowed(const std::string& host,
                                   const std::string& decision_key) const {
  auto it = allowed_.find(host);
  return it != allowed_.end() && it->second.count(decision_key) > 0;
}

bool CertDecisionMemory::HasAnyFor(const std::string& host) const {
  return allowed_.count(host) > 0;
}

void CertDecisionMemory::Revoke(const std::string& host) {
  allowed_.erase(host);
}

void CertDecisionMemory::Clear() {
  allowed_.clear();
}

}  // namespace tor
}  // namespace molt_ai
