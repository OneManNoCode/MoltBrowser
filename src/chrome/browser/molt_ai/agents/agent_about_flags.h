// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// chrome://flags entries for the web agent (agent_features.h): its
// screenshots, its use of site tools, its planner and its page map. Expanded inside
// kFeatureEntries in chrome/browser/about_flags.cc next to the other
// MoltBrowser entries, so the upstream file only gains an include and one
// macro line. All default to Enabled; the first two take effect at the
// agent's next step, the planner and the page map at its next run.
//
// The macro uses about_flags.cc's kOsAll and FEATURE_VALUE_TYPE.

#ifndef CHROME_BROWSER_MOLT_AI_AGENTS_AGENT_ABOUT_FLAGS_H_
#define CHROME_BROWSER_MOLT_AI_AGENTS_AGENT_ABOUT_FLAGS_H_

#include "chrome/browser/molt_ai/agents/agent_features.h"

// clang-format off
#define MOLT_AGENT_FEATURE_ENTRIES                                            \
    {molt_ai::agent_features::flags::kScreenshots,                            \
     "MoltBrowser: screenshots for the web agent",                            \
     "When the loaded on-device model can see images, the AI agent also "     \
     "shows it the visible part of the page it works on, with password, "     \
     "payment and autofilled fields and embedded frames masked. The image "   \
     "stays on this device. Disabled, the agent reads page text only.",       \
     kOsAll,                                                                  \
     FEATURE_VALUE_TYPE(molt_ai::agent_features::kMoltAgentScreenshots)},     \
    {molt_ai::agent_features::flags::kSiteTools,                              \
     "MoltBrowser: site tools for the web agent",                             \
     "When a page offers tools for AI agents (WebMCP), the AI agent may use " \
     "them instead of clicking and typing: only the page's own tools, on "    \
     "the sites of the task. A tool the site doesn't mark as read-only "      \
     "asks you first. Disabled, the agent never reads or calls a page's "     \
     "tools.",                                                                \
     kOsAll,                                                                  \
     FEATURE_VALUE_TYPE(molt_ai::agent_features::kMoltAgentSiteTools)},       \
    {molt_ai::agent_features::flags::kPlanner,                                \
     "MoltBrowser: planner for the web agent",                                \
     "Before the AI agent acts, the on-device model writes a short plan for " \
     "the task (at most 6 steps), which you see while the agent works. The "  \
     "agent follows it step by step and has it rewritten when a step "        \
     "fails. A plan never lets the agent do more: every action is checked "   \
     "and asks you as before. Disabled, the agent picks one action at a "     \
     "time without a plan.",                                                  \
     kOsAll,                                                                  \
     FEATURE_VALUE_TYPE(molt_ai::agent_features::kMoltAgentPlanner)},        \
    {molt_ai::agent_features::flags::kPageMap,                                \
     "MoltBrowser: accessibility page map for the web agent",                 \
     "Outside MoltNet windows, the AI agent reads pages through their "       \
     "accessibility tree, as voice mode does (numbered items, lists and "     \
     "fields), and clicks, types and chooses through accessibility "          \
     "actions, which pages treat as your own interaction. It never types "    \
     "into password, card or code fields there. Disabled, it reads and "      \
     "acts through page scripts only, except in voice mode's runs.",          \
     kOsAll,                                                                  \
     FEATURE_VALUE_TYPE(molt_ai::agent_features::kMoltAgentPageMap)}
// clang-format on

#endif  // CHROME_BROWSER_MOLT_AI_AGENTS_AGENT_ABOUT_FLAGS_H_
