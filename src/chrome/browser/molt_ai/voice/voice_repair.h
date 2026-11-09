// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// A misheard command, repaired. When the grammar (voice_command.h)
// understood nothing in an utterance, the recognizer has often got one
// command word wrong ("close this tip", "Joe my downloads", "read the
// tile", "skip the head") or left a scrap of the wake phrase in front of a
// command ("halt. Play the live one by Queen"). RepairMisheardCommand()
// tries, in order:
//
//   1. Without a scrap of the wake phrase: one or two words ending at a
//      full stop, comma, question or exclamation mark within the first
//      three words, that sound like the wake phrase's name; or, after a
//      wake phrase the grammar took, a first word that is only the stop
//      the name ends on ("him all to scroll to the top": the "t" of
//      "Molt" heard as "to").
//   2. One word, or two run together, swapped for a word of the grammar's
//      own patterns that sounds the same or nearly (phonetic_key.h:
//      "tip"/"tab", "joe"/"show", "on mute"/"unmute"), the rest as heard.
//
// A repair counts only when the grammar then understands a command of a
// kind that may run on it, and only when every way of repairing the
// utterance gives the same command: two different commands, and it stays
// not understood. After a swap, a kind safe to run from a guessed word
// (RepairableKind(): moving around, reading, media, scrolling, zoom, tabs,
// typing; never an answer to a question, a choice, a form's submission, a
// setting, a routine, voice mode itself or closing a window). With only a
// scrap dropped, the command's words are as said ("Kalt, click empty the
// cart."), and more kinds may run (RepairableAsSaidKind(): presses and
// submissions too, which ask first as they would in full); a command as
// said that isn't one of them is never swapped into another. Never while voice mode waits for a yes or no, and
// only in English (the phonetic key is English's).
//
// The planner applies it last, to what nothing else took: after the
// browser's prompt, a stray yes or no, a routine's name and a site's name
// alone, before the on-device planner is asked (voice_controller_logic.h),
// and never to what may have answered a confirmation. Std-only, with the
// grammar: unit-tested.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_REPAIR_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_REPAIR_H_

#include <optional>
#include <string>
#include <string_view>

#include "chrome/browser/molt_ai/voice/voice_command.h"

namespace molt_ai {
namespace voice {

// An utterance longer than this (in words) isn't repaired: the work is
// quadratic in the number of words, and a long utterance the grammar
// didn't understand is a request for the planner.
inline constexpr size_t kMaxRepairWords = 12;

struct RepairedCommand {
  RepairedCommand();
  ~RepairedCommand();
  RepairedCommand(const RepairedCommand&);
  RepairedCommand& operator=(const RepairedCommand&);
  RepairedCommand(RepairedCommand&&);
  RepairedCommand& operator=(RepairedCommand&&);

  // What the grammar understood in |text|; its |heard| is the utterance as
  // the recognizer wrote it.
  VoiceCommand command;
  // The utterance as repaired: "close this tab".
  std::string text;
  // Only a scrap of the wake phrase was dropped: |text| is the rest as
  // heard, and |command| one RepairableAsSaidKind() allows.
  bool as_said = false;
};

// Whether a command of |kind| may run on an utterance with a word swapped.
bool RepairableKind(VoiceCommandKind kind);

// Whether a command of |kind| may run on what was said after a scrap of the
// wake phrase: RepairableKind()'s, and presses, submissions, editing, a
// correction, routines run or listed, bookmarks made or opened, history,
// settings' page, windows opened or switched and the AI.
bool RepairableAsSaidKind(VoiceCommandKind kind);

// The command a misheard |utterance| meant, as described above, or none.
// |context| is the one the grammar was given.
std::optional<RepairedCommand> RepairMisheardCommand(
    std::string_view utterance,
    const ParseContext& context);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_REPAIR_H_
