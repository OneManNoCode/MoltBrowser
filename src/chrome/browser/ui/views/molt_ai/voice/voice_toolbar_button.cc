// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/views/molt_ai/voice/voice_toolbar_button.h"

#include <optional>
#include <string>

#include "base/functional/bind.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/gfx/text_constants.h"
#include "ui/views/accessibility/view_accessibility.h"

namespace molt_ai {
namespace voice {

namespace {

constexpr char16_t kLabel[] = u"\U0001F399 Voice";
constexpr char16_t kAccessibleName[] = u"Voice mode";

}  // namespace

VoiceToolbarButton::VoiceToolbarButton(BrowserWindowInterface* browser)
    : ToolbarButton(base::BindRepeating(&VoiceToolbarButton::OnPressed,
                                        base::Unretained(this))),
      browser_(browser) {
  GetViewAccessibility().SetRole(ax::mojom::Role::kToggleButton);
  GetViewAccessibility().SetCheckedState(ax::mojom::CheckedState::kFalse);
  SetTooltipText(
      u"Voice mode — listen for “Hey Molt” in this window");
  SetHorizontalAlignment(gfx::ALIGN_CENTER);
  session_ = VoiceSession::GetOrCreateForBrowser(browser);
  if (session_) {
    session_->AddObserver(this);
  }
  Update();
}

VoiceToolbarButton::~VoiceToolbarButton() {
  if (session_) {
    session_->RemoveObserver(this);
  }
}

void VoiceToolbarButton::OnVoiceSessionChanged(VoiceSession* session) {
  Update();
}

void VoiceToolbarButton::OnVoiceSessionDestroying(VoiceSession* session) {
  if (session == session_) {
    session_->RemoveObserver(this);
    session_ = nullptr;
  }
  Update();
}

bool VoiceToolbarButton::ShouldBlendHighlightColor() const {
  return true;
}

std::optional<SkColor> VoiceToolbarButton::GetHighlightTextColor() const {
  return SK_ColorWHITE;
}

void VoiceToolbarButton::AdjustAccessibleName(std::u16string& new_name,
                                              ax::mojom::NameFrom& name_from) {
  new_name = kAccessibleName;
}

void VoiceToolbarButton::UpdateAccessibleCheckedState() {}

void VoiceToolbarButton::OnPressed() {
  if (!session_) {
    session_ = VoiceSession::GetOrCreateForBrowser(browser_);
    if (!session_) {
      return;
    }
    session_->AddObserver(this);
  }
  session_->Toggle();
  Update();
}

void VoiceToolbarButton::Update() {
  const bool listening = session_ && session_->listening();
  // The violet ring of the AI and Agent mode buttons while it listens.
  const std::optional<SkColor> violet =
      listening ? std::optional<SkColor>(SkColorSetRGB(0xA7, 0x8B, 0xFA))
                : std::nullopt;
  SetHighlight(kLabel, violet);
  GetViewAccessibility().SetCheckedState(
      listening ? ax::mojom::CheckedState::kTrue
                : ax::mojom::CheckedState::kFalse);
}

BEGIN_METADATA(VoiceToolbarButton)
END_METADATA

}  // namespace voice
}  // namespace molt_ai
