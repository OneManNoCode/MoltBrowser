// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/translate/page_translate_plan.h"

#include <algorithm>
#include <utility>

#include "chrome/browser/molt_ai/web_ai/web_ai_policy.h"

namespace molt_ai::page_translate {

namespace {

using web_ai::TranslationPiece;
using web_ai::TranslatorLanguage;

bool IsUnknown(std::string_view language) {
  return language.empty() || language == "und";
}

}  // namespace

// ---------------------------------------------------------------------------
// Languages
// ---------------------------------------------------------------------------

std::string PageTranslateCode(const TranslatorLanguage& language) {
  // Chinese by region, as Translate's language detection, prefs and UI
  // name it.
  const std::string_view code = language.code;
  if (code == "zh") {
    return "zh-CN";
  }
  if (code == "zh-Hant") {
    return "zh-TW";
  }
  return std::string(code);
}

std::vector<std::string> PageTranslateLanguageCodes() {
  std::vector<std::string> codes;
  for (const TranslatorLanguage& language : web_ai::kTranslatorLanguages) {
    codes.push_back(PageTranslateCode(language));
  }
  std::sort(codes.begin(), codes.end());
  return codes;
}

PagePair ResolvePagePair(std::string_view source, std::string_view target) {
  PagePair result;
  result.pair.target = web_ai::FindTranslatorLanguage(target);
  if (IsUnknown(source)) {
    result.problem = PairProblem::kUnknownSource;
    return result;
  }
  result.pair.source = web_ai::FindTranslatorLanguage(source);
  if (!result.pair.source) {
    result.problem = PairProblem::kUnsupportedSource;
  } else if (!result.pair.target) {
    result.problem = PairProblem::kUnsupportedTarget;
  } else if (result.pair.source == result.pair.target) {
    result.problem = PairProblem::kIdentical;
  }
  return result;
}

// ---------------------------------------------------------------------------
// The page's text
// ---------------------------------------------------------------------------

PageText::PageText() = default;
PageText::~PageText() = default;
PageText::PageText(PageText&&) = default;
PageText& PageText::operator=(PageText&&) = default;

Batch::Batch() = default;
Batch::~Batch() = default;
Batch::Batch(const Batch&) = default;
Batch& Batch::operator=(const Batch&) = default;
Batch::Batch(Batch&&) = default;
Batch& Batch::operator=(Batch&&) = default;

// ---------------------------------------------------------------------------
// The plan
// ---------------------------------------------------------------------------

struct TranslationPlan::SlotState {
  std::string original;
  std::vector<TranslationPiece> pieces;
  // One per piece: the translation of a piece sent to the model.
  std::vector<std::optional<std::string>> results;
  // Pieces sent to the model and not settled yet.
  size_t open = 0;
};

std::optional<std::string> CheckTranslation(std::string_view source,
                                            std::string_view translation) {
  std::optional<std::string> text =
      web_ai::CleanTranslation(source, translation);
  if (!text) {
    return std::nullopt;
  }
  // Both sizes are capped (kMaxSlotBytes, the reply budget), so this can't
  // overflow.
  if (text->size() > kMaxGrowthFactor * source.size() + kMaxGrowthSlack) {
    return std::nullopt;
  }
  return text;
}

TranslationPlan::TranslationPlan(const PageText& page) {
  slots_.resize(page.slots.size());
  // Visible slots first, then the rest, each in page order.
  std::vector<size_t> order;
  order.reserve(page.slots.size());
  for (size_t i = 0; i < page.slots.size(); ++i) {
    if (page.slots[i].in_viewport) {
      order.push_back(i);
    }
  }
  for (size_t i = 0; i < page.slots.size(); ++i) {
    if (!page.slots[i].in_viewport) {
      order.push_back(i);
    }
  }

  std::vector<size_t> batch;
  uint32_t batch_tokens = 0;
  auto close_batch = [&] {
    if (!batch.empty()) {
      queue_.push_back(std::move(batch));
      batch.clear();
    }
    batch_tokens = 0;
  };
  for (size_t slot : order) {
    SlotState& state = slots_[slot];
    state.original = page.slots[slot].text;
    state.pieces = web_ai::SplitForTranslation(state.original,
                                               web_ai::kTranslatorPieceTokens);
    state.results.resize(state.pieces.size());
    for (size_t piece = 0; piece < state.pieces.size(); ++piece) {
      if (!state.pieces[piece].translate) {
        continue;
      }
      const uint32_t tokens =
          web_ai::TranslatorBatchTextTokens(state.pieces[piece].text);
      if (batch.size() == web_ai::kTranslatorMaxBatchTexts ||
          (!batch.empty() &&
           web_ai::AddTokens(batch_tokens, tokens) >
               web_ai::kTranslatorPieceTokens)) {
        close_batch();
      }
      batch.push_back(units_.size());
      batch_tokens = web_ai::AddTokens(batch_tokens, tokens);
      units_.push_back({slot, piece, /*settled=*/false});
      ++state.open;
    }
  }
  close_batch();
}

TranslationPlan::~TranslationPlan() = default;

Batch TranslationPlan::MakeBatch(std::vector<size_t> units) const {
  Batch batch;
  batch.texts.reserve(units.size());
  for (size_t unit : units) {
    const Unit& u = units_[unit];
    batch.texts.push_back(slots_[u.slot].pieces[u.piece].text);
  }
  batch.units = std::move(units);
  return batch;
}

std::optional<Batch> TranslationPlan::NextBatch() {
  if (queue_.empty()) {
    return std::nullopt;
  }
  std::vector<size_t> units = std::move(queue_.front());
  queue_.pop_front();
  return MakeBatch(std::move(units));
}

void TranslationPlan::Settle(size_t unit,
                             std::optional<std::string> text,
                             std::vector<SlotWrite>* writes) {
  Unit& u = units_[unit];
  if (u.settled) {
    return;  // A batch answered twice changes nothing.
  }
  u.settled = true;
  SlotState& state = slots_[u.slot];
  ++settled_;
  if (!text) {
    ++failed_;
  }
  state.results[u.piece] = std::move(text);
  --state.open;
  if (state.open > 0) {
    return;
  }
  // Every piece is settled: the slot's text, with the translated pieces and
  // everything between them as it was.
  std::string joined;
  for (size_t piece = 0; piece < state.pieces.size(); ++piece) {
    const std::optional<std::string>& result = state.results[piece];
    joined += (state.pieces[piece].translate && result)
                  ? *result
                  : state.pieces[piece].text;
  }
  if (joined != state.original) {
    writes->push_back({u.slot, std::move(joined)});
  }
}

std::vector<SlotWrite> TranslationPlan::AcceptReply(
    const Batch& batch,
    const std::vector<std::string>& translations) {
  if (translations.size() != batch.units.size() ||
      batch.texts.size() != batch.units.size()) {
    return RejectBatch(batch);
  }
  std::vector<SlotWrite> writes;
  std::vector<size_t> retry;
  for (size_t i = 0; i < batch.units.size(); ++i) {
    if (batch.units[i] >= units_.size() || units_[batch.units[i]].settled) {
      continue;
    }
    std::optional<std::string> text =
        CheckTranslation(batch.texts[i], translations[i]);
    if (!text && batch.units.size() > 1) {
      retry.push_back(batch.units[i]);
      continue;
    }
    // Translated, or failed alone: a failed piece keeps its original
    // (Settle()), never anything else.
    Settle(batch.units[i], std::move(text), &writes);
  }
  // Each one that failed in company gets a batch of its own, at the front,
  // in page order.
  for (auto it = retry.rbegin(); it != retry.rend(); ++it) {
    queue_.push_front({*it});
  }
  return writes;
}

std::vector<SlotWrite> TranslationPlan::RejectBatch(const Batch& batch) {
  std::vector<SlotWrite> writes;
  std::vector<size_t> units;
  for (size_t unit : batch.units) {
    if (unit < units_.size() && !units_[unit].settled) {
      units.push_back(unit);
    }
  }
  if (units.size() > 1) {
    const size_t half = (units.size() + 1) / 2;
    queue_.push_front(std::vector<size_t>(units.begin() + half, units.end()));
    queue_.push_front(std::vector<size_t>(units.begin(), units.begin() + half));
    return writes;
  }
  for (size_t unit : units) {
    Settle(unit, std::nullopt, &writes);
  }
  return writes;
}

// ---------------------------------------------------------------------------
// Reports
// ---------------------------------------------------------------------------

Report ReportForJob(const JobResult& result) {
  Report report;
  const bool wrote = result.written_slots > 0;
  switch (result.end) {
    case JobEnd::kFinished:
      if (result.units == 0) {
        report.problem = Problem::kNothingToTranslate;
        report.partial = result.truncated;
        return report;
      }
      if (!wrote && result.failed_units == result.units) {
        report.error = TranslateError::kTranslation;
        report.problem = Problem::kModelErrors;
        return report;
      }
      report.partial = result.truncated || result.failed_units > 0;
      return report;
    case JobEnd::kStopped:
      report.problem = Problem::kStopped;
      break;
    case JobEnd::kTimedOut:
      report.problem = Problem::kTimedOut;
      break;
    case JobEnd::kModelErrors:
      report.problem = Problem::kModelErrors;
      break;
    case JobEnd::kNoModel:
      report.error = TranslateError::kInitialization;
      report.problem = Problem::kNoModel;
      return report;
    case JobEnd::kDeviceUnsupported:
      report.error = TranslateError::kInitialization;
      report.problem = Problem::kDeviceUnsupported;
      return report;
    case JobEnd::kModelFailed:
      report.error = TranslateError::kInitialization;
      report.problem = Problem::kModelFailed;
      return report;
    case JobEnd::kPageUnreadable:
      report.problem = Problem::kPageUnreadable;
      break;
  }
  // Ended early: what was written stays, and counts as a (partial)
  // translation so "Show original" can undo it.
  if (wrote) {
    report.partial = true;
    return report;
  }
  report.error = result.end == JobEnd::kTimedOut ? TranslateError::kTimeout
                                                 : TranslateError::kTranslation;
  return report;
}

Report ReportForPair(PairProblem problem) {
  Report report;
  switch (problem) {
    case PairProblem::kNone:
      break;
    case PairProblem::kUnknownSource:
      report.error = TranslateError::kUnknownLanguage;
      report.problem = Problem::kUnknownSource;
      break;
    case PairProblem::kUnsupportedSource:
    case PairProblem::kUnsupportedTarget:
      report.error = TranslateError::kUnsupportedLanguage;
      report.problem = Problem::kUnsupportedPair;
      break;
    case PairProblem::kIdentical:
      report.error = TranslateError::kIdenticalLanguages;
      report.problem = Problem::kIdentical;
      break;
  }
  return report;
}

// ---------------------------------------------------------------------------
// Where page translation is on
// ---------------------------------------------------------------------------

StartDecision DecideStart(bool moltnet, PairProblem problem) {
  StartDecision decision;
  if (PageTranslationOff(moltnet)) {
    // An error, not a crash: Translate shows the bubble's error step, and
    // the footnote says why.
    decision.report.error = TranslateError::kInitialization;
    decision.report.problem = Problem::kOffInMoltNet;
    return decision;
  }
  decision.report = ReportForPair(problem);
  decision.start = problem == PairProblem::kNone;
  return decision;
}

}  // namespace molt_ai::page_translate
