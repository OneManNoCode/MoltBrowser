// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/views/molt_ai/voice/voice_overlay_style.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/common/web_preferences/web_preferences.h"
#include "ui/color/color_id.h"
#include "ui/color/color_provider.h"
#include "ui/color/color_provider_key.h"
#include "ui/gfx/font.h"
#include "ui/gfx/text_utils.h"
#include "ui/native_theme/native_theme.h"
#include "ui/views/view.h"

namespace molt_ai {
namespace voice {

namespace {

// |base| at |size| DIPs (rounded), with |weight|.
gfx::FontList Sized(const gfx::FontList& base,
                    float size,
                    gfx::Font::Weight weight) {
  const int delta = static_cast<int>(std::lround(size)) - base.GetFontSize();
  return base.Derive(delta, gfx::Font::NORMAL, weight);
}

}  // namespace

VoiceOverlayStyle::VoiceOverlayStyle() = default;
VoiceOverlayStyle::VoiceOverlayStyle(const VoiceOverlayStyle&) = default;
VoiceOverlayStyle& VoiceOverlayStyle::operator=(const VoiceOverlayStyle&) =
    default;
VoiceOverlayStyle::~VoiceOverlayStyle() = default;

// static
VoiceOverlayStyle VoiceOverlayStyle::For(const views::View& view,
                                         content::WebContents* contents) {
  VoiceOverlayStyle style;
  const gfx::FontList interface_font;
  // The default font size the tab's pages get.
  const float page_font =
      contents ? contents->GetOrCreateWebPreferences().default_font_size : 16;
  style.sizes = ComputeOverlaySizes(interface_font.GetFontSize(), page_font);

  // Badges: bold digits, the widest one measured.
  const gfx::FontList full =
      Sized(interface_font, style.sizes.badge_font, gfx::Font::Weight::BOLD);
  float digit_width = 0;
  for (char16_t digit = u'0'; digit <= u'9'; ++digit) {
    digit_width = std::max(
        digit_width, gfx::GetStringWidthF(std::u16string(1, digit), full));
  }
  style.badge_style.text_height = full.GetHeight();
  style.badge_style.digit_width = digit_width;
  style.badge_style.padding_x = std::round(0.3f * style.sizes.badge_font);
  style.badge_style.padding_y = std::round(2 * style.sizes.scale);
  style.badge_style.gap = 2;
  style.badge_style.min_scale =
      style.sizes.badge_min_font / style.sizes.badge_font;
  const BadgeScales scales = BadgeScalesFor(style.badge_style);
  for (size_t i = 0; i < kBadgeScaleSteps; ++i) {
    const float scale = scales.scale[std::min(i, scales.count - 1)];
    style.badge_fonts[i] = Sized(
        interface_font, style.sizes.badge_font * scale, gfx::Font::Weight::BOLD);
  }

  // The caption.
  const gfx::FontList caption = CaptionFontAt(style.sizes.caption_font);
  style.caption_metrics.font = style.sizes.caption_font;
  style.caption_metrics.line_height = caption.GetHeight();
  style.caption_metrics.padding = std::round(0.4f * style.sizes.caption_font);
  style.caption_metrics.border = 3;
  style.caption_min_font = style.sizes.caption_min_font;

  // Colours.
  const ui::NativeTheme* theme = view.GetNativeTheme();
  const bool forced =
      theme &&
      theme->forced_colors() != ui::ColorProviderKey::ForcedColors::kNone;
  SystemColors system;
  if (const ui::ColorProvider* colors = view.GetColorProvider();
      forced && colors) {
    // In forced colours these are the OS's Window, WindowText, Highlight
    // and HighlightText (ui/color/win/native_color_mixers_win.cc).
    system.window = colors->GetColor(ui::kColorSysSurface);
    system.window_text = colors->GetColor(ui::kColorSysOnSurface);
    system.highlight = colors->GetColor(ui::kColorSysPrimary);
    system.highlight_text = colors->GetColor(ui::kColorSysOnPrimary);
  }
  style.palette = ChoosePalette(forced, system);
  return style;
}

// static
gfx::FontList VoiceOverlayStyle::CaptionFontAt(float size) {
  return Sized(gfx::FontList(), size, gfx::Font::Weight::SEMIBOLD);
}

}  // namespace voice
}  // namespace molt_ai
