// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The browser's prompts and a page's dialogs read aloud and answered by
// voice (voice/voice_prompts.h, voice/voice_gesture.h): a dialog's message
// and a file's name are page text, the site is the page's host, and what
// is heard is whatever the microphone (or a helper someone took over)
// gives. The input is text, so seeds and the dictionary stay readable:
//   byte 0      bits 0-2 the prompt's kind (7: a draft's sites); bit 3 a
//               gesture word is
//               offered; bit 4 a malicious download; bit 5 a reload; bit 6
//               a flood; bit 7 the word was retired once (with bit 3), or
//               the screen is shared (without it)
//   byte 1      (optional, from round 43) the word channel's inputs, bits
//               0-4 (voice_gesture_channel.h), and whether the prompt said
//               why the word is shown (bit 5)
//   line 0      the site
//   line 1      the page's text (a dialog's message, a file's name)
//   lines 2...  what is heard, one utterance per line
//
// Checks: what is said is valid, speech-safe and bounded, starts with the
// site's words (or "This page"), and never holds the page's text past its
// cap; an answer names the gesture word only when one was offered, and a
// "can't allow" only when none was; a dictated answer is bounded and only
// for prompt(); a gesture book minting from the same lines mints only the
// prompt's word, once, and a retired word neither mints nor refuses; what
// is said with the word has a twin that differs only where the word is,
// and the reply's twin never holds it; the channel never shows the word
// while the screen is captured, and says it without the setting only
// privately.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/check.h"
#include "base/check_op.h"
#include "chrome/browser/molt_ai/voice/spoken_text.h"
#include "chrome/browser/molt_ai/voice/voice_gesture.h"
#include "chrome/browser/molt_ai/voice/voice_gesture_channel.h"
#include "chrome/browser/molt_ai/voice/voice_prompts.h"

namespace {

using molt_ai::voice::BrowserPrompt;
using molt_ai::voice::ConfirmationWords;
using molt_ai::voice::GestureAction;
using molt_ai::voice::GestureBinding;
using molt_ai::voice::GestureBook;
using molt_ai::voice::HeardEvidence;
using molt_ai::voice::HearResult;
using molt_ai::voice::kMaxDialogSpokenChars;
using molt_ai::voice::kMaxPromptAnswerBytes;
using molt_ai::voice::MatchConfirmationWord;
using molt_ai::voice::PendingPrompt;
using molt_ai::voice::PromptAnswer;
using molt_ai::voice::PromptKind;
using molt_ai::voice::PromptReply;
using molt_ai::voice::VoiceGesture;

constexpr PromptKind kKinds[] = {
    PromptKind::kPermission,   PromptKind::kAlert,
    PromptKind::kConfirm,      PromptKind::kTextPrompt,
    PromptKind::kBeforeUnload, PromptKind::kDangerousDownload,
    PromptKind::kSavePassword, PromptKind::kConfirmSites,
};

// The browser's framing around the page's text, at most.
constexpr size_t kMaxFramingBytes = 400;

std::vector<std::string_view> Lines(std::string_view text) {
  std::vector<std::string_view> lines;
  while (!text.empty() && lines.size() < 64) {
    const size_t end = text.find('\n');
    lines.push_back(text.substr(0, end));
    if (end == std::string_view::npos) {
      break;
    }
    text.remove_prefix(end + 1);
  }
  return lines;
}

void CheckSaid(const std::string& said) {
  CHECK(molt_ai::voice::IsValidUtf8(said));
  CHECK_LE(said.size(), 4 * kMaxDialogSpokenChars + kMaxFramingBytes);
  for (char c : said) {
    CHECK(static_cast<unsigned char>(c) >= 0x20) << said;
  }
}

// The word's channel for |bits|: never shown while the screen is captured;
// said without the setting only privately, where nothing captures.
void CheckChannel(uint8_t bits) {
  molt_ai::voice::WordChannelInputs inputs;
  inputs.echo_cancelled = bits & 1;
  inputs.bundled_voice = bits & 2;
  inputs.audio_captured = bits & 4;
  inputs.screen_captured = bits & 8;
  inputs.say_aloud_anyway = bits & 16;
  const molt_ai::voice::WordChannel channel =
      molt_ai::voice::DecideWordChannel(inputs);
  CHECK(!(inputs.screen_captured && channel.shown));
  if (channel.said && !inputs.say_aloud_anyway) {
    CHECK(channel.privately);
    CHECK(!inputs.audio_captured && !inputs.screen_captured);
  }
  CHECK(!channel.available || channel.said || channel.shown);
  // What went out through it is exposed by nothing it already allows.
  CHECK(!molt_ai::voice::WordExposed(channel, inputs));
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 1) {
    return 0;
  }
  const uint8_t flags = data[0];
  // A second byte of flags only when the input says so (bit 7 of the
  // first, with a gesture word offered, retires a word), else text: the
  // seeds before round 43 keep their meaning.
  const uint8_t channel_bits = size > 1 && (flags & 0x88) == 0x88 ? data[1] : 0;
  const size_t text_at = size > 1 && (flags & 0x88) == 0x88 ? 2 : 1;
  CheckChannel(channel_bits & 31);
  const std::string_view rest(reinterpret_cast<const char*>(data + text_at),
                              size - text_at);
  const std::vector<std::string_view> lines = Lines(rest);

