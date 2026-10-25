// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/run_questions.h"

#include <optional>
#include <string>
#include <vector>

#include "base/functional/bind.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace automation {
namespace {

// The registry is process-wide: each test cleans up after itself.
class RunQuestionsTest : public ::testing::Test {
 protected:
  void TearDown() override {
    for (int64_t id : asked_)
      RunQuestions::Get()->Withdraw(id);
    EXPECT_EQ(RunQuestions::Get()->size_for_testing(), 0u);
  }

  int64_t Ask(const std::string& script_id,
              const std::string& text,
              std::vector<bool>* answers) {
    const int64_t id = RunQuestions::Get()->Ask(
        script_id, 2, kQuestionWeakMatch, text,
        base::BindOnce([](std::vector<bool>* out,
                          bool allowed) { out->push_back(allowed); },
                       answers));
    asked_.push_back(id);
    return id;
  }

  std::vector<int64_t> asked_;
};

TEST_F(RunQuestionsTest, AsksAndAnswersOnce) {
  std::vector<bool> answers;
  const int64_t id = Ask("wf", "Use it?", &answers);
  EXPECT_NE(id, 0);
  std::optional<RunQuestion> q = RunQuestions::Get()->PendingFor("wf");
  ASSERT_TRUE(q);
  EXPECT_EQ(q->id, id);
  EXPECT_EQ(q->step, 2);
  EXPECT_EQ(q->kind, kQuestionWeakMatch);
  EXPECT_EQ(q->text, "Use it?");
  EXPECT_FALSE(RunQuestions::Get()->PendingFor("other"));

  // Only for its own workflow, and once.
  EXPECT_FALSE(RunQuestions::Get()->Answer(id, "other", true));
  EXPECT_TRUE(answers.empty());
  EXPECT_TRUE(RunQuestions::Get()->Answer(id, "wf", true));
  EXPECT_EQ(answers, std::vector<bool>{true});
  EXPECT_FALSE(RunQuestions::Get()->Answer(id, "wf", false));
  EXPECT_EQ(answers.size(), 1u);
  EXPECT_FALSE(RunQuestions::Get()->PendingFor("wf"));
}

TEST_F(RunQuestionsTest, DeclinesAndWithdraws) {
  std::vector<bool> answers;
  const int64_t a = Ask("wf", "A?", &answers);
  EXPECT_TRUE(RunQuestions::Get()->Answer(a, "wf", false));
  EXPECT_EQ(answers, std::vector<bool>{false});

  // A withdrawn question is never answered.
  const int64_t b = Ask("wf", "B?", &answers);
  RunQuestions::Get()->Withdraw(b);
  EXPECT_FALSE(RunQuestions::Get()->Answer(b, "wf", true));
  EXPECT_EQ(answers.size(), 1u);
  EXPECT_FALSE(RunQuestions::Get()->PendingFor("wf"));
  RunQuestions::Get()->Withdraw(12345);  // unknown: nothing
}

TEST_F(RunQuestionsTest, TwoRunsOfOneWorkflowShowTheNewest) {
  std::vector<bool> first, second;
  const int64_t a = Ask("wf", "A?", &first);
  const int64_t b = Ask("wf", "B?", &second);
  EXPECT_NE(a, b);
  EXPECT_EQ(RunQuestions::Get()->PendingFor("wf")->id, b);
  EXPECT_TRUE(RunQuestions::Get()->Answer(b, "wf", true));
  EXPECT_EQ(RunQuestions::Get()->PendingFor("wf")->id, a);
  EXPECT_TRUE(first.empty());
  EXPECT_EQ(second, std::vector<bool>{true});
}

TEST_F(RunQuestionsTest, CapsTheText) {
  std::vector<bool> answers;
  // A multi-byte character straddling the cap is left out whole.
  std::string text(kMaxQuestionBytes - 1, 'a');
  text += "\xE2\x80\x9C" "more";
  Ask("wf", text, &answers);
  const std::string shown = RunQuestions::Get()->PendingFor("wf")->text;
  EXPECT_EQ(shown, std::string(kMaxQuestionBytes - 1, 'a'));
  Ask("wf2", std::string(kMaxQuestionBytes, 'b'), &answers);
  EXPECT_EQ(RunQuestions::Get()->PendingFor("wf2")->text.size(),
            kMaxQuestionBytes);
}

}  // namespace
}  // namespace automation
}  // namespace molt_ai
