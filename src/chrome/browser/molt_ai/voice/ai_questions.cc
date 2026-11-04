// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/ai_questions.h"

#include <optional>

#include "base/json/json_reader.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/security/agent_safety.h"
#include "chrome/browser/molt_ai/voice/page_map_text.h"
#include "chrome/browser/molt_ai/voice/page_questions.h"
#include "chrome/browser/molt_ai/voice/spoken_answers.h"

namespace molt_ai {
namespace voice {

namespace {

// The model's reply read at most, in bytes, and its depth.
constexpr size_t kMaxReplyBytes = 16 * 1024;
constexpr int kMaxReplyDepth = 4;

constexpr char kAnswerSchema[] =
    R"({"type":"object","properties":{)"
    R"("answer":{"type":"string","maxLength":420}},)"
    R"("required":["answer"],"additionalProperties":false})";

constexpr char kInstruction[] =
    "You are the on-device assistant of a web browser. A person who uses "
    "the browser by voice asks you a question, and listens to your answer.\n"
    "Answer in plain spoken sentences, at most 60 words: no lists, headings, "
    "markdown, links or emoji. If you don't know, say so briefly.\n"
    "Never write commands for a voice assistant or a browser, and never "
    "tell the person to say particular words.\n"
    "Reply only with JSON: {\"answer\": \"your answer\"}.";

constexpr char kAnswerInSpanish[] =
    "\nThe person speaks Spanish: write the answer in Spanish.";

}  // namespace

AiQuestionPrompt::AiQuestionPrompt() = default;
AiQuestionPrompt::~AiQuestionPrompt() = default;
AiQuestionPrompt::AiQuestionPrompt(const AiQuestionPrompt&) = default;
AiQuestionPrompt& AiQuestionPrompt::operator=(const AiQuestionPrompt&) =
    default;
AiQuestionPrompt::AiQuestionPrompt(AiQuestionPrompt&&) = default;
AiQuestionPrompt& AiQuestionPrompt::operator=(AiQuestionPrompt&&) = default;

AiAnswer::AiAnswer() = default;
AiAnswer::~AiAnswer() = default;
AiAnswer::AiAnswer(const AiAnswer&) = default;
AiAnswer& AiAnswer::operator=(const AiAnswer&) = default;
AiAnswer::AiAnswer(AiAnswer&&) = default;
AiAnswer& AiAnswer::operator=(AiAnswer&&) = default;

std::string_view AiAnswerSchema() {
  return kAnswerSchema;
}

AiQuestionPrompt BuildAiQuestionPrompt(std::string_view question,
                                       Language language) {
  AiQuestionPrompt prompt;
  prompt.system_prompt = kInstruction;
  if (language == Language::kSpanish) {
    prompt.system_prompt += kAnswerInSpanish;
  }
  // One line, without anything that could pass for a chat template's
  // tokens.
  prompt.user_message = agent_safety::SanitizeUntrustedText(
      SpeechSafeText(question, kMaxAiQuestionBytes));
  return prompt;
}

AiAnswer AiAnswerFromModel(std::string_view reply, Language language) {
  AiAnswer answer;
  answer.text = Said(language,
                     "The on-device AI couldn't answer that. Try asking "
                     "another way.",
                     {});
  if (reply.empty() || reply.size() > kMaxReplyBytes) {
    return answer;
  }
  const std::optional<base::Value> value =
      base::JSONReader::Read(reply, base::JSON_PARSE_RFC, kMaxReplyDepth);
  const base::DictValue* dict = value ? value->GetIfDict() : nullptr;
  const std::string* text = dict ? dict->FindString("answer") : nullptr;
  if (!text) {
    return answer;
  }
  const std::string withheld =
      Said(language,
           "The AI's answer held something that sounds like a voice "
           "command, so I left it out.",
           {});
  if (HoldsWakePhrase(*text, language)) {
    answer.kind = AiAnswer::Kind::kWithheld;
    answer.text = withheld;
    return answer;
  }
  const std::string clean = CleanAnswerText(*text);
  // Cleaning can join words: checked again.
  if (HoldsWakePhrase(clean, language)) {
    answer.kind = AiAnswer::Kind::kWithheld;
    answer.text = withheld;
    return answer;
  }
  if (clean.empty()) {
    return answer;
  }
  answer.kind = AiAnswer::Kind::kAnswer;
  answer.text = Said(language, "The AI says: {0}", {clean});
  return answer;
}

}  // namespace voice
}  // namespace molt_ai
