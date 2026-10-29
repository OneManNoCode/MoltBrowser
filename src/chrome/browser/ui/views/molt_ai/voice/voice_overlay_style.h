// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The voice overlay's sizes, fonts and colours for a view, from the user's
// settings and the view's theme (docs/design/TOUCHLESS_BROWSING.md §5.5).
// Shared by the numbers view and the caption bar; the math is in
// voice_overlay_layout.h.

#ifndef CHROME_BROWSER_UI_VIEWS_MOLT_AI_VOICE_VOICE_OVERLAY_STYLE_H_
#define CHROME_BROWSER_UI_VIEWS_MOLT_AI_VOICE_VOICE_OVERLAY_STYLE_H_

#include "chrome/browser/ui/views/molt_ai/voice/voice_overlay_layout.h"
#include "ui/gfx/font_list.h"

namespace content {
class WebContents;
}  // namespace content

namespace views {
class View;
}  // namespace views

namespace molt_ai {
namespace voice {

struct VoiceOverlayStyle {
  VoiceOverlayStyle();
  VoiceOverlayStyle(const VoiceOverlayStyle&);
  VoiceOverlayStyle& operator=(const VoiceOverlayStyle&);
  ~VoiceOverlayStyle();

  // From the user's text settings: the browser's interface font, which
  // follows the OS's text size where Chromium reads one (Windows' "Text
  // size", the desktop's font on Linux), and the default page font size of
  // |contents| (Settings, Appearance, Font size), the larger of the two;
  // and from |view|'s theme: in forced colours (a Windows contrast theme,
  // or emulated) only the OS's colours.
  static VoiceOverlayStyle For(const views::View& view,
                               content::WebContents* contents);

  // The caption's font at |size| DIPs.
  static gfx::FontList CaptionFontAt(float size);

  OverlaySizes sizes;
  OverlayPalette palette;
  BadgeStyle badge_style;
  // The badges' bold font at each of BadgeScalesFor(badge_style).
  gfx::FontList badge_fonts[kBadgeScaleSteps];
  // The caption's metrics at full size, and the smallest its font may get.
  CaptionMetrics caption_metrics;
  float caption_min_font = 16;
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_UI_VIEWS_MOLT_AI_VOICE_VOICE_OVERLAY_STYLE_H_
