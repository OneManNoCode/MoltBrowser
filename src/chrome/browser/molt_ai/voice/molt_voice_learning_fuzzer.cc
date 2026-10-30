// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// What voice mode learned, as files anything on the machine may have
// written (docs/design/TOUCHLESS_BROWSING.md §6.8): the learning file
// (voice_learning_store.h) and a routine's .molt workflow
// (voice_routines.h), which may have been imported or edited; and what
// the learned corrections do to utterances.
//
// The first byte picks the input; the rest is it.
//   0: the learning file. Checks: what is read back is within the caps,
//      every phrase is learnable and no correction's side answers a
//      question, hints are clean; it saves and reads back the same.
//   1: a .molt file. Checks: a routine voice mode would run has at most
//      kMaxRoutineSteps steps of routine kinds or the planner's (never a
//      gap), each describable; built again from them it reads back the
//      same steps (after one opening the site the first was said on, when
//      the first opens none); a routine with the planner's steps needs its
//      sites confirmed unless they are exactly the sites its steps use.
//   2: two utterances, split at the first newline: a correction lined up
//      with the first and applied to the second. Checks: the results are
//      valid UTF-8 within the grammar's cap.
//   3: a step the planner carried out, as the web agent reports it, its
//      fields split at newlines (the action's number, the address, the
//      query, the host, the element's kind and name, the input's type, the
//      text, the option, the direction, whether it is a credential field,
//      the request, the page's host). Checks: it is kept as the planner's
//      step or as a gap that says why; kept, its text is within the caps
//      and a routine of it reads back as the same step, its typed text
//      never in the file when withheld.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/check.h"
#include "base/check_op.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/automation/automation_voice_steps.h"
#include "chrome/browser/molt_ai/voice/spoken_text.h"
#include "chrome/browser/molt_ai/voice/voice_command.h"
#include "chrome/browser/molt_ai/voice/voice_learning_store.h"
#include "chrome/browser/molt_ai/voice/voice_planner.h"
#include "chrome/browser/molt_ai/voice/voice_routines.h"
#include "chrome/browser/molt_ai/voice/voice_vocabulary.h"

