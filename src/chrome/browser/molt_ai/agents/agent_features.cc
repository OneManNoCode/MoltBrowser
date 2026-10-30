// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/agents/agent_features.h"

namespace molt_ai::agent_features {

// Feature name: "MoltAgentScreenshots".
BASE_FEATURE(kMoltAgentScreenshots, base::FEATURE_ENABLED_BY_DEFAULT);

// Feature name: "MoltAgentSiteTools".
BASE_FEATURE(kMoltAgentSiteTools, base::FEATURE_ENABLED_BY_DEFAULT);

// Feature name: "MoltAgentPlanner".
BASE_FEATURE(kMoltAgentPlanner, base::FEATURE_ENABLED_BY_DEFAULT);

// Feature name: "MoltAgentPageMap".
BASE_FEATURE(kMoltAgentPageMap, base::FEATURE_DISABLED_BY_DEFAULT);

bool AgentScreenshotsEnabled() {
  return base::FeatureList::IsEnabled(kMoltAgentScreenshots);
}

bool AgentSiteToolsEnabled() {
  return base::FeatureList::IsEnabled(kMoltAgentSiteTools);
}

bool AgentPlannerEnabled() {
  return base::FeatureList::IsEnabled(kMoltAgentPlanner);
}

bool AgentPageMapEnabled() {
  return base::FeatureList::IsEnabled(kMoltAgentPageMap);
}

}  // namespace molt_ai::agent_features
