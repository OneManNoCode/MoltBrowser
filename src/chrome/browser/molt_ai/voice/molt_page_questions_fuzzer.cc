// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Questions about the page (voice/page_questions.h) over hostile pages and
// hostile model replies: what a page, through a renderer that may be
// compromised, puts in its accessibility tree (read as Blink's tree from
// HTML by the test converter, html_fixture_tree.h), the question as a
// recognizer wrote it, and what a model that read the page answers.
//
// Format: [flags][question length][question][reply length (2 bytes)]
//         [reply][html]
//   flags (1 byte): bits 0-1 the question's kind (about, main points,
//     topic, question), bits 2-3 "tell me more" rounds after it (0-3),
//     bits 4-5 the budget (the default, a small model's window, a tiny
//     one, the smallest window), bit 6 the reply is wrapped as the
//     schema's answer ({"found":true,"answer":<reply>}), bit 7 "found"
//     is false then.
//
// Checks: everything returns (the work is linear, and capped) and is the
// same twice; the excerpt stays within its budget, in reading order, each
// block once (across "tell me more" rounds too), speech-safe; the prompt
// has exactly one fence, the page's text inside it and the question alone
// after it; the answer said is speech-safe, one line and capped, never
// holds the wake phrase, and is either "The page says: ..." or one of the
// browser's own sentences.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "base/check.h"
#include "base/check_op.h"
#include "chrome/browser/molt_ai/security/agent_safety.h"
#include "chrome/browser/molt_ai/voice/html_fixture_tree.h"
#include "chrome/browser/molt_ai/voice/page_map.h"
#include "chrome/browser/molt_ai/voice/page_map_text.h"
#include "chrome/browser/molt_ai/voice/page_questions.h"
#include "chrome/browser/molt_ai/voice/page_reading.h"
#include "chrome/browser/molt_ai/voice/spoken_text.h"

namespace {

using molt_ai::voice::CleanAnswerText;
using molt_ai::voice::CountCodePoints;
using molt_ai::voice::GivenBlocks;
using molt_ai::voice::HoldsWakePhrase;
using molt_ai::voice::kMaxPageAnswerChars;
using molt_ai::voice::kMaxPageAnswerWords;
using molt_ai::voice::kMaxPageQuestionBytes;
using molt_ai::voice::MakePageQuestion;
using molt_ai::voice::PageAnswer;
using molt_ai::voice::PageAnswerFromModel;
using molt_ai::voice::PageExcerpt;
using molt_ai::voice::PageQuestion;
using molt_ai::voice::PageQuestionKind;
using molt_ai::voice::PageQuestionPrompt;
using molt_ai::voice::PageTextBudget;
using molt_ai::voice::PageTextBudgetFor;
using molt_ai::voice::PageTree;
using molt_ai::voice::QuestionDocument;
using molt_ai::voice::ReadingDocument;
using molt_ai::voice::SpeechSafeText;

constexpr char kPreface[] = "The page says: ";

size_t Count(std::string_view haystack, std::string_view needle) {
  size_t count = 0;
  for (size_t at = haystack.find(needle); at != std::string_view::npos;
       at = haystack.find(needle, at + 1)) {
    ++count;
  }
  return count;
}

void CheckSpeakable(std::string_view s, size_t cap) {
  CHECK_LE(CountCodePoints(s), cap);
  CHECK_EQ(SpeechSafeText(s, cap), std::string(s));
}

// |s| as a JSON string literal (quotes included).
std::string Quote(std::string_view s) {
  std::string out = "\"";
  for (char c : s) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(c);
    } else if (u < 0x20) {
      static constexpr char kHex[] = "0123456789abcdef";
      out += "\\u00";
      out.push_back(kHex[u >> 4]);
      out.push_back(kHex[u & 15]);
    } else {
      out.push_back(c);
    }
  }
  out.push_back('"');
  return out;
}

PageTextBudget BudgetFor(uint8_t which) {
  switch (which & 3) {
    case 1:
      return PageTextBudgetFor(2048);
    case 2: {
      PageTextBudget tiny;
      tiny.max_bytes = 300;
      tiny.max_blocks = 4;
      tiny.max_block_chars = 120;
      return tiny;
    }
    case 3:
      return PageTextBudgetFor(1);
    default:
      return PageTextBudget();
  }
}

void CheckExcerpt(const PageExcerpt& excerpt,
                  const PageTextBudget& budget,
                  const ReadingDocument& document,
                  GivenBlocks* seen) {
  CHECK_LE(excerpt.bytes, budget.max_bytes);
  CHECK_LE(excerpt.parts.size(), budget.max_blocks);
  size_t bytes = excerpt.title.size();
  CheckSpeakable(excerpt.title, 300);
  for (size_t i = 0; i < excerpt.parts.size(); ++i) {
    const PageExcerpt::Part& part = excerpt.parts[i];
    CHECK_LT(part.block, document.blocks.size());
    if (i > 0) {
      CHECK_GT(part.block, excerpt.parts[i - 1].block);
    }
    CHECK_EQ(part.kind, document.blocks[part.block].kind);
    CHECK(!part.text.empty());
    CheckSpeakable(part.text, budget.max_block_chars);
    bytes += part.text.size() + 1;
    // A passage is given once, across rounds; a heading may come again as
    // its section's context.
    if (part.kind != molt_ai::voice::ReadingBlockKind::kHeading) {
      CHECK(!(*seen)[part.block]);
      (*seen)[part.block] = true;
    }
  }
  CHECK_EQ(bytes, excerpt.bytes);
}

