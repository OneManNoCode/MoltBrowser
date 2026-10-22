// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Summarizer, Writer and Rewriter options -> a prompt for the user's open
// model, and the LanguageModel session's own instructions. Pure functions
// of the options, standard library only, tested in molt_ai_unittests.
//
// Trust: the fields mirror molt_ai::ChatPrompt's trust labels.
//  * system_prompt and context_preamble are browser-authored. They are built
//    only from enums and from language tags that passed IsValidLanguageTag(),
//    never from page text.
//  * context (the shared and per-call context) and user_message (the input)
//    are page text. The inference service neutralizes chat-template special
//    tokens in them, so a page can't forge turns.

#ifndef CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_PROMPTS_H_
#define CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_PROMPTS_H_

#include <cstdint>
#include <string>
#include <string_view>

namespace molt_ai::web_ai {

// The writing-assistance options (blink::mojom::AISummarizer*, AIWriter*,
// AIRewriter* enums).
enum class SummarizerType { kTldr, kKeyPoints, kTeaser, kHeadline };
enum class SummarizerLength { kShort, kMedium, kLong };
enum class TextFormat { kPlainText, kMarkdown };
enum class WriterTone { kFormal, kNeutral, kCasual };
enum class WriterLength { kShort, kMedium, kLong };
enum class RewriterTone { kAsIs, kMoreFormal, kMoreCasual };
enum class RewriterFormat { kAsIs, kPlainText, kMarkdown };
enum class RewriterLength { kAsIs, kShorter, kLonger };

struct SummarizerOptions {
  SummarizerType type = SummarizerType::kKeyPoints;
  TextFormat format = TextFormat::kMarkdown;
  SummarizerLength length = SummarizerLength::kShort;
  // Empty, or a tag that passed IsValidLanguageTag().
  std::string output_language;
};

struct WriterOptions {
  WriterTone tone = WriterTone::kNeutral;
  TextFormat format = TextFormat::kMarkdown;
  WriterLength length = WriterLength::kShort;
  std::string output_language;
};

struct RewriterOptions {
  RewriterTone tone = RewriterTone::kAsIs;
  RewriterFormat format = RewriterFormat::kAsIs;
  RewriterLength length = RewriterLength::kAsIs;
  std::string output_language;
};

struct TaskPrompt {
  std::string system_prompt;
  std::string context_preamble;
  std::string context;
  std::string user_message;
  uint32_t max_output_tokens = 0;
  float temperature = 0.3f;
  uint32_t top_k = 40;
};

// Shared context (from create()) and the call's context, as one block.
std::string CombineContext(std::string_view shared_context,
                           std::string_view context);

TaskPrompt BuildSummarizerPrompt(const SummarizerOptions& options,
                                 std::string_view shared_context,
                                 std::string_view context,
                                 std::string_view input);

TaskPrompt BuildWriterPrompt(const WriterOptions& options,
                             std::string_view shared_context,
                             std::string_view context,
                             std::string_view input);

TaskPrompt BuildRewriterPrompt(const RewriterOptions& options,
                               std::string_view shared_context,
                               std::string_view context,
                               std::string_view input);

// Reply budgets, model-independent.
uint32_t SummarizerMaxOutputTokens(SummarizerType type,
                                   SummarizerLength length);
uint32_t WriterMaxOutputTokens(WriterLength length);
uint32_t RewriterMaxOutputTokens(RewriterLength length,
                                 uint32_t input_tokens);

// LanguageModel: MoltBrowser's instructions for a page's session. The
// page's own system prompt is page text and goes in the context, after
// kPageInstructionsPreamble. |output_language|: empty, or a tag that passed
// IsValidLanguageTag().
std::string BuildLanguageModelSystemPrompt(std::string_view output_language);
inline constexpr char kPageInstructionsPreamble[] =
    "\n\nThe web page set these instructions for the conversation:\n";

}  // namespace molt_ai::web_ai

#endif  // CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_PROMPTS_H_
