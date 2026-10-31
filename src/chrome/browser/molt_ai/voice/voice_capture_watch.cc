// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_capture_watch.h"

#include <set>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/media/webrtc/media_capture_devices_dispatcher.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/web_contents.h"

namespace molt_ai {
namespace voice {

VoiceCaptureWatch::VoiceCaptureWatch(base::RepeatingClosure changed)
    : changed_(std::move(changed)) {
  if (MediaCaptureDevicesDispatcher* dispatcher =
          MediaCaptureDevicesDispatcher::GetInstance()) {
    indicator_ = dispatcher->GetMediaStreamCaptureIndicator();
  }
  if (indicator_) {
    observation_.Observe(indicator_.get());
  }
  Refresh();
}

VoiceCaptureWatch::~VoiceCaptureWatch() = default;

void VoiceCaptureWatch::Refresh() {
  bool audio = false;
  bool screen = false;
  if (indicator_) {
    // Every page with a frame in a renderer: tabs, extensions' pages and
    // the browser's own, in every profile.
    std::set<content::WebContents*> pages;
    for (content::RenderProcessHost::iterator it =
             content::RenderProcessHost::AllHostsIterator();
         !it.IsAtEnd(); it.Advance()) {
      it.GetCurrentValue()->ForEachRenderFrameHost(
          [&pages](content::RenderFrameHost* frame) {
            if (content::WebContents* contents =
                    content::WebContents::FromRenderFrameHost(frame)) {
              pages.insert(contents);
            }
          });
    }
    for (content::WebContents* contents : pages) {
      audio = audio || indicator_->IsCapturingAudio(contents);
      screen = screen || indicator_->IsCapturingWindow(contents) ||
               indicator_->IsCapturingDisplay(contents);
    }
  }
  audio_captured_ = audio;
  screen_captured_ = screen;
}

void VoiceCaptureWatch::RefreshAndReport() {
  refresh_posted_ = false;
  const bool audio = audio_captured_;
  const bool screen = screen_captured_;
  Refresh();
  if ((audio != audio_captured_ || screen != screen_captured_) && changed_) {
    changed_.Run();
  }
}

void VoiceCaptureWatch::Began(bool audio) {
  bool& captured = audio ? audio_captured_ : screen_captured_;
  if (captured) {
    return;
  }
  captured = true;
  if (changed_) {
    changed_.Run();
  }
}

void VoiceCaptureWatch::Ended() {
  if (refresh_posted_) {
    return;
  }
  refresh_posted_ = true;
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&VoiceCaptureWatch::RefreshAndReport,
                                weak_factory_.GetWeakPtr()));
}

void VoiceCaptureWatch::OnIsCapturingAudioChanged(
    content::WebContents* web_contents,
    bool is_capturing_audio) {
  if (is_capturing_audio) {
    Began(/*audio=*/true);
  } else {
    Ended();
  }
}

void VoiceCaptureWatch::OnIsCapturingWindowChanged(
    content::WebContents* web_contents,
    bool is_capturing_window) {
  if (is_capturing_window) {
    Began(/*audio=*/false);
  } else {
    Ended();
  }
}

void VoiceCaptureWatch::OnIsCapturingDisplayChanged(
    content::WebContents* web_contents,
    bool is_capturing_display) {
  if (is_capturing_display) {
    Began(/*audio=*/false);
  } else {
    Ended();
  }
}

}  // namespace voice
}  // namespace molt_ai
