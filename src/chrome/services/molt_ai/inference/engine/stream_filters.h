// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Streaming output filters applied to every decoded token piece before it
// leaves the inference service, in this order:
//
//   token piece -> HarmonyChannelFilter -> GemmaThoughtFilter (Gemma 4
//   vocabularies only) -> ThinkTagFilter -> StopSequenceFilter ->
//   Utf8Assembler -> ReplaceInvalidUtf8
//
// - HarmonyChannelFilter: gpt-oss emits its private reasoning in an
//   "analysis" channel and the answer in a "final" channel, framed by
//   <|channel|>/<|message|>/<|start|>/<|end|>/... control tokens. Only the
//   final channel is surfaced. Models that never emit those markers pass
//   through untouched.
// - GemmaThoughtFilter: Gemma 4 writes its thoughts as
//   "<|channel>thought\n...<channel|>" before (or between parts of) the
//   answer. Every such block is hidden, wherever it appears.
// - ThinkTagFilter: Qwen3 (and other reasoning models) wrap their reasoning
//   in <think>...</think>. The block is hidden even when the tags are split
//   across tokens ("<th" + "ink>"), and the whitespace that follows the
//   closing tag is trimmed.
// - StopSequenceFilter: caller-supplied stop strings, matched on the visible
//   text (after the reasoning filters). The stop string and everything after
//   it are dropped. The browser runs the same filter again over what it
//   receives from the service (chrome/browser/molt_ai/runtime/
//   stream_validator.h); that copy is the one callers see.
// - Utf8Assembler: a token can end in the middle of a multi-byte UTF-8
//   character (e.g. half an emoji); bytes are held back until the
//   character is complete.
// - ReplaceInvalidUtf8: byte-level tokens can still form invalid UTF-8; the
//   service replaces it with U+FFFD, because the browser treats invalid
//   UTF-8 from the service as a protocol violation (IsValidUtf8).
//
// Std-only so they are unit-tested standalone (stream_filters_unittest.cc).
// The browser links this file for StopSequenceFilter and IsValidUtf8 only.

#ifndef CHROME_SERVICES_MOLT_AI_INFERENCE_ENGINE_STREAM_FILTERS_H_
#define CHROME_SERVICES_MOLT_AI_INFERENCE_ENGINE_STREAM_FILTERS_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace molt_ai {

class HarmonyChannelFilter {
 public:
  // Returns the visible part of one decoded token piece.
  std::string Feed(const std::string& piece);

  // True once a Harmony marker has been seen.
  bool active() const { return harmony_; }
  // True while output is being suppressed (channel header, analysis /
  // commentary body, role names between messages).
  bool hiding() const { return harmony_ && !(state_ == kBody && emit_); }

  // Control-token text that ends a hidden message and opens the final
  // channel; used to force an answer when the reasoning budget runs out.
  static constexpr char kForceFinal[] =
      "<|end|><|start|>assistant<|channel|>final<|message|>";

 private:
  enum State { kOutside, kChannelName, kBody };
  bool harmony_ = false;
  State state_ = kOutside;
  std::string channel_;
  bool emit_ = false;
};

// Hides Gemma 4's thought channel. The markers, as in the vendored
// llama.cpp's built-in "gemma4" chat format (src/llama-chat.cpp) and the
// Jinja template Gemma 4 GGUFs ship (llama.cpp b11641
// models/templates/google-gemma-4-31B-it.jinja), are
//   <|channel>  (token 100)  opens a block: "<|channel>thought\n..."
//   <channel|>  (token 101)  closes it
// Both are USER_DEFINED tokens in the Gemma 4 vocab
// (models/ggml-vocab-gemma-4.gguf), so llama_token_to_piece() renders them
// even with special=false and they arrive here as text, normally as whole
// pieces. They are still matched across piece boundaries ("<|chan" +
// "nel>"), in case a model spells one out of ordinary tokens.
//
// Rules (fail closed: when in doubt, hide):
//  * Everything from "<|channel>" to its matching "<channel|>" is hidden,
//    the channel name ("thought") included, anywhere in the stream. The
//    answer never starts with whitespace; whitespace around a block in the
//    middle of the answer is kept (as the template's strip_thinking()
//    does).
//  * Nested blocks count: "<|channel>a<|channel>b<channel|>c<channel|>"
//    hides a, b and c. Reasoning that quotes the markers in pairs can't end
//    the block early; an extra unpaired "<|channel>" keeps hiding until
//    ForceClose() (the runtime's reasoning budget).
//  * An unterminated block hides the rest of the stream (Flush() drops it).
//  * A "<channel|>" with no open block is dropped (never shown).
//  * Look-alikes are text: Harmony's "<|channel|>", "<channel>", "<|chan".
// Linear time: only a possible marker prefix (< 10 bytes) is held back.
class GemmaThoughtFilter {
 public:
  // |start_inside|: the prompt ends inside an open thought block.
  explicit GemmaThoughtFilter(bool start_inside = false);

