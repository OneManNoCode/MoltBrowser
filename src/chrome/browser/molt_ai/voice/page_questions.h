// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Questions about the page, answered by the on-device model
// (docs/design/TOUCHLESS_BROWSING.md §5.4, "Questions about the page"):
// "what is this page about?", "read me the main points", "what does it
// say about the battery?", "who wrote this?", "tell me more".
//
//   1. The page's text is reading's (page_reading.h): the site skill's
//      content root and skips, else the main landmark, the first article,
//      the page; never a frame, a form or a credential field.
//   2. SelectPageText() picks what fits the model's budget: for "about"
//      and the main points, the title, the headings and the leading
//      paragraphs (and the first paragraph of each section); for a topic
//      or a question, the passages that best match its words (a linear
//      lexical score, no std::regex), with their headings. Everything is
//      capped, in bytes and in blocks.
//   3. BuildPageQuestionPrompt() puts the page's text inside the untrusted
//      fence (agent_safety::FenceUntrustedText()), after a fixed
//      instruction: answer only from the fenced text, at most about 60
//      words of plain sentences, and say when the page doesn't say. The
//      answer is constrained to {"found": bool, "answer": string} (JSON
//      Schema, constrained decoding), so it is one bounded string. No tools
//      and no actions: the answer is only said and shown.
//   4. PageAnswerFromModel() treats the model's reply as hostile (it was
//      written from page text): parsed strictly, cleaned of markup and
//      anything SpeechSafeText() drops, capped, and said as "The page says:
//      ...". An answer that holds voice mode's wake phrase ("Hey Molt,
//      open ...") is never said. Nothing in it is ever parsed as a command,
//      a URL or a step.
//
// Pure functions over base::Value (the reply) and std types, with
// agent_safety's fence; unit-tested in molt_ai_unittests and fuzzed by
// molt_page_questions_fuzzer. The browser's side (the model, the time
// limit, "stop") is voice_page_questions.h.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_PAGE_QUESTIONS_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_PAGE_QUESTIONS_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/voice/page_map.h"
#include "chrome/browser/molt_ai/voice/page_reading.h"
#include "chrome/browser/molt_ai/voice/voice_command.h"

namespace molt_ai {
namespace voice {

struct SiteSkill;

// The question's words, in bytes (the grammar's free-text cap).
inline constexpr size_t kMaxPageQuestionBytes = kMaxCommandTextBytes;
// The question's words that are scored against the page, at most (before
// the few related words some questions add).
inline constexpr size_t kMaxQueryTerms = 12;
// The answer said, in code points (about 60 words), and in words.
inline constexpr size_t kMaxPageAnswerChars = 420;
inline constexpr size_t kMaxPageAnswerWords = 70;
// The model's reply read at most, in bytes.
inline constexpr size_t kMaxModelReplyBytes = 16 * 1024;
// The answer's visible tokens (the schema's maxLength bounds it too).
inline constexpr int kPageAnswerMaxTokens = 220;

// A question about the page, as the grammar heard it (kAskPage).
struct PageQuestion {
  PageQuestion();
  ~PageQuestion();
  PageQuestion(const PageQuestion&);
  PageQuestion& operator=(const PageQuestion&);
  PageQuestion(PageQuestion&&);
  PageQuestion& operator=(PageQuestion&&);

  // Never kMore here: "tell me more" repeats the question it follows.
  PageQuestionKind kind = PageQuestionKind::kAbout;
  // The question as heard ("who wrote this"), or the topic for kTopic ("the
  // battery"): speech-safe, at most kMaxPageQuestionBytes.
  std::string text;
};

// |kind| and |text| from the grammar (VoiceCommand::page_question and
// ::text), cleaned and capped. kMore is kept as it is (the caller looks up
// the question it follows).
PageQuestion MakePageQuestion(PageQuestionKind kind, std::string_view text);

// How much of the page goes to the model.
struct PageTextBudget {
  // The page's text given (title, headings and passages), in bytes.
  size_t max_bytes = 6 * 1024;
  // Headings and passages given, at most.
  size_t max_blocks = 40;
  // One block, in code points; a longer one is cut at a word.
  size_t max_block_chars = 1200;
};

// The budget for a model whose context window is |context_tokens| tokens
// (0 when it isn't known): room for the instruction, the question and the
// answer, at about three bytes a token, and never more than the default
// (more page text is slower to read on a CPU, and the answer is short).
PageTextBudget PageTextBudgetFor(uint32_t context_tokens);

// What is given to the model: the title, and blocks of the reading
// document in reading order.
struct PageExcerpt {
  PageExcerpt();
  ~PageExcerpt();
  PageExcerpt(const PageExcerpt&);
  PageExcerpt& operator=(const PageExcerpt&);
  PageExcerpt(PageExcerpt&&);
  PageExcerpt& operator=(PageExcerpt&&);

