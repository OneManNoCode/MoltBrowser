// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Questions about the page in a window's voice mode
// (docs/design/TOUCHLESS_BROWSING.md §5.4): the loop's kAskPage steps,
// answered by the on-device model (the inference service, through
// BrowserAIRuntime, as the planner fallback calls it) from the page's text
// (page_questions.h).
//
//   - The page's text is reading's, picked for the question and the
//     model's budget, inside the untrusted fence; the answer is
//     constrained to one bounded string and treated as hostile
//     (PageAnswerFromModel()).
//   - No tools and no actions: the outcome is words for the voice loop to
//     say. An answer from the page is read as page text is
//     (VoiceReader::ReadText()): in chunks, each said to the listening
//     session as it starts, so the microphone hearing it back is the
//     browser's own voice, never a command or a wake phrase.
//   - One question at a time: a new one, Cancel() ("stop", barge-in, any
//     new command) and a page change end the one running, and the
//     inference request is cancelled. A generation that takes longer than
//     kTimeLimit is stopped and said to be.
//   - Without a model (none installed, a computer that can't run one, a
//     load that failed) it says so plainly.
//   - "Tell me more" goes on with the next passages of the same page; it
//     is forgotten when the page changes or voice mode stops.
//   - Nothing is stored: the page's text and the question live in memory
//     until then. Voice mode is off in MoltNet windows, so this never runs
//     there.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_PAGE_QUESTIONS_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_PAGE_QUESTIONS_H_

#include <cstdint>
#include <memory>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "chrome/browser/molt_ai/runtime/browser_ai_runtime.h"
#include "chrome/browser/molt_ai/voice/page_map.h"
#include "chrome/browser/molt_ai/voice/page_questions.h"
#include "chrome/browser/molt_ai/voice/page_reading.h"

namespace molt_ai {
namespace voice {

struct SiteSkill;

class VoicePageQuestions {
 public:
  // What the browser says, once the question is done.
  struct Outcome {
    // Speech-safe.
    std::string text;
    // An answer from the page (read as page text), or the browser's own
    // words.
    bool from_page = false;
  };
  using DoneCallback = base::OnceCallback<void(Outcome)>;
  // Words the browser says while it works ("Let me look at the page.").
  using ProgressCallback = base::RepeatingCallback<void(const std::string&)>;

  // A model in place of the on-device runtime, for browser tests: it gets
  // the request as the runtime would (the chat prompt and the options) and
  // answers at once.
  using ModelForTesting =
      base::RepeatingCallback<GenerationResult(const ChatPrompt& prompt,
                                               const PromptOptions& options)>;
  static void SetModelForTesting(ModelForTesting model);
  static bool HasModelForTesting();

  // The longest the model may take to answer, once it is loaded; and when
  // the browser says it is still working on it.
  static constexpr base::TimeDelta kTimeLimit = base::Seconds(40);
  static constexpr base::TimeDelta kStillWorking = base::Seconds(12);

  VoicePageQuestions();
  VoicePageQuestions(const VoicePageQuestions&) = delete;
  VoicePageQuestions& operator=(const VoicePageQuestions&) = delete;
  ~VoicePageQuestions();

  // Asks the question |kind| (|text|: as heard, or the topic) about page
  // |page_id|, whose accessibility tree is |tree| and whose site's skill is
  // |skill| (null for none). kMore follows the last question on the same
  // page. |progress| may run while it works; |done| runs once, unless the
  // question is cancelled first. Ends the question running, if any.
  void Ask(PageQuestionKind kind,
           const std::string& text,
           int64_t page_id,
           const PageTree& tree,
           const SiteSkill* skill,
           ProgressCallback progress,
           DoneCallback done);

  // Ends the question running, if any: its request is cancelled, and
  // nothing more is said for it. "Tell me more" still follows it.
  void Cancel();

  // The active tab shows page |page_id| now: a question about another page
  // ends (its |done| hears that the page changed), and "tell me more" is
  // forgotten.
  void PageChanged(int64_t page_id);

  // Forgets everything (voice mode is off).
  void Clear();

  // A question is being answered.
  bool running() const { return run_ != 0 && static_cast<bool>(done_); }

 private:
  struct Followed;
  struct ModelCheck;

  // Whether the on-device model is there (loaded, or loaded now), from a
  // worker.
  static ModelCheck EnsureModel();
  void OnModelChecked(int run,
                      ChatPrompt prompt,
                      PromptOptions options,
                      ModelCheck check);
  void Generate(int run, ChatPrompt prompt, PromptOptions options);
  void OnReply(int run, GenerationResult result);
  void OnStillWorking(int run);
  void OnTimeLimit(int run);
  // The question is done: |done| hears |text|.
  void Finish(std::string text, bool from_page);
  // The question is done without the model (the browser's own |text|),
  // from a task of its own.
  void FinishSoon(const char* text);
  // Cancels the inference request, if one is out.
  void CancelRequest();

  // The last question and its page's text, for "tell me more".
  std::unique_ptr<Followed> last_;
  // The question running: its run (0 for none), its page, its request.
  int run_ = 0;
  int next_run_ = 0;
  int64_t page_id_ = -1;
  uint64_t request_id_ = 0;
  // A worker generates with |request_id_| (and releases it).
  bool generating_ = false;
  PageQuestion question_;
  ProgressCallback progress_;
  DoneCallback done_;
  base::OneShotTimer still_working_timer_;
  base::OneShotTimer time_limit_timer_;
  base::WeakPtrFactory<VoicePageQuestions> weak_factory_{this};
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_PAGE_QUESTIONS_H_
