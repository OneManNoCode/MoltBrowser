// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// RunQuestions: what a running workflow asks the user, and the answer.
//
// AutomationRunner asks before it acts on an element it found only by a
// weak fallback locator (automation_locators.h), and before it calls a
// site's tool that may change something when the workflow isn't trusted
// for that site (automation_tool_steps.h). The question waits here until
// the user answers it in the Agent studio (a real click: the studio's
// handler checks the user activation), the run stops, or it times out;
// the studio lists the questions of the workflows it shows.
//
// The text is the browser's, with page text in it cleaned (one line,
// hidden characters shown) by whoever asks; the studio renders it as text.
//
// Process-wide, UI thread only (like AgentInboxRegistry).

#ifndef CHROME_BROWSER_MOLT_AI_AUTOMATION_RUN_QUESTIONS_H_
#define CHROME_BROWSER_MOLT_AI_AUTOMATION_RUN_QUESTIONS_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"

namespace base {
template <typename T>
class NoDestructor;
}  // namespace base

namespace molt_ai {
namespace automation {

// What a question is about: the studio words its buttons by it.
inline constexpr char kQuestionWeakMatch[] = "weak_match";
inline constexpr char kQuestionSiteTool[] = "site_tool";

// How long a run waits for an answer; then the step stops.
inline constexpr int kQuestionTimeoutSeconds = 120;

// A question's text is capped (characters are the asker's business; this
// is a backstop in bytes).
inline constexpr size_t kMaxQuestionBytes = 2048;

struct RunQuestion {
  RunQuestion();
  ~RunQuestion();
  RunQuestion(const RunQuestion&);
  RunQuestion& operator=(const RunQuestion&);

  int64_t id = 0;
  std::string script_id;
  int step = 0;  // 0-based
  std::string kind;
  std::string text;
};

class RunQuestions {
 public:
  // true: the user allowed it; false: they declined.
  using AnswerCallback = base::OnceCallback<void(bool allowed)>;

  static RunQuestions* Get();

  RunQuestions(const RunQuestions&) = delete;
  RunQuestions& operator=(const RunQuestions&) = delete;

  // Asks |text| for step |step| of workflow |script_id|; returns the
  // question's id (never 0). A runner asks one question at a time; two
  // runs of one workflow may each have one.
  int64_t Ask(const std::string& script_id,
              int step,
              const std::string& kind,
              const std::string& text,
              AnswerCallback answer);

  // The asker no longer waits (it timed out, the run stopped): the
  // callback is dropped without running.
  void Withdraw(int64_t id);

  // The newest question |script_id| waits on, if any.
  std::optional<RunQuestion> PendingFor(const std::string& script_id) const;

  // The user's answer to question |id| of |script_id|. Runs its callback
  // once and forgets the question; false if there is no such question
  // (answered already, withdrawn, another workflow's).
  bool Answer(int64_t id, const std::string& script_id, bool allowed);

  size_t size_for_testing() const { return pending_.size(); }

 private:
  friend class base::NoDestructor<RunQuestions>;

  struct Pending {
    Pending();
    ~Pending();
    Pending(Pending&&);
    Pending& operator=(Pending&&);

    RunQuestion question;
    AnswerCallback answer;
  };

  RunQuestions();
  ~RunQuestions();

  int64_t next_id_ = 1;
  std::map<int64_t, Pending> pending_;
};

}  // namespace automation
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AUTOMATION_RUN_QUESTIONS_H_
