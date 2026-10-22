// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The web agent's switches (docs/design/AGENT_ACTIONS.md):
//
// Screenshots (§2.6):
//   chrome://flags/#molt-agent-screenshots
//   --disable-features=MoltAgentScreenshots
//
// Enabled by default: when the loaded on-device model can see images, the
// agent shows it the visible viewport (masked and tagged) with each step.
// Nothing changes for a model without vision. Disabled, the agent is
// text-only with every model. Read at each observation, so it applies from
// the next step.
//
// Site tools (§2.7):
//   chrome://flags/#molt-agent-site-tools
//   --disable-features=MoltAgentSiteTools
//
// Enabled by default: on a page that offers WebMCP tools, the agent lists
// them and may call them (read-only ones at once, any other after the
// user's approval). Nothing changes on a page without tools. Disabled, the
// agent never reads or calls a page's tools. Read at each observation.
//
// Planner (§2.8):
//   chrome://flags/#molt-agent-planner
//   --disable-features=MoltAgentPlanner
//
// Enabled by default: before its first action the agent has the on-device
// model write a short plan, follows it step by step, and has it rewritten
// when a step fails. A plan never allows more than the run allows without
// one. Disabled, the agent chooses one action at a time without a plan, and
// its prompts are what they were. Read when a run starts.

#ifndef CHROME_BROWSER_MOLT_AI_AGENTS_AGENT_FEATURES_H_
#define CHROME_BROWSER_MOLT_AI_AGENTS_AGENT_FEATURES_H_

#include "base/feature_list.h"

namespace molt_ai::agent_features {

BASE_DECLARE_FEATURE(kMoltAgentScreenshots);
BASE_DECLARE_FEATURE(kMoltAgentSiteTools);
BASE_DECLARE_FEATURE(kMoltAgentPlanner);

namespace flags {
// chrome://flags internal names (entries in agent_about_flags.h).
inline constexpr char kScreenshots[] = "molt-agent-screenshots";
inline constexpr char kSiteTools[] = "molt-agent-site-tools";
inline constexpr char kPlanner[] = "molt-agent-planner";
}  // namespace flags

bool AgentScreenshotsEnabled();
bool AgentSiteToolsEnabled();
bool AgentPlannerEnabled();

}  // namespace molt_ai::agent_features

#endif  // CHROME_BROWSER_MOLT_AI_AGENTS_AGENT_FEATURES_H_
