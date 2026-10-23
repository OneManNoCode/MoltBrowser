// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Translating a whole page with the user's on-device model, instead of
// Google Translate: the pure logic. Design: docs/design/PAGE_TRANSLATION.md.
//
// The page's text is read from Translate's isolated world as "slots" (a
// visible text node, the document's title, an image's alt text or a text
// field's placeholder), each cut into pieces with the Translator API's
// splitter (web_ai::SplitForTranslation()). The pieces to translate are
// packed, visible ones first, into batches of at most
// web_ai::kTranslatorMaxBatchTexts texts and web_ai::kTranslatorPieceTokens
// tokens, each one generation under web_ai::BuildTranslatorBatchPrompt().
// Each translation is checked (CheckTranslation()); a batch that fails is
// split in two and tried again, a text that fails alone keeps its original.
// A slot is written back, as text, once all its pieces are settled.
//
// Standard library only: molt_ai_unittests tests it and
// molt_page_translate_fuzzer fuzzes it.

#ifndef CHROME_BROWSER_MOLT_AI_TRANSLATE_PAGE_TRANSLATE_PLAN_H_
#define CHROME_BROWSER_MOLT_AI_TRANSLATE_PAGE_TRANSLATE_PLAN_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/web_ai/web_ai_translation.h"

namespace molt_ai::page_translate {

// ---------------------------------------------------------------------------
// Caps
// ---------------------------------------------------------------------------

// What the isolated-world script reads from a page. Characters are UTF-16
// code units (JavaScript's length); the browser checks the same text again
// in UTF-8 bytes, at most 3 per code unit.
inline constexpr size_t kMaxSlots = 3000;
// A longer text is skipped (left as it is), not cut.
inline constexpr size_t kMaxSlotChars = 2000;
// The page's text in all; the script stops there and the page is
// translated up to that point.
inline constexpr size_t kMaxPageChars = 60000;
// DOM nodes the script visits before it stops, whatever it found.
inline constexpr size_t kMaxVisitedNodes = 60000;
inline constexpr size_t kMaxSlotBytes = 3 * kMaxSlotChars;
inline constexpr size_t kMaxPageBytes = 3 * kMaxPageChars;

// A translation longer than this many times its text, plus the slack, is
// refused: the model rambled or merged neighbouring texts into it.
inline constexpr size_t kMaxGrowthFactor = 4;
inline constexpr size_t kMaxGrowthSlack = 256;

// Batches in a row that fail as a whole before a job gives up (the model
// isn't producing usable translations).
inline constexpr int kMaxFailedBatchesInARow = 6;

// How long one page translation may run, model load included.
inline constexpr int kMaxTranslateMinutes = 15;

// ---------------------------------------------------------------------------
// Languages
// ---------------------------------------------------------------------------

// The languages Translate offers: the Translator API's table
// (web_ai::kTranslatorLanguages) in Translate's codes, which name Chinese
// by region ("zh-CN", "zh-TW") as its language detection and prefs do.
// TranslateService installs them as Translate's list at startup.
std::vector<std::string> PageTranslateLanguageCodes();

// Translate's code for one entry of the table ("zh-CN" for "zh").
std::string PageTranslateCode(const web_ai::TranslatorLanguage& language);

enum class PairProblem {
  kNone,
  // The page's language wasn't detected ("und" or empty).
  kUnknownSource,
  kUnsupportedSource,
  kUnsupportedTarget,
  kIdentical,
};

struct PagePair {
  web_ai::TranslatorPair pair;
  PairProblem problem = PairProblem::kNone;
};

// The pair for translating a page in |source| (the language
// TranslateManager::TranslatePage() was given, before it turns an
// unsupported one into "und") into |target|. On the device nothing can
// detect the language, so an unknown one is a problem, not "und".
PagePair ResolvePagePair(std::string_view source, std::string_view target);

// ---------------------------------------------------------------------------
// The page's text
// ---------------------------------------------------------------------------

enum class SlotKind {
  kText = 0,
  kTitle = 1,
  kAlt = 2,
  kPlaceholder = 3,
};
inline constexpr int kMaxSlotKind = 3;

struct PageSlot {
  SlotKind kind = SlotKind::kText;
  // On screen when the page was read: translated first.
  bool in_viewport = false;
  // Valid UTF-8 without NUL, at most kMaxSlotBytes.
  std::string text;
};

struct PageText {
  PageText();
  ~PageText();
  PageText(PageText&&);
  PageText& operator=(PageText&&);

  std::vector<PageSlot> slots;
  // The script stopped at a cap; the rest of the page isn't translated.
  bool truncated = false;
};

// A slot's new text.
struct SlotWrite {
  size_t slot = 0;
  std::string text;
};

// ---------------------------------------------------------------------------
// The plan
// ---------------------------------------------------------------------------

struct Batch {
  Batch();
  ~Batch();
  Batch(const Batch&);
  Batch& operator=(const Batch&);
  Batch(Batch&&);
  Batch& operator=(Batch&&);

