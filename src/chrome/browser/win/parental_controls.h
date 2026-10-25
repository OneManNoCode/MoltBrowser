// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_WIN_PARENTAL_CONTROLS_H_
#define CHROME_BROWSER_WIN_PARENTAL_CONTROLS_H_

#include "base/feature_list.h"

BASE_DECLARE_FEATURE(kEmitWindowsParentalControlsHistograms);

struct WinParentalControls {
  bool any_restrictions = false;
  bool logging_required = false;
  bool web_filter = false;
};

// Calculates and caches the platform parental controls on a worker thread.
void InitializeWinParentalControls();

// Returns a struct of enabled parental controls. This method evaluates and
// caches if the platform controls have been enabled on the first call, which
// requires a thread supporting blocking. Subsequent calls may be from any
// thread.
[[nodiscard]] const WinParentalControls& GetWinParentalControls();

// MoltBrowser: the platform parental controls as they are now, read the way
// GetWinParentalControls() reads them once per run, but not cached
// (chrome/browser/molt_ai/tor/molt_net_parental_controls.h asks each time a
// MoltNet window opens). Blocking: call it on a COM STA thread that may
// block.
[[nodiscard]] WinParentalControls MoltReadWinParentalControlsNow();

#endif  // CHROME_BROWSER_WIN_PARENTAL_CONTROLS_H_
