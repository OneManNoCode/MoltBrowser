// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// "Ask the AI ..." in a window's voice mode (ai_questions.h): the question
// goes to the on-device model, through BrowserAIRuntime as the AI chat and
// questions about the page do, and its answer comes back as words to say.
//
//   - The answer is model output: hostile, cleaned and checked
//     (AiAnswerFromModel()), and read as page text is
//     (VoiceReader::ReadText()); nothing in it is ever run.
//   - One question at a time: a new one, Cancel() ("stop", barge-in, any new
//     command) ends the one running, its inference request cancelled. A
//     generation past kTimeLimit is stopped and said to be.
//   - Without a model, plainly: what to do instead.
//   - Nothing is stored. Voice mode is off in MoltNet windows.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_AI_ANSWERS_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_AI_ANSWERS_H_

#include <cstdint>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "chrome/browser/molt_ai/runtime/browser_ai_runtime.h"
#include "chrome/browser/molt_ai/voice/spoken_text.h"

namespace molt_ai {
namespace voice {

class VoiceAiAnswers {
 public:
  // What the browser says once the question is done: speech-safe; |model|
  // when it is the model's answer (read as page text), else the browser's
  // own words.
  using DoneCallback = base::OnceCallback<void(std::string text, bool model)>;
  // Words the browser says while it works ("Let me think.").
  using ProgressCallback = base::RepeatingCallback<void(const std::string&)>;

  // A model in place of the on-device runtime, for browser tests (as
  // VoicePageQuestions::ModelForTesting).
  using ModelForTesting =
      base::RepeatingCallback<GenerationResult(const ChatPrompt& prompt,
                                               const PromptOptions& options)>;
  static void SetModelForTesting(ModelForTesting model);
  static bool HasModelForTesting();

  static constexpr base::TimeDelta kTimeLimit = base::Seconds(40);
  static constexpr base::TimeDelta kStillWorking = base::Seconds(12);

  VoiceAiAnswers();
  VoiceAiAnswers(const VoiceAiAnswers&) = delete;
  VoiceAiAnswers& operator=(const VoiceAiAnswers&) = delete;
  ~VoiceAiAnswers();

  // Asks |question| in |language|. |progress| may run while it works;
  // |done| runs once, unless Cancel() comes first. Ends the one running.
  void Ask(const std::string& question,
           Language language,
           ProgressCallback progress,
           DoneCallback done);

  void Cancel();

  bool running() const { return run_ != 0 && static_cast<bool>(done_); }

 private:
  struct ModelCheck;

  static ModelCheck EnsureModel();
  void OnModelChecked(int run,
                      ChatPrompt prompt,
                      PromptOptions options,
                      ModelCheck check);
  void Generate(int run, ChatPrompt prompt, PromptOptions options);
  void OnReply(int run, GenerationResult result);
  void OnStillWorking(int run);
  void OnTimeLimit(int run);
  void Finish(std::string text, bool model);
  void FinishSoon(std::string text);
  void CancelRequest();

  int run_ = 0;
  int next_run_ = 0;
  Language language_ = Language::kEnglish;
  uint64_t request_id_ = 0;
  bool generating_ = false;
  ProgressCallback progress_;
  DoneCallback done_;
  base::OneShotTimer still_working_timer_;
  base::OneShotTimer time_limit_timer_;
  base::WeakPtrFactory<VoiceAiAnswers> weak_factory_{this};
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_AI_ANSWERS_H_
