// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// A page's dialogs (alert(), confirm(), prompt(), beforeunload) in a tab
// voice mode listens on (docs/design/TOUCHLESS_BROWSING.md §7.3): the tab's
// own dialog manager (javascript_dialogs::TabModalDialogManager) shows them
// as always; this stands in front of it to tell voice mode that one is
// shown and when it closes, so voice mode reads it and answers it
// (voice_browser_prompts.h).
//
// BrowserWebContentsDelegate::GetJavaScriptDialogManager() (a hook marked
// MoltBrowser) returns For(): this manager while a listener is set for the
// tab, the tab's own otherwise. Everything is passed on unchanged, so
// Chromium's own rules stay (a dialog from a background tab, one while
// another browser dialog shows, the beforeunload rules).
//
// Small and without the voice loop's deps, so the delegate's target can
// depend on it.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_DIALOG_MANAGER_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_DIALOG_MANAGER_H_

#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/molt_ai/voice/voice_prompts.h"
#include "content/public/browser/javascript_dialog_manager.h"
#include "content/public/browser/web_contents_user_data.h"
#include "url/origin.h"

namespace molt_ai {
namespace voice {

class VoiceDialogManager
    : public content::JavaScriptDialogManager,
      public content::WebContentsUserData<VoiceDialogManager> {
 public:
  // A dialog the page shows.
  struct Dialog {
    // kAlert, kConfirm, kTextPrompt or kBeforeUnload.
    PromptKind kind = PromptKind::kAlert;
    // The frame that shows it (a dialog of a frame the page embeds names
    // that frame's origin).
    url::Origin origin;
    std::u16string message;
    std::u16string default_text;
    // beforeunload: the page is being reloaded.
    bool reload = false;
  };

  class Listener {
   public:
    virtual ~Listener() = default;
    // |dialog| shows in |contents| (it waits for an answer).
    virtual void OnDialogShown(content::WebContents* contents,
                               const Dialog& dialog) = 0;
    // The dialog in |contents| closed, answered by anyone.
    virtual void OnDialogClosed(content::WebContents* contents) = 0;
  };

  VoiceDialogManager(const VoiceDialogManager&) = delete;
  VoiceDialogManager& operator=(const VoiceDialogManager&) = delete;
  ~VoiceDialogManager() override;

  // The hook: the manager for |contents|' dialogs, |upstream| being the
  // tab's own.
  static content::JavaScriptDialogManager* For(
      content::WebContents* contents,
      content::JavaScriptDialogManager* upstream);

  // |listener| hears about |contents|' dialogs until RemoveListener().
  static void SetListener(content::WebContents* contents, Listener* listener);
  static void RemoveListener(content::WebContents* contents,
                             Listener* listener);

  // Answers the dialog |contents| shows: OK (|accept|) or Cancel, with
  // |text| for prompt(). False when none shows.
  static bool Answer(content::WebContents* contents,
                     bool accept,
                     const std::u16string& text);

  // content::JavaScriptDialogManager:
  void RunJavaScriptDialog(content::WebContents* web_contents,
                           content::RenderFrameHost* render_frame_host,
                           content::JavaScriptDialogType dialog_type,
                           const std::u16string& message_text,
                           const std::u16string& default_prompt_text,
                           DialogClosedCallback callback,
                           bool* did_suppress_message) override;
  void RunBeforeUnloadDialog(content::WebContents* web_contents,
                             content::RenderFrameHost* render_frame_host,
                             bool is_reload,
                             DialogClosedCallback callback) override;
  bool HandleJavaScriptDialog(content::WebContents* web_contents,
                              bool accept,
                              const std::u16string* prompt_override) override;
  void CancelDialogs(content::WebContents* web_contents,
                     bool reset_state) override;

 private:
  friend class content::WebContentsUserData<VoiceDialogManager>;
  WEB_CONTENTS_USER_DATA_KEY_DECL();

  explicit VoiceDialogManager(content::WebContents* contents);

  // The tab's own manager (it lives as long as the tab).
  content::JavaScriptDialogManager* Upstream();
  // Wraps |callback| so the listener hears the dialog close.
  DialogClosedCallback Watch(DialogClosedCallback callback, int dialog);
  void Closed(int dialog);

  raw_ptr<Listener> listener_ = nullptr;
  raw_ptr<content::JavaScriptDialogManager> upstream_ = nullptr;
  // The dialog showing (0: none), numbered as they come.
  int showing_ = 0;
  int next_dialog_ = 0;
  base::WeakPtrFactory<VoiceDialogManager> weak_factory_{this};
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_DIALOG_MANAGER_H_
