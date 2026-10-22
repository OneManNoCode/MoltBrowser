// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The web agent's screenshots (docs/design/AGENT_ACTIONS.md §2.6):
//
//   chrome://flags/#molt-agent-screenshots
//   --disable-features=MoltAgentScreenshots
//
// Enabled by default: when the loaded on-device model can see images, the
// agent shows it the visible viewport (masked and tagged) with each step.
// Nothing changes for a model without vision. Disabled, the agent is
// text-only with every model. Read at each observation, so it applies from
// the next step.

#ifndef CHROME_BROWSER_MOLT_AI_AGENTS_AGENT_FEATURES_H_
#define CHROME_BROWSER_MOLT_AI_AGENTS_AGENT_FEATURES_H_

#include "base/feature_list.h"

namespace molt_ai::agent_features {

BASE_DECLARE_FEATURE(kMoltAgentScreenshots);

namespace flags {
// chrome://flags internal name (entry in agent_about_flags.h).
inline constexpr char kScreenshots[] = "molt-agent-screenshots";
}  // namespace flags

bool AgentScreenshotsEnabled();

}  // namespace molt_ai::agent_features

#endif  // CHROME_BROWSER_MOLT_AI_AGENTS_AGENT_FEATURES_H_
