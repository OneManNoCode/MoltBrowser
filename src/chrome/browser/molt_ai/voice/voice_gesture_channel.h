// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// How the voice gesture's confirmation word reaches the user
// (docs/design/TOUCHLESS_BROWSING.md §7.3, "The word's channel"): said
// aloud, shown in the caption bar, both, or neither, so that nothing that
// could answer the prompt in the user's place learns it first.
//
// What could learn it:
//   - a tab, a window or an extension capturing a microphone: it hears the
//     browser say the word (and the user say it), and can play it back or
//     pass it on;
//   - the speech helper, if hostile audio took it over: it hears the
//     platform's voices in full (they play outside the echo canceller's
//     reference), and voice mode's own voice when the microphone has no
//     echo canceller;
//   - a page capturing the screen or a window (with the system's sound,
//     where the platform lets it): it sees the caption bar, and may hear
//     the speakers directly.
//
// The decision (DecideWordChannel()):
//   - The screen or a window is captured: never shown. With the setting
//     ("say the word aloud even when another program could hear it") it is
//     said; without it there is no word for the prompt: the gesture is off
//     for it, and "no" still refuses.
//   - Otherwise it is always shown, large, in the caption bar (not to
//     assistive technology: a screen reader would say it with a voice the
//     speech helper hears in full). It is also said when nothing else can
//     hear it: voice mode's own voice speaks, the microphone has an echo
//     canceller (whose reference holds that voice, so the speech helper
//     hears none of it; measured in §7.3) and nothing captures a
//     microphone. It is then said only through that voice
//     (VoiceSpeech::SpeakPrivately()): never by the platform's voices as a
//     fallback. With the setting it is said whatever the voice and the
//     captures.
//
// A word that went out through a channel that is no longer private
// (WordExposed(): a capture began after it was said or shown) is retired:
// the prompt gets a new word, through the channel decided then.
//
// Std-only, unit-tested; the browser's side is voice_browser_prompts.h.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_GESTURE_CHANNEL_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_GESTURE_CHANNEL_H_

#include <string>
#include <string_view>

namespace molt_ai {
namespace voice {

// A prompt gets at most this many words: its first, and the ones that
// replaced a word retired (a capture began, or the word was heard but not
// cleanly while something captured). Then the gesture is off for it.
inline constexpr int kMaxWordsPerPrompt = 4;

// The longest word the caption bar shows (the list's are much shorter).
inline constexpr size_t kMaxShownWordChars = 16;

// What decides the channel, looked at whenever a word is about to go out
// and whenever one of them changes.
struct WordChannelInputs {
  // The microphone has an echo canceller (Chrome-wide, or the platform's).
  bool echo_cancelled = false;
  // Voice mode's own voice (the bundled one, played by the browser) can
  // speak now.
  bool bundled_voice = false;
  // A tab, a window or an extension (any profile's, MoltNet's included)
  // captures a microphone.
  bool audio_captured = false;
  // A page or an extension captures the screen or a window.
  bool screen_captured = false;
  // The setting: "Say the confirmation word aloud even when another
  // program could hear it" (voice_session_settings.h).
  bool say_aloud_anyway = false;

  friend bool operator==(const WordChannelInputs&,
                         const WordChannelInputs&) = default;
};

struct WordChannel {
  // False: the prompt gets no word (the screen is shared).
  bool available = false;
  // The browser says the word...
  bool said = false;
  // ... only through its own voice, whose audio the echo canceller
  // removes: where that voice can't say it, the platform's voice says the
  // prompt without it.
  bool privately = false;
  // The caption bar shows it.
  bool shown = false;

  friend bool operator==(const WordChannel&, const WordChannel&) = default;
};

WordChannel DecideWordChannel(const WordChannelInputs& inputs);

// Whether a word that went out through |delivered| may have reached
// something that could answer with it, now that the inputs are |now|: it
// was shown and the screen is captured now, or it was said (or heard from
// the user, |delivered|.said) and a microphone or the screen is captured
// now, without the setting.
bool WordExposed(const WordChannel& delivered, const WordChannelInputs& now);

// What the browser says in place of the word where it can't say it ("To
// allow it, say the word on your screen.").
inline constexpr char kWordOnScreen[] = "the word on your screen";

// |word| as the caption bar shows it: lower-case ASCII letters only, at
// most kMaxShownWordChars, else empty.
std::string ShownWordText(std::string_view word);

// Short names for the decision, for logs and tests ("said privately,
// shown").
std::string WordChannelName(const WordChannel& channel);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_GESTURE_CHANNEL_H_
