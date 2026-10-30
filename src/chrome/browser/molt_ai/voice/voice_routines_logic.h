// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Routines taught and run by voice (docs/design/TOUCHLESS_BROWSING.md
// §6.8): "remember this as my news" saves what the user just did; "run my
// news" (or "my news" alone) replays it, without the language model,
// through the voice loop's own steps.
//
// A routine is a list of RoutineSteps: the voice commands that were
// carried out, as the grammar parsed them (voice_command.h), with only the
// fields their kind uses. voice_routines.h turns them into an Agent-mode
// workflow (.molt, automation_script.h) and back.
//
// What "this" is (RoutineRecorder): the commands the user carried out
// since the task began. A task begins at the last command that opened a
// site or searched the web ("open YouTube", "search the web for ...",
// "search YouTube for ..."), or at voice mode's start; at most
// kMaxThisSteps steps. A command that waited for a "yes" counts once the
// "yes" carried it out. The steps the on-device planner carried out for a
// request the grammar had no command for are added as they run, in order
// with the user's (|planned|, the agent's step in |agent|: a navigation, a
// click, typing, a choice in a list or a scroll, as an agent draft keeps
// them); they make the routine a draft whose sites the user confirms
// first, and a run that opens a page with its first step begins a new
// task. A step of the planner's that a routine can't repeat (a site's own
// tool, ...) is kept as a gap: "this" can't be saved until a new task.
//
// Choices replay as they were made:
//   - by position ("the first one", "the last one"): the same position in
//     the new page's list, which is what such a routine means (the first
//     news story of the day);
//   - by name ("the live one by Queen"): the user's own words, matched on
//     the new page;
//   - by a number or "the next one": those name a place on that page at
//     that moment, so the step keeps what they chose: its position in the
//     list it was in, else the item's name (its label, cleaned and capped
//     as the agent's drafts keep a text anchor).
//
// Std-only (the grammar's types): unit-tested.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_ROUTINES_LOGIC_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_ROUTINES_LOGIC_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/voice/spoken_choice.h"
#include "chrome/browser/molt_ai/voice/voice_command.h"
#include "chrome/browser/molt_ai/voice/voice_hints.h"
#include "chrome/browser/molt_ai/voice/voice_planner.h"

namespace molt_ai {
namespace voice {

// A routine's steps, at most; what "remember this" keeps, at most.
inline constexpr size_t kMaxRoutineSteps = 50;
inline constexpr size_t kMaxThisSteps = 20;
// Routines read out by "what are my routines", at most.
inline constexpr size_t kMaxRoutinesRead = 10;
// A routine's name: at most this many words (and kMaxRoutineNameBytes).
inline constexpr size_t kMaxRoutineNameWords = 8;
// An item's name kept for a choice made by number, in code points.
inline constexpr size_t kMaxChoiceNameChars = 60;

// What voice mode's learning gives the loop for one utterance
// (VoiceEnvironment::learning). The glue fills it from the profile
// (voice_learning.h); it stays empty in MoltNet windows.
struct LearningEnvironment {
  LearningEnvironment();
  ~LearningEnvironment();
  LearningEnvironment(const LearningEnvironment&);
  LearningEnvironment& operator=(const LearningEnvironment&);
  LearningEnvironment(LearningEnvironment&&);
  LearningEnvironment& operator=(LearningEnvironment&&);

  // Routines can be kept, run and listed here (not in MoltNet windows).
  bool routines_available = false;
  // "Learn from what I say" is on: personal words, corrections and site
  // hints are learned and used.
  bool enabled = false;
  // The saved routines' names, as the user named them.
  std::vector<std::string> routines;
  // What "remember this as ..." would save now, as the browser says each
  // step ("open YouTube"), and whether some steps are the planner's.
  std::vector<std::string> this_steps;
  bool this_planned = false;
  // Why "this" can't be saved (a step of the planner's a routine can't
  // repeat), as the browser says it; "" when it can.
  std::string this_refusal;
  // Changes whenever |this_steps| does: a save holds the version it read
  // back, and saves only that.
  int this_version = 0;
  // The learned words and corrections (folded), for "forget the word X".
  std::vector<std::string> words;
  // The learned hint for the active tab's site (empty host: none).
  SiteHint hint;
};

// What a kLearning step asks the glue to do (VoiceStep::index).
enum class LearningOp : int {
  kSaveRoutine = 1,    // |text|: the name; |count|: this_version
  kRunRoutine = 2,     // |text|: the name
  kForgetRoutine = 3,  // |text|: the name
  kForgetWord = 4,     // |text|: the word
  kForgetEverything = 5,
};

struct RoutineStep {
  RoutineStep();
  ~RoutineStep();
  RoutineStep(const RoutineStep&);
  RoutineStep& operator=(const RoutineStep&);
  RoutineStep(RoutineStep&&);
  RoutineStep& operator=(RoutineStep&&);