namespace {

using molt_ai::voice::LearnedData;
using molt_ai::voice::VocabularyEntry;

constexpr int64_t kNow = 1'800'000'000;

bool AnswersAQuestion(std::string_view phrase) {
  molt_ai::voice::ParseContext context;
  context.expecting_yes_no = true;
  const auto kind = molt_ai::voice::ParseVoiceCommand(phrase, context).kind;
  return kind == molt_ai::voice::VoiceCommandKind::kYes ||
         kind == molt_ai::voice::VoiceCommandKind::kNo;
}

void LearningFile(std::string_view input) {
  const LearnedData data = molt_ai::voice::ParseLearning(input, kNow);
  CHECK_LE(data.vocabulary.entries().size(),
           molt_ai::voice::kMaxVocabularyEntries);
  CHECK_LE(data.hints.hints().size(), molt_ai::voice::kMaxHintedSites);
  for (const VocabularyEntry& entry : data.vocabulary.entries()) {
    CHECK(molt_ai::voice::LearnablePhrase(entry.phrase));
    CHECK(!AnswersAQuestion(entry.phrase));
    CHECK_GE(entry.uses, 1);
    if (entry.correction()) {
      CHECK(molt_ai::voice::LearnablePhrase(entry.heard));
      CHECK(!AnswersAQuestion(entry.heard));
    }
  }
  for (const molt_ai::voice::SiteHint& hint : data.hints.hints()) {
    CHECK(!hint.host.empty());
    CHECK(!hint.empty());
    CHECK(molt_ai::voice::IsValidUtf8(hint.search_field));
    CHECK(molt_ai::voice::IsValidUtf8(hint.result_list));
    CHECK(molt_ai::voice::IsValidUtf8(hint.player));
  }
  const std::string saved = molt_ai::voice::SerializeLearning(data);
  const LearnedData again = molt_ai::voice::ParseLearning(saved, kNow);
  CHECK_EQ(molt_ai::voice::SerializeLearning(again), saved);
}

void RoutineFile(std::string_view input) {
  std::optional<molt_ai::automation::Script> script =
      molt_ai::automation::Script::FromJSONString(std::string(input));
  if (!script) {
    return;
  }
  const molt_ai::voice::ReadRoutineResult read =
      molt_ai::voice::ReadRoutine(*script);
  if (!read.ok) {
    CHECK(read.steps.empty());
    CHECK(!read.why.empty());
    return;
  }
  CHECK(!read.steps.empty());
  CHECK_LE(read.steps.size(), molt_ai::voice::kMaxRoutineSteps);
  bool planned = false;
  for (const molt_ai::voice::RoutineStep& step : read.steps) {
    CHECK(molt_ai::voice::IsAgentStep(step) ||
          molt_ai::voice::IsRoutineKind(step.command.kind));
    CHECK(!molt_ai::voice::IsUnkeptStep(step));
    CHECK(!molt_ai::voice::DescribeRoutineStep(step).empty());
    planned = planned || step.planned;
  }
  // Confirmed only for exactly the sites its steps use.
  if (planned && !read.needs_sites_confirmed) {
    CHECK(!script->security.sites_unconfirmed);
    CHECK(molt_ai::voice::RoutineSitesConfirmed(*script));
  }
  std::optional<molt_ai::automation::Script> unconfirmed =
      molt_ai::automation::Script::FromJSONString(script->ToJSONString());
  if (unconfirmed && molt_ai::voice::UnconfirmChangedRoutine(*unconfirmed)) {
    CHECK(read.needs_sites_confirmed);
    CHECK(unconfirmed->security.sites_unconfirmed);
    CHECK(unconfirmed->security.domain_whitelist.empty());
  }
  // Built again, it reads back as the same steps.
  molt_ai::voice::BuiltRoutine built =
      molt_ai::voice::BuildRoutine("voice_fuzz", "fuzz", read.steps, kNow);
  if (!built.script) {
    return;
  }
  const std::string json = built.script->ToJSONString();
  std::optional<molt_ai::automation::Script> loaded =
      molt_ai::automation::Script::FromJSONString(json);
  CHECK(loaded);
  const molt_ai::voice::ReadRoutineResult again =
      molt_ai::voice::ReadRoutine(*loaded);
  CHECK(again.ok);
  // The same steps; when the first doesn't open a site, after one that
  // opens the site it was said on (BuildRoutine()).
  CHECK_GE(again.steps.size(), read.steps.size());
  const size_t opened = again.steps.size() - read.steps.size();
  CHECK_LE(opened, 1u);
  if (opened == 1) {
    CHECK(!molt_ai::voice::IsTaskStart(read.steps.front()));
    CHECK(again.steps.front().command.kind ==
          molt_ai::voice::VoiceCommandKind::kOpenSite);
    CHECK_EQ(again.steps.front().command.site, read.steps.front().host);
  }
  for (size_t i = 0; i < read.steps.size(); ++i) {
    const molt_ai::voice::RoutineStep& a = again.steps[i + opened];
    const molt_ai::voice::RoutineStep& b = read.steps[i];
    CHECK_EQ(molt_ai::voice::IsAgentStep(a), molt_ai::voice::IsAgentStep(b));
    if (molt_ai::voice::IsAgentStep(b)) {
      CHECK(a.agent.action == b.agent.action);
      CHECK_EQ(a.agent.label, b.agent.label);
      CHECK_EQ(a.agent.option, b.agent.option);
      CHECK_EQ(a.url, b.url);
      CHECK_EQ(a.host, b.host);
    } else {
      CHECK(a.command.kind == b.command.kind);
    }
  }
}

void PlannerStep(std::string_view input) {
  using Action = molt_ai::voice::PlannerStep::Action;
  std::vector<std::string> fields;
  size_t start = 0;
  while (fields.size() < 12) {
    const size_t newline = input.find('\n', start);
    fields.emplace_back(input.substr(start, newline == std::string_view::npos
                                                ? std::string_view::npos
                                                : newline - start));
    if (newline == std::string_view::npos) {
      break;
    }
    start = newline + 1;
  }
  fields.resize(12);
  molt_ai::voice::PlannerStep carried;
  carried.action = static_cast<Action>(
      fields[0].empty() ? 0
                        : static_cast<uint8_t>(fields[0][0]) %
                              (static_cast<int>(Action::kUnkept) + 1));
  carried.url = fields[1];
  carried.query = fields[2];
  carried.host = fields[3];
  carried.kind = fields[4];
  carried.label = fields[5];
  carried.input_type = fields[6];
  carried.text = fields[7];
  carried.option = fields[8];
  carried.direction = fields[9];
  carried.credential_field = !fields[10].empty();
  carried.why = fields[10];
  const molt_ai::voice::RoutineStep step =
      molt_ai::voice::RoutineStepForPlanner(carried, fields[11], fields[3]);
  CHECK(step.planned);
  CHECK(molt_ai::voice::IsAgentStep(step));
  CHECK(!molt_ai::voice::DescribeRoutineStep(step).empty());
  if (molt_ai::voice::IsUnkeptStep(step)) {
    CHECK(!step.agent.why.empty());
    CHECK(!molt_ai::voice::BuildRoutine("voice_fuzz", "fuzz", {step}, kNow)
               .script);
    return;
  }
  CHECK(molt_ai::voice::IsValidUtf8(step.agent.label));
  CHECK(molt_ai::voice::IsValidUtf8(step.agent.text));
  CHECK_LE(step.agent.label.size(), 4 * molt_ai::voice::kMaxPlannerLabelChars);
  CHECK_LE(step.agent.text.size(), 4 * molt_ai::voice::kMaxPlannerTextChars);
  CHECK_LE(step.url.size(), molt_ai::voice::kMaxPlannerUrlBytes);
  CHECK(step.variable.empty() || step.agent.text.empty());
  // A routine of it reads back as the same step, after the page it was
  // done on when it acted on one.
  molt_ai::voice::BuiltRoutine built =
      molt_ai::voice::BuildRoutine("voice_fuzz", "fuzz", {step}, kNow);
  CHECK(built.script);
  std::optional<molt_ai::automation::Script> loaded =
      molt_ai::automation::Script::FromJSONString(built.script->ToJSONString());
  CHECK(loaded);
  // Withheld text isn't in the file: a variable stands in for it.
  if (!step.variable.empty()) {
    const molt_ai::automation::Step& stored = loaded->steps.back();
    CHECK_EQ(stored.value, "{{" + step.variable + "}}");
    CHECK(!stored.extra.GetDict()
               .FindDict(molt_ai::automation::kVoiceStepKey)
               ->Find("text"));
  }
  const molt_ai::voice::ReadRoutineResult read =
      molt_ai::voice::ReadRoutine(*loaded);
  CHECK(read.ok);
  CHECK(read.needs_sites_confirmed);
  const molt_ai::voice::RoutineStep& back = read.steps.back();
  CHECK(back.agent.action == step.agent.action);
  CHECK_EQ(back.agent.kind, step.agent.kind);
  CHECK_EQ(back.agent.label, step.agent.label);
  CHECK_EQ(back.agent.text, step.agent.text);
  CHECK_EQ(back.agent.option, step.agent.option);
  CHECK_EQ(back.agent.query, step.agent.query);
  CHECK_EQ(back.host, step.host);
}

void Utterances(std::string_view input) {
  const size_t newline = input.find('\n');
  const std::string_view first = input.substr(0, newline);
  const std::string_view second =
      newline == std::string_view::npos ? std::string_view()
                                        : input.substr(newline + 1);
  molt_ai::voice::PersonalVocabulary vocabulary;
  if (std::optional<molt_ai::voice::AlignedCorrection> aligned =
          molt_ai::voice::AlignCorrection(first, second)) {
    CHECK(molt_ai::voice::IsValidUtf8(aligned->corrected));
    CHECK_LE(aligned->corrected.size(), molt_ai::voice::kMaxUtteranceBytes);
    if (!aligned->heard.empty() &&
        vocabulary.NoteCorrection(aligned->heard, aligned->meant, kNow)) {
      CHECK_EQ(vocabulary.entries().size(), 1u);
    }
  }
  vocabulary.NoteCorrection(first, second, kNow);
  vocabulary.NoteName(first, kNow);
  if (std::optional<std::string> out = vocabulary.ApplyCorrections(second)) {
    CHECK(molt_ai::voice::IsValidUtf8(*out));
    CHECK_LE(out->size(), molt_ai::voice::kMaxUtteranceBytes);
  }
  for (const std::string& phrase :
       vocabulary.HotwordPhrases(molt_ai::voice::kMaxPersonalHotwords)) {
    CHECK(molt_ai::voice::LearnablePhrase(phrase));
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0) {
    return 0;
  }
  // SAFETY: libFuzzer passes |size| valid bytes at |data|.
  const std::string_view whole(reinterpret_cast<const char*>(data), size);
  const std::string_view input = whole.substr(1);
  switch (static_cast<uint8_t>(whole[0]) % 4) {
    case 0:
      LearningFile(input);
      break;
    case 1:
      RoutineFile(input);
      break;
    case 2:
      Utterances(input);
      break;
    default:
      PlannerStep(input);
      break;
  }
  return 0;
}
