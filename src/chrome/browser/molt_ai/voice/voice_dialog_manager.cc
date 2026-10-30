// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_dialog_manager.h"

#include <utility>

#include "base/functional/bind.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"

namespace molt_ai {
namespace voice {

namespace {

PromptKind KindOf(content::JavaScriptDialogType type) {
  switch (type) {
    case content::JAVASCRIPT_DIALOG_TYPE_ALERT:
      return PromptKind::kAlert;
    case content::JAVASCRIPT_DIALOG_TYPE_CONFIRM:
      return PromptKind::kConfirm;
    case content::JAVASCRIPT_DIALOG_TYPE_PROMPT:
      return PromptKind::kTextPrompt;
  }
  return PromptKind::kAlert;
}

}  // namespace

WEB_CONTENTS_USER_DATA_KEY_IMPL(VoiceDialogManager);

VoiceDialogManager::VoiceDialogManager(content::WebContents* contents)
    : content::WebContentsUserData<VoiceDialogManager>(*contents) {}

VoiceDialogManager::~VoiceDialogManager() = default;

// static
content::JavaScriptDialogManager* VoiceDialogManager::For(
    content::WebContents* contents,
    content::JavaScriptDialogManager* upstream) {
  VoiceDialogManager* self =
      contents ? VoiceDialogManager::FromWebContents(contents) : nullptr;
  if (!self || !upstream || (!self->listener_ && !self->showing_)) {
    return upstream;
  }
  self->upstream_ = upstream;
  return self;
}

// static
void VoiceDialogManager::SetListener(content::WebContents* contents,
                                     Listener* listener) {
  if (!contents || !listener) {
    return;
  }
  VoiceDialogManager::CreateForWebContents(contents);
  VoiceDialogManager::FromWebContents(contents)->listener_ = listener;
}

// static
void VoiceDialogManager::RemoveListener(content::WebContents* contents,
                                        Listener* listener) {
  VoiceDialogManager* self =
      contents ? VoiceDialogManager::FromWebContents(contents) : nullptr;
  if (self && self->listener_ == listener) {
    self->listener_ = nullptr;
  }
}

// static
bool VoiceDialogManager::Answer(content::WebContents* contents,
                                bool accept,
                                const std::u16string& text) {
  VoiceDialogManager* self =
      contents ? VoiceDialogManager::FromWebContents(contents) : nullptr;
  if (!self || !self->showing_ || !self->upstream_) {
    return false;
  }
  // Always with the text: the tab's own manager reads the dialog's field
  // otherwise.
  return self->upstream_->HandleJavaScriptDialog(contents, accept, &text);
}

content::JavaScriptDialogManager* VoiceDialogManager::Upstream() {
  return upstream_.get();
}

VoiceDialogManager::DialogClosedCallback VoiceDialogManager::Watch(
    DialogClosedCallback callback,
    int dialog) {
  return base::BindOnce(
      [](base::WeakPtr<VoiceDialogManager> self, int dialog,
         DialogClosedCallback callback, bool success,
         const std::u16string& user_input) {
        if (self) {
          self->Closed(dialog);
        }
        std::move(callback).Run(success, user_input);
      },
      weak_factory_.GetWeakPtr(), dialog, std::move(callback));
}

void VoiceDialogManager::Closed(int dialog) {
  if (showing_ != dialog) {
    return;
  }
  showing_ = 0;
  if (listener_) {
    listener_->OnDialogClosed(&GetWebContents());
  }
}

void VoiceDialogManager::RunJavaScriptDialog(
    content::WebContents* web_contents,
    content::RenderFrameHost* render_frame_host,
    content::JavaScriptDialogType dialog_type,
    const std::u16string& message_text,
    const std::u16string& default_prompt_text,
    DialogClosedCallback callback,
    bool* did_suppress_message) {
  content::JavaScriptDialogManager* upstream = Upstream();
  if (!upstream) {
    *did_suppress_message = true;
    return;
  }
  const int dialog = ++next_dialog_;
  showing_ = dialog;
  upstream->RunJavaScriptDialog(web_contents, render_frame_host, dialog_type,
                                message_text, default_prompt_text,
                                Watch(std::move(callback), dialog),
                                did_suppress_message);
  if (*did_suppress_message) {
    if (showing_ == dialog) {
      showing_ = 0;
    }
    return;
  }
  // Already answered (an alert in a background tab), or nothing to tell.
  if (showing_ != dialog || !listener_) {
    return;
  }
  Dialog shown;
  shown.kind = KindOf(dialog_type);
  shown.origin = render_frame_host ? render_frame_host->GetLastCommittedOrigin()
                                   : url::Origin();
  shown.message = message_text;
  shown.default_text = default_prompt_text;
  listener_->OnDialogShown(web_contents, shown);
}

void VoiceDialogManager::RunBeforeUnloadDialog(
    content::WebContents* web_contents,
    content::RenderFrameHost* render_frame_host,
    bool is_reload,
    DialogClosedCallback callback) {
  content::JavaScriptDialogManager* upstream = Upstream();
  if (!upstream) {
    std::move(callback).Run(true, std::u16string());
    return;
  }
  const int dialog = ++next_dialog_;
  showing_ = dialog;
  upstream->RunBeforeUnloadDialog(web_contents, render_frame_host, is_reload,
                                  Watch(std::move(callback), dialog));
  if (showing_ != dialog || !listener_) {
    return;
  }
  Dialog shown;
  shown.kind = PromptKind::kBeforeUnload;
  shown.origin = render_frame_host ? render_frame_host->GetLastCommittedOrigin()
                                   : url::Origin();
  shown.reload = is_reload;
  listener_->OnDialogShown(web_contents, shown);
}

bool VoiceDialogManager::HandleJavaScriptDialog(
    content::WebContents* web_contents,
    bool accept,
    const std::u16string* prompt_override) {
  content::JavaScriptDialogManager* upstream = Upstream();
  return upstream && upstream->HandleJavaScriptDialog(web_contents, accept,
                                                      prompt_override);
}

void VoiceDialogManager::CancelDialogs(content::WebContents* web_contents,
                                       bool reset_state) {
  if (content::JavaScriptDialogManager* upstream = Upstream()) {
    upstream->CancelDialogs(web_contents, reset_state);
  }
}

}  // namespace voice
}  // namespace molt_ai
