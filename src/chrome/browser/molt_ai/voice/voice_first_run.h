// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Voice mode offered at the browser's first run
// (docs/design/TOUCHLESS_BROWSING.md §3, principle 1: "That includes ...
// first-run"; §10, Phase 1): behind chrome://flags/#molt-voice-mode, and off
// until the owner decides (kMoltVoiceModeAtFirstRun, §11 decision 5).
//
// With both on, on the browser's first run, in its first normal window
// (never a MoltNet one): voice mode starts listening there and says what it
// is and how to turn it off ("turn off voice mode", which asks first), so a
// person who can only speak can start using the browser at once. Nothing
// is changed in settings: "start with the browser" stays as it was.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_FIRST_RUN_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_FIRST_RUN_H_

class BrowserWindowInterface;

namespace molt_ai {
namespace voice {

// Whether voice mode is offered at first run in this build and run (the two
// features, and the browser's first run).
bool VoiceModeOfferedAtFirstRun();

// Offers it in |browser|'s window when VoiceModeOfferedAtFirstRun(), once
// per run of the browser.
void MaybeOfferVoiceModeAtFirstRun(BrowserWindowInterface* browser);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_FIRST_RUN_H_
