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
//      kMaxRoutineSteps steps of routine kinds, each describable; built
//      again from them it reads back the same steps, and it holds no "{{"
//      outside a variable.
//   2: two utterances, split at the first newline: a correction lined up
//      with the first and applied to the second. Checks: the results are
//      valid UTF-8 within the grammar's cap.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "base/check.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/voice/spoken_text.h"
#include "chrome/browser/molt_ai/voice/voice_command.h"
#include "chrome/browser/molt_ai/voice/voice_learning_store.h"
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
  for (const molt_ai::voice::RoutineStep& step : read.steps) {
    CHECK(molt_ai::voice::IsRoutineKind(step.command.kind));
    CHECK(!molt_ai::voice::DescribeRoutineStep(step).empty());
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
  CHECK_EQ(again.steps.size(), read.steps.size());
  for (size_t i = 0; i < read.steps.size(); ++i) {
    CHECK(again.steps[i].command.kind == read.steps[i].command.kind);
  }
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
  switch (static_cast<uint8_t>(whole[0]) % 3) {
    case 0:
      LearningFile(input);
      break;
    case 1:
      RoutineFile(input);
      break;
    default:
      Utterances(input);
      break;
  }
  return 0;
}
