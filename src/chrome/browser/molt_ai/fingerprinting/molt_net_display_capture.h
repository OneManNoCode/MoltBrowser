// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The browser's half of "no screen, window or tab capture in MoltNet"
// (molt_net_display_capture_policy.h): the boundary, which a renderer that
// skipped its own check (third_party/blink/renderer/core/molt_net/
// molt_net_renderer.h) still meets. Header-only, like molt_net_profile_id.h,
// for its one caller:
//
//   MediaCaptureDevicesDispatcher::ProcessMediaAccessRequest
//     (chrome/browser/media/webrtc/media_capture_devices_dispatcher.cc),
//     which every WebContents delegate hands its media requests to. A
//     refused request never reaches DisplayMediaAccessHandler,
//     DesktopCaptureAccessHandler or TabCaptureAccessHandler, so no picker
//     or prompt opens.
//
// Decided by the profile alone, as WebRTC is: normal and Incognito windows
// are unchanged.

#ifndef CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_NET_DISPLAY_CAPTURE_H_
#define CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_NET_DISPLAY_CAPTURE_H_

#include "chrome/browser/molt_ai/fingerprinting/molt_net_display_capture_policy.h"
#include "chrome/browser/molt_ai/tor/molt_net_profile_id.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/media_stream_request.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"

namespace molt_ai::fingerprinting {

// Whether `request` is answered with kDisplayCaptureRefusal before any
// handler sees it: it asks for a display capture, and its WebContents or
// the frame that sent it (`render_frame_host`, null once gone) belongs to a
// MoltNet profile.
inline bool RefusesDisplayCapture(content::WebContents* web_contents,
                                  content::RenderFrameHost* render_frame_host,
                                  const content::MediaStreamRequest& request) {
  if (!IsDisplayCaptureRequest(request.audio_type, request.video_type)) {
    return false;
  }
  if (web_contents && tor::IsMoltNetProfile(Profile::FromBrowserContext(
                          web_contents->GetBrowserContext()))) {
    return true;
  }
  return render_frame_host && tor::IsMoltNetProfile(Profile::FromBrowserContext(
                                  render_frame_host->GetBrowserContext()));
}

}  // namespace molt_ai::fingerprinting

#endif  // CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_NET_DISPLAY_CAPTURE_H_
