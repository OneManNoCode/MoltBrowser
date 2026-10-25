// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet windows offer no screen, window or tab capture. With no WebRTC
// there (chrome/browser/molt_ai/tor/molt_net_webrtc.h) a capture has nowhere
// to go, and Tor Browser doesn't offer it either. This header says which
// requests that covers and what they are told. It is std plus the mojom
// enums only, so molt_ai_unittests tests it. The browser's check is
// molt_net_display_capture.h; the renderer's is
// third_party/blink/renderer/core/molt_net/molt_net_renderer.h, with the same
// list of types.

#ifndef CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_NET_DISPLAY_CAPTURE_POLICY_H_
#define CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_NET_DISPLAY_CAPTURE_POLICY_H_

#include "third_party/blink/public/mojom/mediastream/media_stream.mojom-shared.h"

namespace molt_ai::fingerprinting {

// Whether a request's audio or video part of `type` captures a screen, a
// window or a tab rather than a camera or a microphone: getDisplayMedia()
// (preferCurrentTab included), getAllScreensMedia(), getUserMedia()'s
// chromeMediaSource forms and a capture's "share this tab instead". Every
// type is listed, so a new one has to be sorted before this compiles.
constexpr bool IsDisplayCaptureType(blink::mojom::MediaStreamType type) {
  using T = blink::mojom::MediaStreamType;
  switch (type) {
    case T::NO_SERVICE:
    case T::DEVICE_AUDIO_CAPTURE:
    case T::DEVICE_VIDEO_CAPTURE:
    case T::NUM_MEDIA_TYPES:
      return false;
    case T::GUM_TAB_AUDIO_CAPTURE:
    case T::GUM_TAB_VIDEO_CAPTURE:
    case T::GUM_DESKTOP_VIDEO_CAPTURE:
    case T::GUM_DESKTOP_AUDIO_CAPTURE:
    case T::DISPLAY_VIDEO_CAPTURE:
    case T::DISPLAY_AUDIO_CAPTURE:
    case T::DISPLAY_VIDEO_CAPTURE_THIS_TAB:
    case T::DISPLAY_VIDEO_CAPTURE_SET:
      return true;
  }
  // Not a known type (a renderer can send any number): count it as one.
  return true;
}

// Whether a request with these audio and video types (NO_SERVICE for a part
// it doesn't ask for) asks for a display capture.
constexpr bool IsDisplayCaptureRequest(blink::mojom::MediaStreamType audio,
                                       blink::mojom::MediaStreamType video) {
  return IsDisplayCaptureType(audio) || IsDisplayCaptureType(video);
}

// What a refused request is told: the browser's answer to a user's Cancel
// in the picker (DesktopMediaPickerDialogView), so getDisplayMedia() rejects
// with NotAllowedError "Permission denied by user", with no picker shown.
inline constexpr blink::mojom::MediaStreamRequestResult kDisplayCaptureRefusal =
    blink::mojom::MediaStreamRequestResult::PERMISSION_DENIED_BY_USER;

}  // namespace molt_ai::fingerprinting

#endif  // CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_NET_DISPLAY_CAPTURE_POLICY_H_
