// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The player controls voice mode reaches on a page's own media element
// (docs/design/TOUCHLESS_BROWSING.md §2, "Media"), where neither the media
// session nor an accessibility action does: the playback speed ("speed
// 1.5", "faster" while nothing is read), the volume set to a level ("volume
// 50 percent", "maximum volume") and the element's captions ("turn on
// captions" on a player with no captions button).
//
//   - The browser runs MediaScript() in the media's frame, in an isolated
//     world (the page can't reach or change the script), on the media
//     element playing, else the largest one on screen, else the first. The
//     script changes only that element and returns what it then is
//     (MediaState); the page sees what a user's change would show
//     ("ratechange", "volumechange").
//   - "Faster" and "slower" step through YouTube's speeds
//     (kPlaybackRateSteps) from the element's own; a speed said is clamped
//     to what players take.
//   - What the browser says (MediaOutcomeSpeech()) is built from the
//     numbers only: nothing of the page is said.
//
// Pure and std-only: unit-tested in the voice harness and
// molt_ai_unittests; the browser's side is voice_browser_actions.h.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_MEDIA_CONTROLS_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_MEDIA_CONTROLS_H_

#include <string>
#include <string_view>

#include "chrome/browser/molt_ai/voice/spoken_text.h"

namespace molt_ai {
namespace voice {

// The speeds "faster" and "slower" step through, in hundredths.
inline constexpr int kPlaybackRateSteps[] = {25,  50,  75,  100,
                                             125, 150, 175, 200};
// The speeds a player is set to at most and at least, in hundredths.
inline constexpr int kMinPlaybackRate = 25;
inline constexpr int kMaxPlaybackRate = 400;
inline constexpr int kNormalPlaybackRate = 100;

// |rate| within kMinPlaybackRate..kMaxPlaybackRate.
int ClampPlaybackRate(int rate);

// The step after |current| (hundredths) towards faster or slower; |current|
// itself (clamped) when there is none.
int NextPlaybackRate(int current, bool faster);

// A speed as the browser says it in |language|: "1.5", "2", "0.75"; in
// Spanish with a decimal comma, "1,5".
std::string SpokenRate(int rate, Language language);

// What the media script does.
enum class MediaOp : uint8_t {
  kQuery,      // nothing: what the element is
  kSetRate,    // |value|: hundredths
  kSetVolume,  // |value|: 0..100 (above 0 the element is unmuted too)
  kCaptions,   // |value|: 1 shows its first captions or subtitles, 0 hides
};

// The script the browser runs in the media's frame for |op| with |value|
// (clamped): a JavaScript expression whose value is a plain object (see
// MediaState).
std::string MediaScript(MediaOp op, int value);

// What the media element is after the script (the browser reads it from
// the script's result, which it treats as the page's: every number
// clamped).
struct MediaState {
  bool found = false;
  int rate = kNormalPlaybackRate;  // hundredths
  int volume = 100;                // 0..100
  bool muted = false;
  // Its captions and subtitles tracks, and whether one shows.
  int caption_tracks = 0;
  bool captions_showing = false;
};

// What the browser says after |op| (|asked|: the value asked for) gave
// |state|, in |language|.
std::string MediaOutcomeSpeech(MediaOp op,
                               int asked,
                               const MediaState& state,
                               bool tab_muted,
                               Language language);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_MEDIA_CONTROLS_H_
