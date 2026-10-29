// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Voice mode's switch (docs/design/TOUCHLESS_BROWSING.md):
//   chrome://flags/#molt-voice-mode
//   --enable-features=MoltVoiceMode
//
// Disabled by default while voice mode is a preview: the browser can't
// hear yet, and commands are typed. Enabled, the agent panel
// (chrome://molt-ai-agent) shows a "Type a voice command" field, and a
// command there runs in its window's active tab (voice_controller.h) with
// a spoken answer. MoltNet windows keep voice mode off either way (§7.4).
// Read at each command.
//
// MOLT_VOICE_FEATURE_ENTRIES is expanded inside kFeatureEntries in
// chrome/browser/about_flags.cc next to the other MoltBrowser entries, so
// the upstream file gains only an include and one macro line. It uses
// about_flags.cc's kOsDesktop and FEATURE_VALUE_TYPE.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_CONTROLLER_FEATURES_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_CONTROLLER_FEATURES_H_

#include "base/feature_list.h"

namespace molt_ai::voice {

BASE_DECLARE_FEATURE(kMoltVoiceMode);

namespace flags {
// The chrome://flags internal name.
inline constexpr char kVoiceMode[] = "molt-voice-mode";
}  // namespace flags

bool VoiceModeEnabled();

}  // namespace molt_ai::voice

// clang-format off
#define MOLT_VOICE_FEATURE_ENTRIES                                            \
    {molt_ai::voice::flags::kVoiceMode,                                       \
     "MoltBrowser: voice mode (preview)",                                     \
     "Type what you would say (\"open YouTube\", \"search for ...\", "        \
     "\"play the fourth one\") in the Agent mode panel; the browser does it " \
     "on the page and says the answer aloud. The browser can't hear you "     \
     "yet. Off in MoltNet windows.",                                          \
     kOsDesktop, FEATURE_VALUE_TYPE(molt_ai::voice::kMoltVoiceMode)}
// clang-format on

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_CONTROLLER_FEATURES_H_
