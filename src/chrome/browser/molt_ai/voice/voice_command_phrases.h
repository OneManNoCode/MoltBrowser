// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The direct-command grammar's patterns, per language, and what each
// command is for (docs/VOICE_COMMANDS.md is generated from these tables by
// tools/voice/voice_commands_doc.py, and "what can I say?" reads them).
//
// A pattern is matched against the cleaned-up words of an utterance
// (voice_command.h), left to right, without backtracking, and must take
// all of them. Patterns are tried in table order; the first that matches
// wins, so a specific pattern comes before a general one ("go back 10
// seconds" before "go back", "go to the third one" before "go to
// <site>").
//
// Pattern syntax (words folded as spoken_text.h folds them):
//   word          that word
//   (a|b c|d)     one of the alternatives (each one or more words); the
//                 longest that matches is taken
//   [a|b c]       the same, or nothing
//   {slot}        a typed slot:
//     {seconds}   a length of time ("30 seconds", "a minute and a half")
//     {seconds?}  the same, or nothing (the player's default step)
//     {level}     a volume: 0-10 on a scale of ten, "N percent", "half",
//                 "max"
//     {pages?}    a scroll amount ("a page", "half a page", "a bit",
//                 "3 pages"), or nothing
//     {position}  a choice by position, number or relation ("the fourth
//                 one", "number 12", "the next one"), no description
//     {number}    {position}, or a bare number ("4", "four", "for")
//     {choice}    any choice reference ("the live one by Queen")
//     {pick}      {choice} said on its own right after a list: a bare
//                 number, or a short description (at most six words)
//     {tab}       a tab: number, position, relation or name
//     {site}      a site name or address ("YouTube", "example dot com")
//     {address}   a site address only (with a dot)
//     {query} {text} {name}   free text, kept as heard
//   {site}, {address} and choice slots take the words up to the first
//   place the next pattern word matches; {query}, {text} and {name} up to
//   the last. Either must be followed by a required word or end the
//   pattern.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_COMMAND_PHRASES_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_COMMAND_PHRASES_H_

#include <cstdint>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/voice/spoken_text.h"
#include "chrome/browser/molt_ai/voice/voice_command.h"

namespace molt_ai {
namespace voice {

// When a pattern applies.
enum class PatternContext : uint8_t {
  kAlways,
  kYesNo,      // ParseContext::expecting_yes_no
  kChoice,     // ParseContext::expecting_choice
  kTextField,  // ParseContext::text_field_focused
};

// What a pattern sets beyond its kind.
enum class PatternVariant : uint8_t {
  kNone,
  kNewTab,       // kOpenSite in a new tab
  kNextTab,      // kSwitchTab to the next tab
  kPreviousTab,  // kSwitchTab to the previous tab
  kPageStep,     // kScrollDown / kScrollUp by a page ("page down")
  // kChoose's verb.
  kVerbOpen,
  kVerbPlay,
  kVerbClick,
  kVerbSelect,
  kVerbRead,
  kVerbDescribe,
  // kSearch's scope.
  kScopeCurrentSite,
  kScopeDefaultEngine,
  kScopeNamedSite,
  // kVerbosity's level.
  kBrief,
  kNormal,
  kFull,
  kMore,
  kLess,
  // kAskPage's question (PageQuestionKind).
  kAskAbout,
  kAskMainPoints,
  kAskTopic,
  kAskQuestion,
  kAskMore,
};

struct CommandPattern {
  VoiceCommandKind kind = VoiceCommandKind::kUnknown;
  std::string_view pattern;
  PatternContext context = PatternContext::kAlways;
  PatternVariant variant = PatternVariant::kNone;
};

// What a command is for, for the reference and for "what can I say?".
struct CommandInfo {
  VoiceCommandKind kind = VoiceCommandKind::kUnknown;
  // The reference's section: "Going places", "Media", ...
  std::string_view section;
  // One line for the user: "Go back to the previous page."
  std::string_view summary;
  // Something to say that does it (the unit tests check it does).
  std::string_view example;
  // The context the example needs.
  PatternContext context = PatternContext::kAlways;
};

// Whether |pattern| follows the syntax above (the unit tests check every
// table entry; a malformed one never matches).
bool IsValidCommandPattern(std::string_view pattern);

// The tables of |language|, kept for the life of the process.
const std::vector<CommandPattern>& CommandPatterns(Language language);
const std::vector<CommandInfo>& CommandInfos(Language language);

// voice_command_phrases_<lang>.cc's tables.
std::vector<CommandPattern> MakeEnglishCommandPatterns();
std::vector<CommandInfo> MakeEnglishCommandInfos();

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_COMMAND_PHRASES_H_