  // Feed a piece; returns the text that can be shown now.
  std::string Feed(std::string_view piece);
  // End of stream: releases held-back bytes that turned out not to be a
  // marker (nothing is released while inside a block).
  std::string Flush();
  // Closes every open block at once: the runtime fed kForceClose to the
  // model because the reasoning budget ran out, and the answer follows.
  void ForceClose();

  // True while inside a thought block.
  bool hiding() const { return depth_ > 0; }

  // True if |prompt_tail| (the end of a rendered prompt) leaves a thought
  // block open, by the same rules as Feed().
  static bool EndsInsideThought(std::string_view prompt_tail);

  static constexpr char kOpenMarker[] = "<|channel>";
  static constexpr char kCloseMarker[] = "<channel|>";
  // Text that closes an open block (as Gemma 4's template writes it);
  // used to force an answer when the reasoning budget runs out.
  static constexpr char kForceClose[] = "\n<channel|>";

 private:
  std::string EmitVisible(std::string_view text);

  size_t depth_;
  // Swallow whitespace until the answer's first visible character.
  bool strip_leading_ws_ = true;
  std::string pending_;
};

class ThinkTagFilter {
 public:
  // |start_inside|: the prompt already opened a <think> block (some chat
  // templates pre-fill it), so output starts hidden.
  explicit ThinkTagFilter(bool start_inside = false);

  // Feed a piece; returns the text that can be shown now. Bytes that might
  // be the start of a tag are held back until disambiguated.
  std::string Feed(std::string_view piece);
  // End of stream: releases held-back bytes that turned out not to be a
  // tag (nothing is released while inside a think block).
  std::string Flush();

  bool in_think() const { return in_think_; }
  // True once any think block was opened.
  bool saw_think() const { return saw_think_; }

  static constexpr char kOpenTag[] = "<think>";
  static constexpr char kCloseTag[] = "</think>";
  // Text that closes an open think block; used to force an answer when
  // the reasoning budget runs out.
  static constexpr char kForceClose[] = "\n</think>\n\n";

 private:
  std::string EmitVisible(std::string_view text);

  bool in_think_;
  bool saw_think_;
  // Swallow whitespace before the answer starts (at the very beginning and
  // after </think>); the answer never begins with blank lines.
  bool strip_leading_ws_ = true;
  // Set once non-whitespace answer text has been emitted. Reasoning tags
  // are only honored before that point, so prose or code that merely
  // mentions "<think>" later in an answer is shown verbatim.
  bool visible_started_ = false;
  std::string pending_;
};

// Stops the visible stream at the first of a set of stop strings. Matching is
// on bytes, with one KMP automaton per stop string, so it is linear in the
// input whatever the stop strings are, and independent of how the text is
// split into pieces. The filter holds back at most (longest stop - 1) bytes:
// a suffix that could still become a stop string.
//
// When a stop string completes, the text before its first byte is returned,
// stopped() becomes true and every later Feed()/Flush() returns "". If two
// stop strings overlap, the one that completes first wins. Empty stop strings
// are ignored. A held-back suffix is a prefix of a stop string; when the stop
// strings are valid UTF-8 they start with a lead byte, so on valid UTF-8
// input the filter never splits a character.
class StopSequenceFilter {
 public:
  explicit StopSequenceFilter(std::vector<std::string> stop_sequences = {});
  ~StopSequenceFilter();
  StopSequenceFilter(StopSequenceFilter&&);
  StopSequenceFilter& operator=(StopSequenceFilter&&);

  // Feed visible text; returns the part that can be shown now.
  std::string Feed(std::string_view text);
  // End of stream: releases the held-back suffix (no stop string can
  // complete any more). Nothing after a stop.
  std::string Flush();

  bool stopped() const { return stopped_; }
  bool empty() const { return stops_.empty(); }
  // Bytes currently held back.
  size_t held_back() const { return pending_.size(); }

 private:
  struct Stop {
    std::string text;
    std::vector<size_t> failure;  // KMP failure function
    size_t matched = 0;           // length of the current partial match
  };
  std::vector<Stop> stops_;
  std::string pending_;
  bool stopped_ = false;
};

class Utf8Assembler {
 public:
  // Appends |bytes|; returns the longest prefix made of complete UTF-8
  // characters (holding back an incomplete trailing sequence).
  std::string Push(std::string_view bytes);
  // Releases whatever is held back (even if incomplete).
  std::string Flush();

 private:
  std::string pending_;
};

// Length of the longest prefix of |s| that does not end inside a
// multi-byte UTF-8 character.
size_t Utf8SafePrefixLength(std::string_view s);

// Strict UTF-8 (RFC 3629): no overlong forms, no surrogates (U+D800..DFFF),
// nothing above U+10FFFF, no truncated sequence at the end. NUL is valid.
bool IsValidUtf8(std::string_view s);

// |s| with every ill-formed subsequence replaced by U+FFFD, one replacement
// per maximal subpart (the Unicode / WHATWG convention). Valid input is
// returned unchanged; the result always passes IsValidUtf8().
std::string ReplaceInvalidUtf8(std::string_view s);

}  // namespace molt_ai

#endif  // CHROME_SERVICES_MOLT_AI_INFERENCE_ENGINE_STREAM_FILTERS_H_
