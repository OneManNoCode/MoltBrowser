// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The direct-command grammar (voice/voice_command.h) on any bytes: what a
// speech recognizer writes, and so whatever anyone near the microphone, or
// a page's audio that gets past echo cancellation, makes it write. The
// first byte picks the context (a yes/no question asked, a list read, a
// text field focused, a custom wake phrase); the rest is the utterance.
//
// Checks: the same command every time; over kMaxUtteranceBytes or invalid
// UTF-8 is refused (and nothing else is); |heard| and |text| are valid
// UTF-8 without control, bidi or invisible characters, single-spaced and
// trimmed, within their caps; |text| is set exactly for the commands that
// carry free text; a site is host-like text (letters, digits and . - _ /,
// no mark at either end or twice in a row) within kMaxSiteBytes; "yes" and
// "no" only when a yes/no question was asked, the dictation edits only in
// a text field; levels, seconds, counts and choice references are within
// their ranges, and a choice always says what it chooses.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "base/check.h"
#include "base/check_op.h"
#include "chrome/browser/molt_ai/voice/spoken_choice.h"
#include "chrome/browser/molt_ai/voice/spoken_numbers.h"
#include "chrome/browser/molt_ai/voice/spoken_text.h"
#include "chrome/browser/molt_ai/voice/voice_command.h"

