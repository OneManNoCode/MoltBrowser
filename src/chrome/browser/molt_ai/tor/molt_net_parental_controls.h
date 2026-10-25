// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The platform's parental controls, read again each time a MoltNet window
// opens. Chromium reads Windows' parental controls once per run
// (GetWinParentalControls(), cached) and turns Incognito off, and with it
// MoltNet, while they require activity reporting; Windows offers no
// notification when they change, so a change made while the browser runs
// would otherwise only count after a restart. MoltNetWindow asks this before
// it starts Tor for a window (New Identity included), and refuses with
// Incognito's message (molt_net_live_policy.h:
// MoltNetParentalControlsAllowOpen()).

#ifndef CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_PARENTAL_CONTROLS_H_
#define CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_PARENTAL_CONTROLS_H_

#include "base/functional/callback.h"

namespace molt_ai {
namespace tor {

// Runs |done(allowed)| on the calling sequence: whether the parental
// controls let a MoltNet window open now. On Windows they are read on a COM
// thread (MoltReadWinParentalControlsNow()); other platforms have nothing
// to read beyond what Chromium reads (IncognitoModePrefs), and |done| runs
// at once.
void CheckMoltNetParentalControls(base::OnceCallback<void(bool allowed)> done);

}  // namespace tor
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_PARENTAL_CONTROLS_H_