  struct Part {
    // The block's index in the document.
    size_t block = 0;
    ReadingBlockKind kind = ReadingBlockKind::kParagraph;
    // Speech-safe, at most PageTextBudget::max_block_chars.
    std::string text;
  };

  // Speech-safe, capped.
  std::string title;
  // In reading order, each block at most once.
  std::vector<Part> parts;
  // The title's and the parts' bytes.
  size_t bytes = 0;
  // A topic or a question: a passage matched its words (else the start of
  // the page was given).
  bool matched = false;
  // Text was left out to keep within the budget.
  bool capped = false;
};

// The blocks given to the model for a question so far, one flag per block
// of its document ("tell me more" gives the next ones).
using GivenBlocks = std::vector<bool>;

// |document|'s text for |question| within |budget|. Blocks |given| marks
// are passed over (the headings of the passages picked may be given again,
// as their context), and those picked are marked; |given| may be null.
// |more|: "tell me more" after |question|: the next passages in reading
// order (about, main points), or the next best matches (a topic, a
// question). An excerpt without parts means there is no more to give.
// Linear in the document's text (a partial sort of the matching blocks
// aside).
PageExcerpt SelectPageText(const ReadingDocument& document,
                           const PageQuestion& question,
                           const PageTextBudget& budget,
                           GivenBlocks* given,
                           bool more);

// The page's text for questions: reading's document of |tree|, with
// |skill|'s content root and skips when it has them (null for none).
ReadingDocument QuestionDocument(const PageTree& tree, const SiteSkill* skill);

// ---------------------------------------------------------------------------
// The prompt
// ---------------------------------------------------------------------------

// The answer's form (JSON Schema, for constrained decoding;
// json_schema_grammar.h's subset): {"found": bool, "answer": string of at
// most kMaxPageAnswerChars}.
std::string_view PageAnswerSchema();

struct PageQuestionPrompt {
  PageQuestionPrompt();
  ~PageQuestionPrompt();
  PageQuestionPrompt(const PageQuestionPrompt&);
  PageQuestionPrompt& operator=(const PageQuestionPrompt&);
  PageQuestionPrompt(PageQuestionPrompt&&);
  PageQuestionPrompt& operator=(PageQuestionPrompt&&);

  // Fixed: the rules (PageQuestionInstruction()).
  std::string system_prompt;
  // The page's text inside the untrusted fence, then the question (the
  // runtime may cut a long user message only in its middle, so both fence
  // markers stay).
  std::string user_message;
};

// The fixed instruction the model gets.
std::string_view PageQuestionInstruction();

// The question as it is put to the model ("What does the page say about
// the battery?"), one line, sanitized: the user's words can't open or
// close the fence either.
std::string QuestionForModel(const PageQuestion& question, bool more);

PageQuestionPrompt BuildPageQuestionPrompt(const PageQuestion& question,
                                           const PageExcerpt& excerpt,
                                           bool more);

// ---------------------------------------------------------------------------
// The answer
// ---------------------------------------------------------------------------

struct PageAnswer {
  PageAnswer();
  ~PageAnswer();
  PageAnswer(const PageAnswer&);
  PageAnswer& operator=(const PageAnswer&);
  PageAnswer(PageAnswer&&);
  PageAnswer& operator=(PageAnswer&&);

  enum class Kind : uint8_t {
    kAnswer,     // "The page says: ..."
    kNotOnPage,  // the model found no answer: the browser's own words
    kWithheld,   // it held the wake phrase: not said
    kNoAnswer,   // an empty, malformed or oversized reply
  };
  Kind kind = Kind::kNoAnswer;
  // What the browser says: speech-safe, one line, at most
  // kMaxPageAnswerChars plus the preface.
  std::string text;
};

// The model's reply (|reply|, JSON text the inference service produced
// under PageAnswerSchema(); untrusted) as what the browser says.
PageAnswer PageAnswerFromModel(std::string_view reply,
                               const PageQuestion& question);

// The model's answer text as it may be said: markup and list markers
// dropped, lines joined, SpeechSafeText()'s cleaning, a preface of its own
// ("The page says:") dropped, at most kMaxPageAnswerWords words and
// kMaxPageAnswerChars code points (cut at a sentence's end where one is
// near, else at a word). May be empty.
std::string CleanAnswerText(std::string_view text);

// Whether |text| holds voice mode's wake phrase: a greeting ("hey", "ok",
// ...) followed by the wake word or a word that sounds like it ("Hey
// Molt", "hey mold", "ok moltbrowser"), as the session would hear it said.
bool HoldsWakePhrase(std::string_view text);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_PAGE_QUESTIONS_H_
