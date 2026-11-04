// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// "Ask the AI ..." in voice mode (docs/design/TOUCHLESS_BROWSING.md §2, "Run
// the browser itself": the AI chat): a question for the on-device model,
// answered in a few spoken sentences.
//
//   - The prompt: a fixed instruction (plain sentences to be read aloud, at
//     most 60 words, say so when it doesn't know, never commands for a
//     voice assistant or a browser), in the window's language, and the
//     question, one line, sanitized (agent_safety::SanitizeUntrustedText():
//     nothing that passes for a chat template's tokens).
//   - The answer is constrained (JSON Schema) to {"answer": string of at
//     most kMaxAiAnswerChars}, and treated as hostile, as a page's answer
//     is (page_questions.h): parsed strictly, cleaned (CleanAnswerText()),
//     never said when it holds the wake phrase, and said as "The AI says:
//     ...". It is read as page text (VoiceReader::ReadText()): never a
//     command, a URL, a click or a step.
//   - Nothing is stored, and voice mode is off in MoltNet windows.
//
// base::Value and std types; unit-tested in molt_ai_unittests and fuzzed by
// molt_page_questions_fuzzer. The browser's side is voice_ai_answers.h.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_AI_QUESTIONS_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_AI_QUESTIONS_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "chrome/browser/molt_ai/voice/spoken_text.h"

namespace molt_ai {
namespace voice {

// The answer said, in code points, and the model's visible tokens.
inline constexpr size_t kMaxAiAnswerChars = 420;
inline constexpr int kAiAnswerMaxTokens = 220;
// The question put to the model, in bytes.
inline constexpr size_t kMaxAiQuestionBytes = 500;

// {"answer": string of at most kMaxAiAnswerChars}.
std::string_view AiAnswerSchema();

struct AiQuestionPrompt {
  AiQuestionPrompt();
  ~AiQuestionPrompt();
  AiQuestionPrompt(const AiQuestionPrompt&);
  AiQuestionPrompt& operator=(const AiQuestionPrompt&);
  AiQuestionPrompt(AiQuestionPrompt&&);
  AiQuestionPrompt& operator=(AiQuestionPrompt&&);

  std::string system_prompt;
  std::string user_message;
};

// The prompt for |question| (the user's words, as heard) in |language|.
// Empty |user_message| when nothing of the question is left once cleaned.
AiQuestionPrompt BuildAiQuestionPrompt(std::string_view question,
                                       Language language);

struct AiAnswer {
  AiAnswer();
  ~AiAnswer();
  AiAnswer(const AiAnswer&);
  AiAnswer& operator=(const AiAnswer&);
  AiAnswer(AiAnswer&&);
  AiAnswer& operator=(AiAnswer&&);

  enum class Kind : uint8_t {
    kAnswer,    // "The AI says: ..." (model output: read as page text)
    kWithheld,  // it held the wake phrase: the browser's own words
    kNoAnswer,  // empty, malformed or oversized: the browser's own words
  };
  Kind kind = Kind::kNoAnswer;
  // Speech-safe.
  std::string text;
};

// The model's reply (JSON text under AiAnswerSchema(); untrusted) as what
// the browser says in |language|.
AiAnswer AiAnswerFromModel(std::string_view reply, Language language);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_AI_QUESTIONS_H_