  BrowserPrompt prompt;
  prompt.kind = kKinds[flags & 7];
  prompt.site = std::string(lines.size() > 0 ? lines[0] : "");
  prompt.text = std::string(lines.size() > 1 ? lines[1] : "");
  prompt.what = prompt.text;
  prompt.default_text = prompt.text;
  prompt.malicious = flags & 16;
  prompt.reload = flags & 32;
  prompt.flood = flags & 64;

  const bool offer = (flags & 8) && molt_ai::voice::NeedsGesture(prompt.kind) &&
                     !prompt.malicious;
  prompt.screen_shared =
      !offer && (flags & 128) && molt_ai::voice::NeedsGesture(prompt.kind);
  prompt.explain_shown_only = channel_bits & 32;
  GestureBook book{[n = uint64_t{size}]() mutable { return n += 0x9E37; }};
  std::optional<molt_ai::voice::GesturePrompt> gesture_prompt;
  std::string retired;
  if (offer) {
    const GestureBinding binding{GestureAction::kAllowPermission, prompt.site};
    gesture_prompt = book.Begin(binding, 0);
    CHECK(gesture_prompt);
    if (flags & 128) {
      // Retired once: the prompt begun again for the same binding.
      retired = gesture_prompt->word;
      gesture_prompt = book.Begin(binding, 0);
      CHECK(gesture_prompt);
      CHECK_NE(gesture_prompt->word, retired);
      CHECK_EQ(gesture_prompt->retired.size(), 1u);
    }
  }
  const std::string word = gesture_prompt ? gesture_prompt->word : "";

  const std::string said = molt_ai::voice::PromptSpeech(prompt, word);
  CheckSaid(said);
  CHECK(!said.empty());

  PendingPrompt pending;
  pending.id = 1;
  pending.kind = prompt.kind;
  pending.word = word;
  pending.malicious = prompt.malicious;
  pending.screen_shared = prompt.screen_shared;
  pending.expires_ms = 1000;

  if (!word.empty()) {
    // The twin differs from what is said only where the word is: the
    // browser's sentence comes after the page's text.
    const std::string twin =
        molt_ai::voice::PromptSpeech(prompt, molt_ai::voice::kWordOnScreen);
    CheckSaid(twin);
    const std::string placeholder =
        std::string(" say ") + molt_ai::voice::kWordOnScreen + ".";
    const size_t at = twin.rfind(placeholder);
    CHECK_NE(at, std::string::npos) << twin;
    std::string back = twin;
    back.replace(at, placeholder.size(), " say " + word + ".");
    CHECK_EQ(back, said);
    molt_ai::voice::NoteWordText(pending, molt_ai::voice::WordText{said, twin});
    if (said != twin) {
      const std::string* found = molt_ai::voice::WordTextTwin(pending, said);
      CHECK(found);
      CHECK_EQ(*found, twin);
    }
  }

  int minted = 0;
  for (size_t i = 2; i < lines.size(); ++i) {
    const PromptReply reply =
        molt_ai::voice::ParsePromptReply(pending, lines[i]);
    switch (reply.answer) {
      case PromptAnswer::kAllow:
      case PromptAnswer::kWrongWord:
      case PromptAnswer::kNeedWord:
      case PromptAnswer::kSayAgain:
        CHECK(!word.empty());
        break;
      case PromptAnswer::kCantAllow:
        CHECK(word.empty());
        break;
      case PromptAnswer::kDictate:
        CHECK(prompt.kind == PromptKind::kTextPrompt);
        CHECK(!reply.text.empty());
        CHECK_LE(reply.text.size(), kMaxPromptAnswerBytes);
        break;
      case PromptAnswer::kNone:
      case PromptAnswer::kRefuse:
      case PromptAnswer::kAccept:
      case PromptAnswer::kDecline:
        break;
    }
    if (reply.answer == PromptAnswer::kAllow) {
      CHECK_EQ(ConfirmationWords()[*MatchConfirmationWord(lines[i])], word);
    }
    CheckSaid(molt_ai::voice::ReplySpeech(pending, reply));
    // The reply's twin: the browser's own words, never the word.
    const std::string reply_twin =
        molt_ai::voice::ReplySpeechWithoutWord(pending, reply);
    CheckSaid(reply_twin);
    if (!word.empty() && reply.answer != PromptAnswer::kDictate) {
      CHECK_EQ(reply_twin.find(" " + word + "."), std::string::npos)
          << reply_twin;
    }

    if (gesture_prompt) {
      VoiceGesture gesture;
      const HearResult result =
          book.Hear(gesture_prompt->id, lines[i], HeardEvidence{true, true},
                    static_cast<int64_t>(i), &gesture);
      if (!retired.empty() &&
          MatchConfirmationWord(lines[i]) == MatchConfirmationWord(retired) &&
          result != HearResult::kNotAWord && result != HearResult::kUnsure &&
          result != HearResult::kNoPrompt && result != HearResult::kLockedOut) {
        // The old word, for sure: neither the word nor a miss.
        CHECK_EQ(result, HearResult::kRetired);
      }
      if (result == HearResult::kMinted) {
        ++minted;
        CHECK_EQ(reply.answer, PromptAnswer::kAllow);
        CHECK(book.Consume(gesture, gesture.binding, static_cast<int64_t>(i)));
        CHECK(!book.Consume(gesture, gesture.binding, static_cast<int64_t>(i)));
      }
    }
  }
  CHECK_LE(minted, 1);
  return 0;
}