  // Unit ids, in order, and their texts (page text, for the model).
  std::vector<size_t> units;
  std::vector<std::string> texts;
};

// The model's |translation| (valid UTF-8, from the reply) of |source| (the
// text that was sent): web_ai::CleanTranslation(), and refused if it grew
// more than kMaxGrowthFactor times plus kMaxGrowthSlack bytes.
std::optional<std::string> CheckTranslation(std::string_view source,
                                            std::string_view translation);

class TranslationPlan {
 public:
  explicit TranslationPlan(const PageText& page);
  ~TranslationPlan();
  TranslationPlan(const TranslationPlan&) = delete;
  TranslationPlan& operator=(const TranslationPlan&) = delete;

  // Pieces sent to the model, in all.
  size_t unit_count() const { return units_.size(); }
  // Translated, or given up.
  size_t settled_units() const { return settled_; }
  size_t failed_units() const { return failed_; }
  bool finished() const { return settled_ == units_.size(); }

  // The next batch, or nullopt when none is queued. Its units are out of
  // the queue until AcceptReply() or RejectBatch().
  std::optional<Batch> NextBatch();

  // |translations| is what the reply held for |batch| (one string per
  // text, or the batch is rejected). Each passes CheckTranslation() or is
  // tried again alone (or, if it was alone, keeps its original). Returns
  // the slots this settled, with their new text; a slot whose text didn't
  // change isn't returned.
  std::vector<SlotWrite> AcceptReply(
      const Batch& batch,
      const std::vector<std::string>& translations);

  // The batch got no usable reply. It is split in two, both halves first
  // in the queue; a single text keeps its original.
  std::vector<SlotWrite> RejectBatch(const Batch& batch);

 private:
  struct SlotState;
  struct Unit {
    size_t slot = 0;
    size_t piece = 0;
    bool settled = false;
  };

  // Settles |unit| with |text| (nullopt: keeps its original), and adds the
  // slot's write once all its pieces are settled.
  void Settle(size_t unit,
              std::optional<std::string> text,
              std::vector<SlotWrite>* writes);
  Batch MakeBatch(std::vector<size_t> units) const;

  std::vector<SlotState> slots_;
  std::vector<Unit> units_;
  // Batches to send, front first.
  std::deque<std::vector<size_t>> queue_;
  size_t settled_ = 0;
  size_t failed_ = 0;
};

// ---------------------------------------------------------------------------
// How a job ended, and what Translate is told
// ---------------------------------------------------------------------------

enum class JobEnd {
  // Every piece settled.
  kFinished,
  // The user stopped it.
  kStopped,
  // kMaxTranslateMinutes passed.
  kTimedOut,
  // kMaxFailedBatchesInARow, or the model failed mid-way.
  kModelErrors,
  // No model downloaded (nothing is downloaded for the user).
  kNoModel,
  // The device can't run local inference.
  kDeviceUnsupported,
  // A downloaded model couldn't be loaded.
  kModelFailed,
  // The page's text couldn't be read, or the page lost the translation's
  // state.
  kPageUnreadable,
};

struct JobResult {
  JobEnd end = JobEnd::kFinished;
  size_t units = 0;
  size_t failed_units = 0;
  size_t written_slots = 0;
  bool truncated = false;
};

// What the footnote explains (page_translate_messages.h).
enum class Problem {
  kNone,
  kNothingToTranslate,
  kNoModel,
  kDeviceUnsupported,
  kModelFailed,
  kUnknownSource,
  kUnsupportedPair,
  kIdentical,
  kPageUnreadable,
  kModelErrors,
  kStopped,
  kTimedOut,
  // A MoltNet window: page translation is off there (PageTranslationOff()).
  kOffInMoltNet,
};

// translate::TranslateErrors, without depending on //components/translate.
enum class TranslateError {
  kNone,
  kInitialization,
  kUnknownLanguage,
  kUnsupportedLanguage,
  kIdenticalLanguages,
  kTranslation,
  kTimeout,
};

struct Report {
  TranslateError error = TranslateError::kNone;
  Problem problem = Problem::kNone;
  // Some of the page stayed in its language (stopped, timed out, too long,
  // or texts the model couldn't translate).
  bool partial = false;
};

// Anything written is reported as translated, so "Show original" can undo
// it; a failure is reported only when the page is unchanged.
Report ReportForJob(const JobResult& result);
Report ReportForPair(PairProblem problem);

// ---------------------------------------------------------------------------
// Where page translation is on
// ---------------------------------------------------------------------------

// Page translation is off in MoltNet windows: a translated page sees the
// language it was translated into, which breaks MoltNet's uniform en-US and
// is a strong, stable identifier across sites (Tor Browser keeps
// translations off too). Translate then offers nothing by itself (no
// bubble, no address bar icon, no automatic translation); its menus still
// open the bubble, which says why. Decided by the window alone.
// docs/design/PAGE_TRANSLATION.md §5.
constexpr bool PageTranslationOff(bool moltnet) {
  return moltnet;
}

// Whether a translation the manager asked for may start: never where page
// translation is off (whatever the manager asked), nor for a pair the
// table doesn't cover. |report| says why not.
struct StartDecision {
  bool start = false;
  Report report;
};
StartDecision DecideStart(bool moltnet, PairProblem problem);

}  // namespace molt_ai::page_translate

#endif  // CHROME_BROWSER_MOLT_AI_TRANSLATE_PAGE_TRANSLATE_PLAN_H_
