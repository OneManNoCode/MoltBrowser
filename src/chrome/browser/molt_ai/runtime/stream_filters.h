// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Streaming output filters applied to every decoded token piece before it
// reaches the UI, in this order:
//
//   token piece -> HarmonyChannelFilter -> ThinkTagFilter -> Utf8Assembler
//
// - HarmonyChannelFilter: gpt-oss emits its private reasoning in an
//   "analysis" channel and the answer in a "final" channel, framed by
//   <|channel|>/<|message|>/<|start|>/<|end|>/... control tokens. Only the
//   final channel is surfaced. Models that never emit those markers pass
//   through untouched.
// - ThinkTagFilter: Qwen3 (and other reasoning models) wrap their reasoning
//   in <think>...</think>. The block is hidden even when the tags are split
//   across tokens ("<th" + "ink>"), and the whitespace that follows the
//   closing tag is trimmed.
// - Utf8Assembler: a token can end in the middle of a multi-byte UTF-8
//   character (e.g. half an emoji); bytes are held back until the
//   character is complete.
//
// Std-only so they are unit-tested standalone (stream_filters_unittest.cc).

#ifndef CHROME_BROWSER_MOLT_AI_RUNTIME_STREAM_FILTERS_H_
#define CHROME_BROWSER_MOLT_AI_RUNTIME_STREAM_FILTERS_H_

#include <cstddef>
#include <string>
#include <string_view>

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

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_RUNTIME_STREAM_FILTERS_H_
