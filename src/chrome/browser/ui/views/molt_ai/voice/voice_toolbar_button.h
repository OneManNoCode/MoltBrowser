// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The toolbar's "Voice" button (behind chrome://flags/#molt-voice-mode):
// turns listening on and off in its window (molt_ai/voice/voice_session.h).
// Styled as the AI mode and Agent mode buttons next to it: the violet ring
// while the window listens. For assistive technology it is a toggle button
// named "Voice mode", pressed while the window listens.

#ifndef CHROME_BROWSER_UI_VIEWS_MOLT_AI_VOICE_VOICE_TOOLBAR_BUTTON_H_
#define CHROME_BROWSER_UI_VIEWS_MOLT_AI_VOICE_VOICE_TOOLBAR_BUTTON_H_

#include <optional>
#include <string>

#include "base/memory/raw_ptr.h"
#include "chrome/browser/molt_ai/voice/voice_session.h"
#include "chrome/browser/ui/views/toolbar/toolbar_button.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/base/metadata/metadata_header_macros.h"

class BrowserWindowInterface;

namespace molt_ai {
namespace voice {

class VoiceToolbarButton : public ToolbarButton,
                           public VoiceSession::Observer {
  METADATA_HEADER(VoiceToolbarButton, ToolbarButton)

 public:
  explicit VoiceToolbarButton(BrowserWindowInterface* browser);
  VoiceToolbarButton(const VoiceToolbarButton&) = delete;
  VoiceToolbarButton& operator=(const VoiceToolbarButton&) = delete;
  ~VoiceToolbarButton() override;

  // VoiceSession::Observer:
  void OnVoiceSessionChanged(VoiceSession* session) override;
  void OnVoiceSessionDestroying(VoiceSession* session) override;

 protected:
  // ToolbarButton (as the AI mode button: a blended violet ring, white
  // text):
  bool ShouldBlendHighlightColor() const override;
  std::optional<SkColor> GetHighlightTextColor() const override;
  // views::View: named by its words alone, not the emoji.
  void AdjustAccessibleName(std::u16string& new_name,
                            ax::mojom::NameFrom& name_from) override;
  // views::Button: the pressed state is the window's listening, not the
  // mouse's.
  void UpdateAccessibleCheckedState() override;

 private:
  void OnPressed();
  void Update();

  raw_ptr<BrowserWindowInterface> browser_;
  raw_ptr<VoiceSession> session_ = nullptr;
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_UI_VIEWS_MOLT_AI_VOICE_VOICE_TOOLBAR_BUTTON_H_