void CheckPrompt(const PageQuestionPrompt& prompt) {
  CHECK_EQ(prompt.system_prompt,
           std::string(molt_ai::voice::PageQuestionInstruction()));
  const std::string& u = prompt.user_message;
  CHECK_EQ(Count(u, molt_ai::agent_safety::kUntrustedBegin), 1u);
  CHECK_EQ(Count(u, molt_ai::agent_safety::kUntrustedEnd), 1u);
  CHECK_EQ(Count(u, "<<<"), 2u);
  CHECK_EQ(Count(u, ">>>"), 2u);
  CHECK_EQ(Count(u, "<|"), 0u);
  CHECK_EQ(Count(u, "|>"), 0u);
  const size_t begin = u.find(molt_ai::agent_safety::kUntrustedBegin);
  const size_t end = u.find(molt_ai::agent_safety::kUntrustedEnd);
  CHECK_LT(begin, end);
  // After the fence: the question, on one line.
  const std::string_view after = std::string_view(u).substr(
      end + std::string_view(molt_ai::agent_safety::kUntrustedEnd).size());
  CHECK(after.starts_with("\n\nQuestion: "));
  CHECK_EQ(after.substr(2).find('\n'), std::string_view::npos);
}

void CheckAnswer(const PageAnswer& answer) {
  CHECK(!answer.text.empty());
  CheckSpeakable(answer.text,
                 kMaxPageAnswerChars + 1 + std::string_view(kPreface).size());
  CHECK_EQ(answer.text.find('\n'), std::string::npos);
  if (answer.kind == PageAnswer::Kind::kAnswer) {
    CHECK(answer.text.starts_with(kPreface));
    CHECK(!HoldsWakePhrase(answer.text));
  } else {
    // The browser's own words: never the model's.
    CHECK(!answer.text.starts_with(kPreface));
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::string_view in(reinterpret_cast<const char*>(data), size);
  if (in.size() < 4) {
    return 0;
  }
  const uint8_t flags = static_cast<uint8_t>(in[0]);
  const size_t question_size = static_cast<uint8_t>(in[1]);
  in.remove_prefix(2);
  const std::string_view question_text = in.substr(0, question_size);
  in.remove_prefix(question_text.size());
  if (in.size() < 2) {
    return 0;
  }
  const size_t reply_size =
      static_cast<uint8_t>(in[0]) |
      (static_cast<size_t>(static_cast<uint8_t>(in[1])) << 8);
  in.remove_prefix(2);
  const std::string_view reply = in.substr(0, reply_size);
  in.remove_prefix(reply.size());
  const std::string_view html = in;

  const PageQuestionKind kind = static_cast<PageQuestionKind>(flags & 3);
  const PageQuestion question = MakePageQuestion(kind, question_text);
  CHECK_EQ(question.kind, kind);
  CHECK_LE(question.text.size(), kMaxPageQuestionBytes);
  CheckSpeakable(question.text, kMaxPageQuestionBytes);

  // The page.
  const PageTree tree = molt_ai::voice::test::PageTreeFromHtml(html);
  const ReadingDocument document = QuestionDocument(tree, nullptr);
  const PageTextBudget budget = BudgetFor(static_cast<uint8_t>(flags >> 4));
  GivenBlocks given;
  GivenBlocks seen(document.blocks.size(), false);
  const int rounds = 1 + ((flags >> 2) & 3);
  for (int round = 0; round < rounds; ++round) {
    const bool more = round > 0;
    GivenBlocks again = given;
    const PageExcerpt excerpt =
        SelectPageText(document, question, budget, &given, more);
    // The same twice.
    const PageExcerpt twice =
        SelectPageText(document, question, budget, &again, more);
    CHECK_EQ(excerpt.parts.size(), twice.parts.size());
    CHECK_EQ(excerpt.bytes, twice.bytes);
    CHECK(given == again);
    CheckExcerpt(excerpt, budget, document, &seen);
    CHECK_EQ(given.size(), document.blocks.size());
    for (const PageExcerpt::Part& part : excerpt.parts) {
      CHECK(given[part.block]);
    }
    CheckPrompt(BuildPageQuestionPrompt(question, excerpt, more));
    if (excerpt.parts.empty()) {
      break;
    }
  }

  // The model's reply.
  std::string wrapped(reply);
  if (flags & 0x40) {
    wrapped = std::string("{\"found\":") + ((flags & 0x80) ? "false" : "true") +
              ",\"answer\":" + Quote(reply) + "}";
  }
  const PageAnswer answer = PageAnswerFromModel(wrapped, question);
  CheckAnswer(answer);
  // A well-formed reply is said, unless it holds the wake phrase or nothing
  // is left of it once cleaned.
  const bool well_formed =
      molt_ai::voice::IsValidUtf8(reply) &&
      wrapped.size() <= molt_ai::voice::kMaxModelReplyBytes;
  if ((flags & 0xC0) == 0x40 && well_formed && !HoldsWakePhrase(reply)) {
    const std::string clean = CleanAnswerText(reply);
    if (!clean.empty() && !HoldsWakePhrase(clean)) {
      CHECK(answer.kind == PageAnswer::Kind::kAnswer);
    }
  }
  if ((flags & 0xC0) == 0xC0 && well_formed) {
    CHECK(answer.kind == PageAnswer::Kind::kNotOnPage);
  }
  // The cleaning alone.
  const std::string clean = CleanAnswerText(reply);
  CheckSpeakable(clean, kMaxPageAnswerChars + 1);
  CHECK_LE(Count(clean, " ") + (clean.empty() ? 0 : 1), kMaxPageAnswerWords);
  return 0;
}
