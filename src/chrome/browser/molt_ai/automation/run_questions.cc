// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/run_questions.h"

#include <utility>

#include "base/no_destructor.h"

namespace molt_ai {
namespace automation {

RunQuestion::RunQuestion() = default;
RunQuestion::~RunQuestion() = default;
RunQuestion::RunQuestion(const RunQuestion&) = default;
RunQuestion& RunQuestion::operator=(const RunQuestion&) = default;

RunQuestions::Pending::Pending() = default;
RunQuestions::Pending::~Pending() = default;
RunQuestions::Pending::Pending(Pending&&) = default;
RunQuestions::Pending& RunQuestions::Pending::operator=(Pending&&) = default;

RunQuestions::RunQuestions() = default;
RunQuestions::~RunQuestions() = default;

// static
RunQuestions* RunQuestions::Get() {
  static base::NoDestructor<RunQuestions> instance;
  return instance.get();
}

int64_t RunQuestions::Ask(const std::string& script_id,
                          int step,
                          const std::string& kind,
                          const std::string& text,
                          AnswerCallback answer) {
  Pending p;
  p.question.id = next_id_++;
  p.question.script_id = script_id;
  p.question.step = step;
  p.question.kind = kind;
  p.question.text = text;
  if (text.size() > kMaxQuestionBytes) {
    // Cut before the character that straddles the cap, never inside it.
    size_t end = kMaxQuestionBytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
      --end;
    p.question.text.resize(end);
  }
  p.answer = std::move(answer);
  const int64_t id = p.question.id;
  pending_.emplace(id, std::move(p));
  return id;
}

void RunQuestions::Withdraw(int64_t id) {
  pending_.erase(id);
}

std::optional<RunQuestion> RunQuestions::PendingFor(
    const std::string& script_id) const {
  // The newest: ids grow.
  for (auto it = pending_.rbegin(); it != pending_.rend(); ++it) {
    if (it->second.question.script_id == script_id)
      return it->second.question;
  }
  return std::nullopt;
}

bool RunQuestions::Answer(int64_t id,
                          const std::string& script_id,
                          bool allowed) {
  auto it = pending_.find(id);
  if (it == pending_.end() || it->second.question.script_id != script_id)
    return false;
  AnswerCallback answer = std::move(it->second.answer);
  pending_.erase(it);
  if (answer)
    std::move(answer).Run(allowed);
  return true;
}

}  // namespace automation
}  // namespace molt_ai
