// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Whether anything in the browser captures a microphone, or the screen or
// a window, now (docs/design/TOUCHLESS_BROWSING.md §7.3, "The word's
// channel"): what decides whether the voice gesture's confirmation word may
// be said or shown (voice_gesture_channel.h).
//
//   - Chromium's own record of the media streams it gave out
//     (MediaStreamCaptureIndicator, which also draws the tab's recording
//     indicator): a tab, a window or an extension's page, in any profile,
//     MoltNet's included.
//   - A microphone: getUserMedia() audio (DEVICE_AUDIO_CAPTURE).
//   - The screen or a window: getDisplayMedia() and extensions' desktop
//     capture of a monitor or a window, video or the system's sound. A
//     tab's capture shows only the tab's page (the caption bar is the
//     browser's, outside it) and hears only the tab's sound, so it isn't
//     counted.
//   - Looked at when the watch starts (every frame of every renderer
//     process, so a capture begun before voice mode is seen too) and again
//     whenever the indicator says a capture began or ended; |changed| runs
//     then, if what the watch reports changed.
//
// Not seen: other programs on the computer (their own microphone or screen
// recording); the platform's indicators show those to the user.
//
// UI thread.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_CAPTURE_WATCH_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_CAPTURE_WATCH_H_

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "chrome/browser/media/webrtc/media_stream_capture_indicator.h"

namespace content {
class WebContents;
}  // namespace content

namespace molt_ai {
namespace voice {

class VoiceCaptureWatch : public MediaStreamCaptureIndicator::Observer {
 public:
  explicit VoiceCaptureWatch(base::RepeatingClosure changed);
  VoiceCaptureWatch(const VoiceCaptureWatch&) = delete;
  VoiceCaptureWatch& operator=(const VoiceCaptureWatch&) = delete;
  ~VoiceCaptureWatch() override;

  // Something captures a microphone.
  bool audio_captured() const { return audio_captured_; }
  // Something captures the screen or a window.
  bool screen_captured() const { return screen_captured_; }

  // Looks at every capture again (as the indicator's changes do).
  void Refresh();

 private:
  // A capture began (|audio| or the screen): reported at once, before
  // anything else is drawn or said.
  void Began(bool audio);
  // One ended: looked at again once the indicator is done (it tells its
  // observers before it forgets a closing tab's streams).
  void Ended();
  void RefreshAndReport();

  // MediaStreamCaptureIndicator::Observer:
  void OnIsCapturingAudioChanged(content::WebContents* web_contents,
                                 bool is_capturing_audio) override;
  void OnIsCapturingWindowChanged(content::WebContents* web_contents,
                                  bool is_capturing_window) override;
  void OnIsCapturingDisplayChanged(content::WebContents* web_contents,
                                   bool is_capturing_display) override;

  base::RepeatingClosure changed_;
  scoped_refptr<MediaStreamCaptureIndicator> indicator_;
  base::ScopedObservation<MediaStreamCaptureIndicator,
                          MediaStreamCaptureIndicator::Observer>
      observation_{this};
  bool audio_captured_ = false;
  bool screen_captured_ = false;
  bool refresh_posted_ = false;
  base::WeakPtrFactory<VoiceCaptureWatch> weak_factory_{this};
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_CAPTURE_WATCH_H_