namespace {

using molt_ai::voice::ChoiceRef;
using molt_ai::voice::IsValidUtf8;
using molt_ai::voice::kMaxChoiceRefWords;
using molt_ai::voice::kMaxCommandTextBytes;
using molt_ai::voice::kMaxSiteBytes;
using molt_ai::voice::kMaxSpokenSeconds;
using molt_ai::voice::kMaxUtteranceBytes;
using molt_ai::voice::ParseContext;
using molt_ai::voice::ParseVoiceCommand;
using molt_ai::voice::RefusalReason;
using molt_ai::voice::VoiceCommand;
using K = molt_ai::voice::VoiceCommandKind;

// Decodes |text| (valid UTF-8) and checks no code point is a control,
// bidi or invisible character, decoded here independently of
// spoken_text.cc.
void CheckClean(std::string_view text, size_t cap) {
  CHECK_LE(text.size(), cap);
  CHECK(IsValidUtf8(text));
  if (text.empty())
    return;
  CHECK_NE(text.front(), ' ');
  CHECK_NE(text.back(), ' ');
  CHECK_EQ(text.find("  "), std::string_view::npos);
  size_t i = 0;
  while (i < text.size()) {
    const auto b0 = static_cast<uint8_t>(text[i]);
    uint32_t cp = 0;
    size_t length = 1;
    if (b0 < 0x80) {
      cp = b0;
    } else if (b0 < 0xE0) {
      cp = b0 & 0x1F;
      length = 2;
    } else if (b0 < 0xF0) {
      cp = b0 & 0x0F;
      length = 3;
    } else {
      cp = b0 & 0x07;
      length = 4;
    }
    for (size_t k = 1; k < length; ++k) {
      cp = cp << 6 | (static_cast<uint8_t>(text[i + k]) & 0x3F);
    }
    i += length;
    CHECK(cp >= 0x20 && cp != 0x7F);
    CHECK(cp < 0x80 || cp > 0x9F);
    CHECK(cp != 0x200B && cp != 0x200C && cp != 0x200D && cp != 0x200E &&
          cp != 0x200F && cp != 0xFEFF && cp != 0xAD);
    CHECK(cp < 0x202A || cp > 0x202E);
    CHECK(cp < 0x2066 || cp > 0x2069);
    CHECK(cp < 0xE0000 || cp > 0xE007F);
    CHECK(cp != '\t' && cp != '\n' && cp != '\r');
  }
}

bool IsMark(char c) {
  return c == '.' || c == '/' || c == '-' || c == '_';
}

void CheckSite(std::string_view site) {
  CHECK_LE(site.size(), kMaxSiteBytes);
  if (site.empty())
    return;
  CHECK(IsValidUtf8(site));
  CHECK(!IsMark(site.front()));
  CHECK(!IsMark(site.back()));
  for (size_t i = 0; i < site.size(); ++i) {
    const auto c = static_cast<uint8_t>(site[i]);
    if (c < 0x80) {
      CHECK((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            IsMark(static_cast<char>(c)));
    }
    if (i > 0)
      CHECK(!(IsMark(site[i]) && IsMark(site[i - 1])));
  }
}

void CheckChoice(const ChoiceRef& ref) {
  switch (ref.kind) {
    case ChoiceRef::Kind::kNone:
      break;
    case ChoiceRef::Kind::kPosition:
      CHECK_GE(ref.ordinal, 1);
      CHECK_LE(ref.ordinal, 999'999);
      break;
    case ChoiceRef::Kind::kNumber:
      CHECK_GE(ref.number, 1);
      CHECK_LE(ref.number, 999'999);
      break;
    case ChoiceRef::Kind::kRelative:
      break;
    case ChoiceRef::Kind::kDescription: {
      CHECK_GE(ref.ordinal, 0);
      const size_t count =
          ref.words.size() + ref.by_words.size() + ref.from_words.size();
      CHECK_LE(count, kMaxChoiceRefWords);
      CHECK(count > 0 || ref.roles != 0);
      for (const auto* list : {&ref.words, &ref.by_words, &ref.from_words}) {
        for (const std::string& word : *list) {
          CHECK(!word.empty());
          CHECK_EQ(word.find(' '), std::string::npos);
          CHECK(IsValidUtf8(word));
        }
      }
      break;
    }
  }
}

bool CarriesText(K kind) {
  return kind == K::kSearch || kind == K::kFindInPage || kind == K::kType ||
         kind == K::kCorrection || kind == K::kRememberRoutine ||
         kind == K::kForgetRoutine || kind == K::kRunRoutine;
}

bool Same(const VoiceCommand& a, const VoiceCommand& b) {
  return a.kind == b.kind && a.heard == b.heard && a.text == b.text &&
         a.site == b.site && a.site_words == b.site_words &&
         a.whole_query == b.whole_query && a.seconds == b.seconds &&
         a.level == b.level && a.count == b.count && a.verb == b.verb &&
         a.scope == b.scope && a.choice.kind == b.choice.kind &&
         a.choice.ordinal == b.choice.ordinal &&
         a.choice.number == b.choice.number &&
         a.choice.words == b.choice.words &&
         a.choice.by_words == b.choice.by_words;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0)
    return 0;
  const uint8_t flags = data[0];
  const std::string_view utterance(reinterpret_cast<const char*>(data + 1),
                                   size - 1);
  ParseContext context;
  context.expecting_yes_no = flags & 1;
  context.expecting_choice = flags & 2;
  context.text_field_focused = flags & 4;
  if (flags & 8)
    context.wake_phrase = "ok computer";

  const VoiceCommand command = ParseVoiceCommand(utterance, context);
  CHECK(Same(command, ParseVoiceCommand(utterance, context)));

  // Refused exactly when it must be.
  if (utterance.size() > kMaxUtteranceBytes) {
    CHECK(command.kind == K::kRefused);
    CHECK(command.refusal == RefusalReason::kTooLong);
  } else if (!IsValidUtf8(utterance)) {
    CHECK(command.kind == K::kRefused);
    CHECK(command.refusal == RefusalReason::kInvalidUtf8);
  } else {
    CHECK(command.kind != K::kRefused);
    CHECK(command.refusal == RefusalReason::kNone);
  }
  if (command.kind == K::kRefused) {
    CHECK(command.heard.empty());
    return 0;
  }

  CheckClean(command.heard, kMaxUtteranceBytes);
  CheckClean(command.text, kMaxCommandTextBytes);
  CheckClean(command.site_words, kMaxUtteranceBytes);
  CheckClean(command.whole_query, kMaxUtteranceBytes);
  CHECK_EQ(!command.text.empty(), CarriesText(command.kind));
  CheckSite(command.site);
  if (command.kind == K::kOpenSite)
    CHECK(!command.site.empty());
  if (command.site_is_address) {
    CHECK_NE(command.site.find('.'), std::string::npos);
  }

  // Context-only commands.
  if (command.kind == K::kYes || command.kind == K::kNo) {
    CHECK(context.expecting_yes_no);
  }
  if (command.kind == K::kNewLine || command.kind == K::kDeleteLastWord ||
      command.kind == K::kClearField || command.kind == K::kReadBack) {
    CHECK(context.text_field_focused);
  }

  // Values within range.
  CHECK_GE(command.level, 0);
  CHECK_LE(command.level, 100);
  CHECK_GE(command.seconds, 0);
  CHECK_LE(command.seconds, kMaxSpokenSeconds);
  CHECK_GE(command.count, 1);
  CHECK_LE(command.count, 50);
  CheckChoice(command.choice);
  if (command.kind == K::kChoose) {
    CHECK(command.choice.kind != ChoiceRef::Kind::kNone);
  }
  if (command.kind == K::kUnknown) {
    CHECK(command.text.empty());
    CHECK(command.site.empty());
  }
  return 0;
}
