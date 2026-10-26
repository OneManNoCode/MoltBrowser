// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The Translator API (Translator.availability() / create() / translate() /
// translateStreaming()) on the user's own on-device model: which languages
// it serves, how a page's text is cut into pieces the model can take, the
// browser-authored instruction for each piece, and the cleanup of what the
// model returns. Standard library only, tested in molt_ai_unittests.
// Design: docs/design/WEB_AI_APIS.md, section 10.
//
// Fingerprinting: the language table is fixed, the same for every user and
// model. Whether a pair is supported depends only on the two tags the page
// passed, never on the installed models, the OS language or Accept-Language.
// Pieces and budgets come from EstimateTokens(), never the model's
// tokenizer.
//
// Trust: the instruction (TaskPrompt::system_prompt) is built only from the
// table's English names; the page's text goes only into user_message, which
// the inference service treats as untrusted.

#ifndef CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_TRANSLATION_H_
#define CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_TRANSLATION_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/web_ai/web_ai_policy.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_prompts.h"
#include "chrome/services/molt_ai/inference/engine/raw_ptr_exclusion.h"

namespace molt_ai::web_ai {

// ---------------------------------------------------------------------------
// Languages
// ---------------------------------------------------------------------------

struct TranslatorLanguage {
  // What the page gets back as Translator.sourceLanguage/targetLanguage.
  const char* code;
  // The English name in the model's instruction.
  const char* name;
};

// The languages the Translator API serves: widely used languages that the
// multilingual models in MoltBrowser's catalog translate reasonably well. A
// general model could attempt any pair, but quality falls off quickly for
// less common languages, and a list that depended on the user's model would
// tell sites which model they have. Sorted by code. Adding a language is a
// change here and in docs/design/WEB_AI_APIS.md (section 10.2).
inline constexpr TranslatorLanguage kTranslatorLanguages[] = {
    {"ar", "Arabic"},
    {"de", "German"},
    {"en", "English"},
    {"es", "Spanish"},
    {"fr", "French"},
    {"hi", "Hindi"},
    {"id", "Indonesian"},
    {"it", "Italian"},
    {"ja", "Japanese"},
    {"ko", "Korean"},
    {"nl", "Dutch"},
    {"pl", "Polish"},
    {"pt", "Portuguese"},
    {"ru", "Russian"},
    {"th", "Thai"},
    {"tr", "Turkish"},
    {"uk", "Ukrainian"},
    {"vi", "Vietnamese"},
    {"zh", "Simplified Chinese"},
    {"zh-Hant", "Traditional Chinese"},
};

// The table entry a BCP 47 tag (as Blink canonicalizes it) falls under, or
// nullptr. Best fit by language: "en-US" and "en-GB" are "en", "pt-BR" is
// "pt". Chinese goes by script, then region: "zh-Hant", "zh-TW", "zh-HK"
// and "zh-MO" are "zh-Hant"; "zh", "zh-Hans", "zh-CN" and "zh-SG" are "zh".
// Extensions and private use ("-u-...", "-x-...") are ignored. A malformed
// tag (IsValidLanguageTag()) has no entry. Linear in the tag's length.
const TranslatorLanguage* FindTranslatorLanguage(std::string_view tag);

struct TranslatorPair {
  MOLT_RAW_PTR_EXCLUSION const TranslatorLanguage* source = nullptr;
  MOLT_RAW_PTR_EXCLUSION const TranslatorLanguage* target = nullptr;
};

// Both tags have an entry, and different ones (English to English isn't a
// translation, so it is "unavailable" like any unsupported pair).
std::optional<TranslatorPair> ResolveTranslatorPair(std::string_view source,
                                                    std::string_view target);

// ---------------------------------------------------------------------------
// Sizes
// ---------------------------------------------------------------------------

// What one translate() call may hold, in EstimateTokens() tokens: the
// writing APIs' fixed quota, the same for every user. (Blink reports
// inputQuota as Infinity and measureInputUsage() as 0 for Translator; both
// are renderer constants.) More fails with QuotaExceededError.
inline constexpr uint32_t kTranslatorInputQuota = kWritingAssistanceInputQuota;

// The largest piece sent to the model in one generation (about 1,100
// characters of English). With its reply budget, the instruction and the
// runtime's reasoning reserve it fits a 2048-token context window, the
// smallest the runtime uses, so the runtime never has to cut a piece.
inline constexpr uint32_t kTranslatorPieceTokens = 384;

// A piece's reply budget: twice its estimated tokens plus 64 (the JSON
// wrapper, escapes, and scripts that tokenize less densely than the
// source), at most kTranslatorMaxReplyTokens.
inline constexpr uint32_t kTranslatorMaxReplyTokens =
    2 * kTranslatorPieceTokens + 64;
uint32_t TranslatorMaxOutputTokens(uint32_t input_tokens);

// Low temperature: a translation has one right answer, not many.
inline constexpr float kTranslatorTemperature = 0.2f;

// ---------------------------------------------------------------------------
// Pieces
// ---------------------------------------------------------------------------

struct TranslationPiece {
  std::string text;
  // Sent to the model. Otherwise it goes back to the page as it is:
  // whitespace between pieces, or text with nothing to translate (no
  // letters: digits, punctuation, ASCII symbols).
  bool translate = false;
};

// Cuts |text| (valid UTF-8, as ToDisplayText() leaves it) into pieces of at
// most |max_tokens| estimated tokens each (|max_tokens| >= 2), so a long
// input is translated piece by piece within the model's context window.
// Cuts go at the coarsest boundary that fits: line ends, then sentence ends
// (". ", "! ", "? ", and the CJK, Arabic and Devanagari full stops), then
// spaces, then characters (never inside one). Consecutive lines share a
// piece while they fit. Each piece's leading and trailing whitespace is
// kept apart and returned as it is, so the pieces' texts concatenate back
// to |text| exactly. Adjacent pieces that aren't translated are merged.
// Linear in |text|; no regular expressions.
std::vector<TranslationPiece> SplitForTranslation(std::string_view text,
                                                  uint32_t max_tokens);

// The text of the pieces from |*next| that aren't translated, up to the next
// one that is (or the end); advances |*next| past them.
std::string TakeUntranslated(const std::vector<TranslationPiece>& pieces,
                             size_t* next);

// ---------------------------------------------------------------------------
// The model's instruction and its reply
// ---------------------------------------------------------------------------

// The JSON Schema every piece's reply is decoded under (constrained
// decoding, PromptOptions::json_schema): one object with one string, the
// translation. Within the subset json_schema_grammar.h supports.
inline constexpr char kTranslatorResponseSchema[] =
    "{\"type\":\"object\","
    "\"properties\":{\"translation\":{\"type\":\"string\"}},"
    "\"required\":[\"translation\"],"
    "\"additionalProperties\":false}";
// The member of the reply that holds the translation.
inline constexpr char kTranslatorResponseKey[] = "translation";

// The generation for one piece: MoltBrowser's fixed instruction naming the
// two languages, and |piece| (page text) as the user turn.
TaskPrompt BuildTranslatorPrompt(const TranslatorPair& pair,
                                 std::string_view piece);

// The translation the page gets for |source| (the piece that was sent),
// from the model's |translation| (the string in its reply, valid UTF-8):
// surrounding whitespace removed (the piece's own whitespace is kept apart
// by SplitForTranslation()), and quotation marks the model wrapped around a
// piece that had none removed. nullopt if nothing is left: an empty
// translation of a piece with letters is a failure, not an answer.
std::optional<std::string> CleanTranslation(std::string_view source,
                                            std::string_view translation);

// ---------------------------------------------------------------------------
// Several texts in one generation (page translation)
// ---------------------------------------------------------------------------
//
// Translating a whole page (chrome/browser/molt_ai/translate/,
// docs/design/PAGE_TRANSLATION.md) sends many short texts: one generation
// each would repeat the instruction for every link and heading. A batch
// puts several texts, as a JSON array, in one user turn under a fixed
// instruction like BuildTranslatorPrompt()'s, decoded under
// TranslatorBatchResponseSchema(n): exactly n strings, so the model can
// only answer with one translation per text, in order. Each text is a
// translatable piece from SplitForTranslation(), and each translation is
// checked with CleanTranslation(), as for the Translator API.

// Texts per batch. With kTranslatorPieceTokens as the batch's budget
// (counted with TranslatorBatchTextTokens()) a batch fits the same window
// as one piece.
inline constexpr size_t kTranslatorMaxBatchTexts = 16;

// What |text| adds to a batch's user turn, in EstimateTokens() tokens: the
// text JSON-quoted (escapes included) and its line.
uint32_t TranslatorBatchTextTokens(std::string_view text);

// The JSON Schema a batch of |count| texts (1..kTranslatorMaxBatchTexts)
// is decoded under: {"translations": [string x count]}. Within the subset
// json_schema_grammar.h supports.
std::string TranslatorBatchResponseSchema(size_t count);
// The member of a batch reply that holds the translations.
inline constexpr char kTranslatorBatchResponseKey[] = "translations";

// The generation for |texts| (page text, 1..kTranslatorMaxBatchTexts of
// them): MoltBrowser's fixed instruction naming the two languages, and the
// texts as a JSON array, one per line, as the user turn. The reply budget
// is TranslatorMaxOutputTokens() of their TranslatorBatchTextTokens().
TaskPrompt BuildTranslatorBatchPrompt(const TranslatorPair& pair,
                                      const std::vector<std::string>& texts);

}  // namespace molt_ai::web_ai

#endif  // CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_TRANSLATION_H_