  // The command, with only the fields its kind uses (RoutineCommand());
  // |heard| empty.
  VoiceCommand command;
  // The http(s) page's host it was said on; "" for another page.
  std::string host;
  // kOpenSite: the page it opened (the workflow's navigate target).
  std::string url;
  // kSearch, kType (and the planner's kType): the text wasn't kept (it
  // looked secret); it is asked for at each run under this name.
  // |command.text| (|agent.text|) is then empty.
  std::string variable;
  // A step the on-device planner chose (an agent run's).
  bool planned = false;
  // The planner's step, as the web agent carried it out (|planned|; its
  // action is kNone for a voice command). |command| is then unused, |host|
  // the page it acted on (a navigation's: the page it opened) and |url|
  // a navigation's or a search's address.
  PlannerStep agent;
};

// Whether a command of |kind| can be a routine's step: going places,
// viewing, media, reading the page's lists, choosing and typing; not
// conversation, tabs, the routines themselves or voice mode.
bool IsRoutineKind(VoiceCommandKind kind);

// Whether |step| begins a new task: it opens a site or searches the web.
bool IsTaskStart(const RoutineStep& step);

// Whether |step| is the planner's step as the web agent carried it out
// (not a voice command), and whether it is a gap ("this" can't be saved).
bool IsAgentStep(const RoutineStep& step);
bool IsUnkeptStep(const RoutineStep& step);

// The step for |command| as the user said it and it was carried out, on
// |host|. A choice (kChoose) is kept as RoutineChoice() decides: |position|
// is the chosen entry's place in the list it was in (0: none), |label| the
// chosen item's name (page text), |answered_offer| whether it answered
// "which one?". nullopt for a command that isn't a routine step, or a
// choice that can't be replayed.
std::optional<RoutineStep> RoutineStepFor(const VoiceCommand& command,
                                          std::string_view host,
                                          int position,
                                          std::string_view label,
                                          bool answered_offer);

// |command| with only what a routine step of its kind keeps.
VoiceCommand RoutineCommand(const VoiceCommand& command);

// How a choice is kept (see the file comment); nullopt when it can't be.
std::optional<ChoiceRef> RoutineChoice(const ChoiceRef& said,
                                       int position,
                                       std::string_view label,
                                       bool answered_offer);

// The command to run for |step|, with |value| for its variable.
VoiceCommand CommandForStep(const RoutineStep& step, std::string_view value);

// The step as the browser says it: "open YouTube", "search for news",
// "play the first one", "play the live one by queen", "go back". Words the
// user said, cleaned. The planner's: "open trains.example", "search the web
// for trains to lyon", "click Search", "type Lyon in To", "choose First
// class in Class", "scroll down" (page text and the model's, cleaned).
std::string DescribeRoutineStep(const RoutineStep& step);

// Descriptions joined for speech: "a", "a and b", "a, b and c".
std::string JoinSpokenList(const std::vector<std::string>& items);

// A routine's name as said ("my news"), cleaned: 1 to kMaxRoutineNameWords
// words with a letter, at most kMaxRoutineNameBytes; "" when it isn't
// usable (nor when the whole of it answers a yes-or-no question).
std::string RoutineDisplayName(std::string_view said);

// The key names are compared by: folded, without a leading "my", "the",
// "a" or "an" ("My News", "news" and "the news" are one name).
std::string RoutineNameKey(std::string_view name);

// The name in |names| that |said| names, or null.
const std::string* FindRoutineName(const std::vector<std::string>& names,
                                   std::string_view said);

// "This": the steps the user carried out since the task began.
class RoutineRecorder {
 public:
  RoutineRecorder();
  ~RoutineRecorder();
  RoutineRecorder(const RoutineRecorder&) = delete;
  RoutineRecorder& operator=(const RoutineRecorder&) = delete;

  // A step carried out. A task start begins anew; past kMaxThisSteps the
  // oldest goes.
  void Add(RoutineStep step);
  // The step added last didn't work after all.
  void DropLast();
  void Clear();

  const std::vector<RoutineStep>& steps() const { return steps_; }
  bool planned() const;
  // Why "this" can't be saved: the first gap's (IsUnkeptStep()); "" when
  // it can.
  std::string refusal() const;
  // Changes with every Add() and Clear().
  int version() const { return version_; }

 private:
  std::vector<RoutineStep> steps_;
  int version_ = 0;
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_ROUTINES_LOGIC_H_
