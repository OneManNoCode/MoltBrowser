// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// chrome://flags entries for the web agent (agent_features.h): its
// screenshots and its use of site tools. Expanded inside kFeatureEntries in
// chrome/browser/about_flags.cc next to the other MoltBrowser entries, so
// the upstream file only gains an include and one macro line. Both default
// to Enabled and take effect at the agent's next step.
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
     FEATURE_VALUE_TYPE(molt_ai::agent_features::kMoltAgentSiteTools)}
// clang-format on

#endif  // CHROME_BROWSER_MOLT_AI_AGENTS_AGENT_ABOUT_FLAGS_H_
