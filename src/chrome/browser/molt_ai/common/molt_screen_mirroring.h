// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The browser's own mirroring of the screen or a window to another device:
// Cast's desktop mirroring (chrome/browser/media/
// cast_mirroring_service_host.cc notes each session here). Chromium
// registers a tab's mirroring with MediaStreamCaptureIndicator, which shows
// it on the tab, but not the screen's or a window's, which has no tab to
// show it on; voice mode's capture watch (voice/voice_capture_watch.h)
// counts it as the screen being captured, so the voice gesture's word is
// never shown while the screen goes to a receiver (with, on some platforms,
// the computer's sound).
//
// UI thread. base only.

#ifndef CHROME_BROWSER_MOLT_AI_COMMON_MOLT_SCREEN_MIRRORING_H_
#define CHROME_BROWSER_MOLT_AI_COMMON_MOLT_SCREEN_MIRRORING_H_

#include "base/observer_list_types.h"

namespace molt_ai {

class ScreenMirroringObserver : public base::CheckedObserver {
 public:
  // The browser began, or stopped, mirroring the screen or a window.
  virtual void OnScreenMirroringChanged() = 0;
};

// |session| (the mirroring's host) began, or ended, mirroring the screen
// or a window. A session noted twice, or ended without having begun,
// changes nothing.
void NoteScreenMirroring(const void* session, bool mirroring);

// Whether the browser mirrors the screen or a window now.
bool IsMirroringScreen();

void AddScreenMirroringObserver(ScreenMirroringObserver* observer);
void RemoveScreenMirroringObserver(ScreenMirroringObserver* observer);

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_COMMON_MOLT_SCREEN_MIRRORING_H_
