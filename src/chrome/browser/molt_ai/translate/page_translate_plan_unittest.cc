// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/translate/page_translate_plan.h"

#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "chrome/browser/molt_ai/web_ai/web_ai_policy.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_translation.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai::page_translate {
namespace {

PageSlot Slot(std::string text, bool in_viewport = false) {
  PageSlot slot;
  slot.text = std::move(text);
  slot.in_viewport = in_viewport;
  return slot;
}

PageText Page(std::vector<PageSlot> slots) {
  PageText page;
  page.slots = std::move(slots);
  return page;
}

// Translates every text of every batch with |fn| until nothing is queued;
// returns the writes in order.
template <typename Fn>
std::vector<SlotWrite> RunAll(TranslationPlan& plan, Fn fn) {
  std::vector<SlotWrite> all;
  while (std::optional<Batch> batch = plan.NextBatch()) {
    std::vector<std::string> out;
    for (const std::string& text : batch->texts) {
      out.push_back(fn(text));
    }
    for (SlotWrite& w : plan.AcceptReply(*batch, out)) {
      all.push_back(std::move(w));
    }
  }
  return all;
}

std::string Upper(const std::string& text) {
  std::string out = text;
  for (char& c : out) {
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 'a' + 'A');
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Languages
// ---------------------------------------------------------------------------

TEST(PageTranslatePlanTest, LanguagesAreTheTranslatorTableInTranslateCodes) {
  const std::vector<std::string> codes = PageTranslateLanguageCodes();
  ASSERT_EQ(codes.size(), std::size(web_ai::kTranslatorLanguages));
  EXPECT_TRUE(std::is_sorted(codes.begin(), codes.end()));
  std::set<const web_ai::TranslatorLanguage*> entries;
  for (const std::string& code : codes) {
    const web_ai::TranslatorLanguage* language =
        web_ai::FindTranslatorLanguage(code);
    ASSERT_NE(language, nullptr) << code;
    entries.insert(language);
  }
  // Every table entry, once.
  EXPECT_EQ(entries.size(), codes.size());
  // Chinese as Translate names it.
  EXPECT_TRUE(std::count(codes.begin(), codes.end(), "zh-CN"));
  EXPECT_TRUE(std::count(codes.begin(), codes.end(), "zh-TW"));
  EXPECT_FALSE(std::count(codes.begin(), codes.end(), "zh"));
  EXPECT_FALSE(std::count(codes.begin(), codes.end(), "zh-Hant"));
}

TEST(PageTranslatePlanTest, ResolvePagePair) {
  PagePair p = ResolvePagePair("de", "en");
  EXPECT_EQ(p.problem, PairProblem::kNone);
  ASSERT_TRUE(p.pair.source && p.pair.target);
  EXPECT_STREQ(p.pair.source->code, "de");
  EXPECT_STREQ(p.pair.target->code, "en");

  p = ResolvePagePair("zh-CN", "zh-TW");
  EXPECT_EQ(p.problem, PairProblem::kNone);
  EXPECT_STREQ(p.pair.source->code, "zh");
  EXPECT_STREQ(p.pair.target->code, "zh-Hant");

  EXPECT_EQ(ResolvePagePair("und", "en").problem, PairProblem::kUnknownSource);
  EXPECT_EQ(ResolvePagePair("", "en").problem, PairProblem::kUnknownSource);
  EXPECT_EQ(ResolvePagePair("sv", "en").problem,
            PairProblem::kUnsupportedSource);
  EXPECT_EQ(ResolvePagePair("not a tag", "en").problem,
            PairProblem::kUnsupportedSource);
  EXPECT_EQ(ResolvePagePair("de", "sv").problem,
            PairProblem::kUnsupportedTarget);
  EXPECT_EQ(ResolvePagePair("de", "und").problem,
            PairProblem::kUnsupportedTarget);
  EXPECT_EQ(ResolvePagePair("en-GB", "en").problem, PairProblem::kIdentical);
  EXPECT_EQ(ResolvePagePair("pt-BR", "pt").problem, PairProblem::kIdentical);
}

// ---------------------------------------------------------------------------
// Checks
// ---------------------------------------------------------------------------

TEST(PageTranslatePlanTest, CheckTranslation) {
  EXPECT_EQ(CheckTranslation("Hallo", " Hello "), "Hello");
  EXPECT_EQ(CheckTranslation("Hallo", "\"Hello\""), "Hello");
  EXPECT_FALSE(CheckTranslation("Hallo", ""));
  EXPECT_FALSE(CheckTranslation("Hallo", " \n"));
  // Growth: at most 4x the text plus 256 bytes.
  const std::string source(100, 'a');
  EXPECT_TRUE(CheckTranslation(source, std::string(4 * 100 + 256, 'b')));
  EXPECT_FALSE(CheckTranslation(source, std::string(4 * 100 + 257, 'b')));
  EXPECT_TRUE(CheckTranslation("x", std::string(260, 'b')));
}

// ---------------------------------------------------------------------------
// Batches
// ---------------------------------------------------------------------------

TEST(PageTranslatePlanTest, EmptyAndUntranslatablePages) {
  TranslationPlan empty(Page({}));
  EXPECT_EQ(empty.unit_count(), 0u);
  EXPECT_TRUE(empty.finished());
  EXPECT_FALSE(empty.NextBatch());

  // Whitespace, digits and punctuation never reach the model.
  TranslationPlan none(Page({Slot("  "), Slot("2026 - 10:30"), Slot("...")}));
  EXPECT_EQ(none.unit_count(), 0u);
  EXPECT_TRUE(none.finished());
  EXPECT_FALSE(none.NextBatch());
}

TEST(PageTranslatePlanTest, VisibleTextGoesFirstThenPageOrder) {
  TranslationPlan plan(Page({Slot("one"), Slot("two", true), Slot("three"),
                             Slot("four", true)}));
  std::optional<Batch> batch = plan.NextBatch();
  ASSERT_TRUE(batch);
  EXPECT_EQ(batch->texts,
            (std::vector<std::string>{"two", "four", "one", "three"}));
  EXPECT_FALSE(plan.NextBatch());
}

TEST(PageTranslatePlanTest, BatchesRespectTheCountAndTokenCaps) {
  std::vector<PageSlot> slots;
  for (int i = 0; i < 100; ++i) {
    slots.push_back(Slot("word " + std::to_string(i)));
  }
  // Some long ones, near a piece's size.
  for (int i = 0; i < 10; ++i) {
    slots.push_back(Slot(std::string(1000, 'x') + " y"));
  }
  TranslationPlan plan(Page(std::move(slots)));
  size_t texts = 0;
  while (std::optional<Batch> batch = plan.NextBatch()) {
    ASSERT_FALSE(batch->texts.empty());
    ASSERT_EQ(batch->texts.size(), batch->units.size());
    EXPECT_LE(batch->texts.size(), web_ai::kTranslatorMaxBatchTexts);
    uint32_t tokens = 0;
    for (const std::string& text : batch->texts) {
      tokens += web_ai::TranslatorBatchTextTokens(text);
    }
    if (batch->texts.size() > 1) {
      EXPECT_LE(tokens, web_ai::kTranslatorPieceTokens);
    }
    // The prompt the batch becomes fits the reply budget.
    EXPECT_LE(web_ai::TranslatorMaxOutputTokens(tokens),
              web_ai::kTranslatorMaxReplyTokens);
    texts += batch->texts.size();
    plan.AcceptReply(*batch, batch->texts);
  }
  EXPECT_EQ(texts, plan.unit_count());
  EXPECT_TRUE(plan.finished());
}

TEST(PageTranslatePlanTest, WritesKeepTheTextAroundTheTranslation) {
  TranslationPlan plan(
      Page({Slot("  Hello world  "), Slot("Read more ›"), Slot("42")}));
  std::vector<SlotWrite> writes = RunAll(plan, Upper);
  ASSERT_EQ(writes.size(), 2u);
  EXPECT_EQ(writes[0].slot, 0u);
  EXPECT_EQ(writes[0].text, "  HELLO WORLD  ");
  EXPECT_EQ(writes[1].slot, 1u);
  EXPECT_EQ(writes[1].text, "READ MORE ›");
  EXPECT_TRUE(plan.finished());
  EXPECT_EQ(plan.failed_units(), 0u);
}

TEST(PageTranslatePlanTest, AnUnchangedTextIsNotWritten) {
  TranslationPlan plan(Page({Slot("MoltBrowser"), Slot("hello")}));
  std::vector<SlotWrite> writes =
      RunAll(plan, [](const std::string& t) { return t == "hello" ? "HI" : t; });
  ASSERT_EQ(writes.size(), 1u);
  EXPECT_EQ(writes[0].slot, 1u);
}

TEST(PageTranslatePlanTest, ALongSlotIsWrittenOnceAllItsPiecesAreBack) {
  // Several sentences, each about a third of a piece.
  std::string text;
  for (int i = 0; i < 9; ++i) {
    text += std::string(350, static_cast<char>('a' + i)) + ". ";
  }
  TranslationPlan plan(Page({Slot(text)}));
  ASSERT_GT(plan.unit_count(), 1u);
  std::vector<SlotWrite> writes = RunAll(plan, Upper);
  ASSERT_EQ(writes.size(), 1u);
  EXPECT_EQ(writes[0].text, Upper(text));
}

TEST(PageTranslatePlanTest, ATextThatFailsInABatchIsTriedAlone) {
  TranslationPlan plan(Page({Slot("one"), Slot("two"), Slot("three")}));
  std::optional<Batch> batch = plan.NextBatch();
  ASSERT_TRUE(batch);
  ASSERT_EQ(batch->texts.size(), 3u);
  // "two" comes back empty: the others are written, "two" goes again alone.
  std::vector<SlotWrite> writes = plan.AcceptReply(*batch, {"ONE", "", "THREE"});
  ASSERT_EQ(writes.size(), 2u);
  EXPECT_EQ(plan.settled_units(), 2u);
  std::optional<Batch> retry = plan.NextBatch();
  ASSERT_TRUE(retry);
  EXPECT_EQ(retry->texts, std::vector<std::string>{"two"});
  // Alone, it fails again: it keeps its text, and nothing is written.
  EXPECT_TRUE(plan.AcceptReply(*retry, {""}).empty());
  EXPECT_TRUE(plan.finished());
  EXPECT_EQ(plan.failed_units(), 1u);
  EXPECT_FALSE(plan.NextBatch());
}

TEST(PageTranslatePlanTest, ARejectedBatchIsSplitThenGivenUp) {
  std::vector<PageSlot> slots;
  for (int i = 0; i < 8; ++i) {
    slots.push_back(Slot("text " + std::string(1, static_cast<char>('a' + i))));
  }
  TranslationPlan plan(Page(std::move(slots)));
  std::optional<Batch> batch = plan.NextBatch();
  ASSERT_TRUE(batch);
  ASSERT_EQ(batch->texts.size(), 8u);
  // The wrong number of translations is a rejection.
  EXPECT_TRUE(plan.AcceptReply(*batch, {"x"}).empty());
  std::optional<Batch> first = plan.NextBatch();
  ASSERT_TRUE(first);
  EXPECT_EQ(first->texts.size(), 4u);
  EXPECT_EQ(first->texts.front(), "text a");
  // Every rejection halves, down to single texts that keep their original.
  size_t generations = 1;
  plan.RejectBatch(*first);
  while (std::optional<Batch> next = plan.NextBatch()) {
    ++generations;
    plan.RejectBatch(*next);
  }
  EXPECT_TRUE(plan.finished());
  EXPECT_EQ(plan.failed_units(), 8u);
  // A full binary tree over 8 texts: 15 generations at most.
  EXPECT_LE(generations, 15u);
}

TEST(PageTranslatePlanTest, AnsweringABatchTwiceChangesNothing) {
  TranslationPlan plan(Page({Slot("one"), Slot("two")}));
  std::optional<Batch> batch = plan.NextBatch();
  ASSERT_TRUE(batch);
  EXPECT_EQ(plan.AcceptReply(*batch, {"1", "2"}).size(), 2u);
  EXPECT_TRUE(plan.AcceptReply(*batch, {"1", "2"}).empty());
  EXPECT_TRUE(plan.RejectBatch(*batch).empty());
  EXPECT_EQ(plan.settled_units(), 2u);
  EXPECT_FALSE(plan.NextBatch());
}

TEST(PageTranslatePlanTest, AForgedBatchIsIgnored) {
  TranslationPlan plan(Page({Slot("one")}));
  Batch forged;
  forged.units = {7, 99};
  forged.texts = {"a", "b"};
  EXPECT_TRUE(plan.AcceptReply(forged, {"x", "y"}).empty());
  EXPECT_TRUE(plan.RejectBatch(forged).empty());
  EXPECT_EQ(plan.settled_units(), 0u);
}

// ---------------------------------------------------------------------------
// Reports
// ---------------------------------------------------------------------------

TEST(PageTranslatePlanTest, Reports) {
  JobResult r;
  r.end = JobEnd::kFinished;
  r.units = 10;
  r.written_slots = 5;
  Report report = ReportForJob(r);
  EXPECT_EQ(report.error, TranslateError::kNone);
  EXPECT_EQ(report.problem, Problem::kNone);
  EXPECT_FALSE(report.partial);

  r.failed_units = 2;
  EXPECT_TRUE(ReportForJob(r).partial);
  r.failed_units = 0;
  r.truncated = true;
  EXPECT_TRUE(ReportForJob(r).partial);

  // Every text failed and nothing changed: an error.
  r = JobResult();
  r.units = 3;
  r.failed_units = 3;
  report = ReportForJob(r);
  EXPECT_EQ(report.error, TranslateError::kTranslation);
  EXPECT_EQ(report.problem, Problem::kModelErrors);

  // Nothing to translate is not an error.
  r = JobResult();
  report = ReportForJob(r);
  EXPECT_EQ(report.error, TranslateError::kNone);
  EXPECT_EQ(report.problem, Problem::kNothingToTranslate);

  // Ending early with text written is a partial translation, so "Show
  // original" can undo it; without, an error.
  for (JobEnd end : {JobEnd::kStopped, JobEnd::kTimedOut,
                     JobEnd::kModelErrors, JobEnd::kPageUnreadable}) {
    r = JobResult();
    r.end = end;
    r.units = 4;
    r.written_slots = 1;
    report = ReportForJob(r);
    EXPECT_EQ(report.error, TranslateError::kNone);
    EXPECT_TRUE(report.partial);
    r.written_slots = 0;
    report = ReportForJob(r);
    EXPECT_NE(report.error, TranslateError::kNone);
    EXPECT_FALSE(report.partial);
  }
  r = JobResult();
  r.end = JobEnd::kTimedOut;
  EXPECT_EQ(ReportForJob(r).error, TranslateError::kTimeout);

  for (JobEnd end :
       {JobEnd::kNoModel, JobEnd::kDeviceUnsupported, JobEnd::kModelFailed}) {
    r = JobResult();
    r.end = end;
    EXPECT_EQ(ReportForJob(r).error, TranslateError::kInitialization);
  }
  r.end = JobEnd::kNoModel;
  EXPECT_EQ(ReportForJob(r).problem, Problem::kNoModel);

  EXPECT_EQ(ReportForPair(PairProblem::kNone).error, TranslateError::kNone);
  EXPECT_EQ(ReportForPair(PairProblem::kUnknownSource).error,
            TranslateError::kUnknownLanguage);
  EXPECT_EQ(ReportForPair(PairProblem::kUnsupportedSource).error,
            TranslateError::kUnsupportedLanguage);
  EXPECT_EQ(ReportForPair(PairProblem::kUnsupportedTarget).error,
            TranslateError::kUnsupportedLanguage);
  EXPECT_EQ(ReportForPair(PairProblem::kIdentical).error,
            TranslateError::kIdenticalLanguages);
}

TEST(PageTranslatePlanTest, NothingStartsInAMoltNetWindow) {
  EXPECT_TRUE(PageTranslationOff(/*moltnet=*/true));
  EXPECT_FALSE(PageTranslationOff(/*moltnet=*/false));

  // Whatever the manager asked, and whatever the languages: an error step.
  for (PairProblem problem :
       {PairProblem::kNone, PairProblem::kUnknownSource,
        PairProblem::kUnsupportedSource, PairProblem::kUnsupportedTarget,
        PairProblem::kIdentical}) {
    const StartDecision d = DecideStart(/*moltnet=*/true, problem);
    EXPECT_FALSE(d.start);
    EXPECT_EQ(d.report.error, TranslateError::kInitialization);
    EXPECT_EQ(d.report.problem, Problem::kOffInMoltNet);
  }

  // Elsewhere, only a pair the table covers starts.
  StartDecision d = DecideStart(/*moltnet=*/false, PairProblem::kNone);
  EXPECT_TRUE(d.start);
  EXPECT_EQ(d.report.error, TranslateError::kNone);
  d = DecideStart(/*moltnet=*/false, PairProblem::kIdentical);
  EXPECT_FALSE(d.start);
  EXPECT_EQ(d.report.error, TranslateError::kIdenticalLanguages);
  EXPECT_EQ(d.report.problem, Problem::kIdentical);
  d = DecideStart(/*moltnet=*/false, PairProblem::kUnsupportedTarget);
  EXPECT_FALSE(d.start);
  EXPECT_EQ(d.report.problem, Problem::kUnsupportedPair);
}

}  // namespace
}  // namespace molt_ai::page_translate
