// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet letterboxing, browser side: the geometry.
//
// In MoltNet windows a tab's web contents don't fill the content area. They
// get its size rounded down to kLetterboxStep{Width,Height} and capped at
// kLetterboxMax{Width,Height}, centred, with margins around them. A page's
// innerWidth/innerHeight (at 100% zoom) is then one of a few dozen sizes, the
// same for everybody whose window rounds the same way, and exactly what
// screen.width/height report in MoltNet: Blink's UniformScreenRect() rounds
// the page's size with the same steps (LetterboxBucket in
// third_party/blink/renderer/core/molt_fingerprinting/farbling.h), and
// rounding an already rounded size changes nothing.
//
// The constants are duplicated because browser code can't include Blink's
// renderer headers; molt_letterbox_unittest.cc checks that they, and the
// rounding, match Blink's.
//
// Std-only and constexpr, so it is unit-tested standalone and with
// static_asserts. Where it meets the window: molt_letterbox_view.h (the
// margins) and chrome/browser/molt_ai/tor/molt_net_window.cc (the size new
// MoltNet windows open at, WindowLetterboxSize()).

#ifndef CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_LETTERBOX_H_
#define CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_LETTERBOX_H_

namespace molt_ai::fingerprinting {

// Must equal blink::molt_fingerprinting::kLetterbox* (farbling.h).
inline constexpr int kLetterboxStepWidth = 200;
inline constexpr int kLetterboxStepHeight = 100;
inline constexpr int kLetterboxMaxWidth = 1400;
inline constexpr int kLetterboxMaxHeight = 900;

struct LetterboxMargins {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;

  friend constexpr bool operator==(const LetterboxMargins&,
                                   const LetterboxMargins&) = default;
};

// How much of an `available`-DIP-long area the web contents get: rounded
// down to a multiple of `step`, at most `max` (a multiple of `step`). An area
// shorter than one step keeps its length (there is nothing to round down to;
// Blink reports one step there, see docs/FINGERPRINTING.md); a negative one
// counts as empty.
constexpr int LetterboxLength(int available, int step, int max) {
  if (available <= 0) {
    return 0;
  }
  if (available < step) {
    return available;
  }
  const int rounded = available - available % step;
  return rounded < max ? rounded : max;
}

// The margins that letterbox web contents in a `width` x `height` DIP area:
// what LetterboxLength() leaves in each dimension, split evenly (the odd DIP
// goes to the right / bottom).
constexpr LetterboxMargins LetterboxMarginsFor(int width, int height) {
  const int spare_width =
      width - LetterboxLength(width, kLetterboxStepWidth, kLetterboxMaxWidth);
  const int spare_height =
      height -
      LetterboxLength(height, kLetterboxStepHeight, kLetterboxMaxHeight);
  LetterboxMargins margins;
  if (spare_width > 0) {
    margins.left = spare_width / 2;
    margins.right = spare_width - margins.left;
  }
  if (spare_height > 0) {
    margins.top = spare_height / 2;
    margins.bottom = spare_height - margins.top;
  }
  return margins;
}

struct LetterboxSize {
  int width = 0;
  int height = 0;

  constexpr bool IsEmpty() const { return width <= 0 || height <= 0; }
  friend constexpr bool operator==(const LetterboxSize&,
                                   const LetterboxSize&) = default;
};

// The page size a new MoltNet window opens with (molt_net_window.cc), so it
// starts without margins: the largest letterbox size -- whole steps, at
// most kLetterboxMax{Width,Height} (1400 x 900) -- that fits on the screen
// together with the browser UI around the page. `window_*` is the window's
// current size and `page_*` its page area's, so the difference is that UI
// (frame, tab strip, toolbar, side panel, ...); `work_area_*` is the size of
// the screen's work area. All in DIPs. Empty when there is no room for one
// step, or the sizes make no sense: the window then keeps its size and the
// letterbox margins do the rest.
constexpr LetterboxSize WindowLetterboxSize(int window_width,
                                            int window_height,
                                            int page_width,
                                            int page_height,
                                            int work_area_width,
                                            int work_area_height) {
  if (page_width <= 0 || page_height <= 0 || window_width < page_width ||
      window_height < page_height || work_area_width <= 0 ||
      work_area_height <= 0) {
    return {};
  }
  // Both differences are in [0, window size], so nothing overflows.
  const int room_width = work_area_width - (window_width - page_width);
  const int room_height = work_area_height - (window_height - page_height);
  if (room_width < kLetterboxStepWidth || room_height < kLetterboxStepHeight) {
    return {};
  }
  return {
      LetterboxLength(room_width, kLetterboxStepWidth, kLetterboxMaxWidth),
      LetterboxLength(room_height, kLetterboxStepHeight, kLetterboxMaxHeight)};
}

}  // namespace molt_ai::fingerprinting

#endif  // CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_LETTERBOX_H_
