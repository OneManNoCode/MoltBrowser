// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The listening side's part of the rest of the browser by voice
// (voice_browser_actions.h's WindowListening): whether a window listens,
// starting to listen in another window when voice mode moves there ("switch
// to the other window", closing this one), and always listening changed by
// voice (after the user's "yes"), through settings.json's one writer, with
// every window's listening following as the settings page's switch makes
// it. The voice loop can't depend on the listening session (which builds on
// it), so this installs itself: InstallWindowListening(), called when a
// window gets its Voice button.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_WINDOW_LISTENING_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_WINDOW_LISTENING_H_

namespace molt_ai {
namespace voice {

// Installs the hooks, once (later calls do nothing).
void InstallWindowListening();

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_WINDOW_LISTENING_H_
