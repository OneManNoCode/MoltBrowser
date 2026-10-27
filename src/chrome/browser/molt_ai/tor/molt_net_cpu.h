// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Whether this computer's processor has what MoltNet needs
// (molt_net_cpu_policy.h), read once per browser run with base::CPU. A
// processor can't change while the browser runs, so the answer is cached.

#ifndef CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_CPU_H_
#define CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_CPU_H_

#include <string>

#include "chrome/browser/molt_ai/tor/molt_net_cpu_policy.h"

namespace molt_ai::tor {

// What base::CPU reports for this computer, as the policy reads it.
MoltNetCpuFeatures ReadMoltNetCpuFeatures();

// Empty if MoltNet may open on this computer's processor; otherwise the one
// sentence to show instead (MoltNetCpuMessage()). Every way into MoltNet
// checks it before it starts Tor, makes a MoltNet profile or opens a
// window.
const std::string& MoltNetCpuError();

}  // namespace molt_ai::tor

#endif  // CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_CPU_H_
