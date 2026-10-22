// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The inference service's prompt helpers (engine/prompt_utils.h) on page
// text: NeutralizeSpecialTokens() is what keeps a page from writing
// "<|im_end|><|im_start|>system" into a chat prompt and getting a real
// system turn. Also the token-level helpers on any token lists.
//
// Checks: after neutralization no chat-template special text (as the engine
// collects them: LooksLikeSpecialTokenText() vocab texts) occurs in the
// text; removing the inserted zero-width spaces gives the page's text back;
// text without special texts is unchanged; TruncateTokensMiddle() returns
// exactly |limit| tokens (or the input when it fits), from the input's head
// and tail around the marker; CommonPrefixLength() is the common prefix.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <fuzzer/FuzzedDataProvider.h>

#include "base/check.h"
#include "chrome/services/molt_ai/inference/engine/prompt_utils.h"

namespace {

constexpr char kZeroWidthSpace[] = "\xE2\x80\x8B";

std::vector<int32_t> ConsumeTokens(FuzzedDataProvider* provider,
                                   size_t max) {
  std::vector<int32_t> out(provider->ConsumeIntegralInRange<size_t>(0, max));
  for (int32_t& t : out) {
    t = provider->ConsumeIntegralInRange<int32_t>(0, 7);
  }
  return out;
}

void CheckNeutralize(FuzzedDataProvider* provider) {
  std::vector<std::string> specials;
  const size_t count = provider->ConsumeIntegralInRange<size_t>(0, 8);
  for (size_t i = 0; i < count; ++i) {
    std::string s = provider->ConsumeRandomLengthString(32);
    // The engine only passes vocab texts that look like markup. A vocab
    // text that itself contains a zero-width space could be produced by
    // the neutralization (a crafted model file can do worse anyway).
    if (molt_ai::LooksLikeSpecialTokenText(s) &&
        s.find(kZeroWidthSpace) == std::string::npos) {
      specials.push_back(std::move(s));
    }
  }
  const std::string text = provider->ConsumeRandomLengthString(4096);
  const std::string out = molt_ai::NeutralizeSpecialTokens(text, specials);

  bool had_special = false;
  for (const std::string& s : specials) {
    if (text.find(s) != std::string::npos) had_special = true;
    CHECK(out.find(s) == std::string::npos)
        << "a special token text survived neutralization";
  }
  if (!had_special) CHECK(out == text);
  // Exactly one zero-width space after the first byte of each occurrence,
  // and nothing else changed.
  std::string expected;
  for (size_t i = 0; i < text.size(); ++i) {
    expected.push_back(text[i]);
    for (const std::string& s : specials) {
      if (text.compare(i, s.size(), s) == 0) {
        expected += kZeroWidthSpace;
        break;
      }
    }
  }
  CHECK(out == expected);
}

void CheckTokens(FuzzedDataProvider* provider) {
  const std::vector<int32_t> tokens = ConsumeTokens(provider, 512);
  const std::vector<int32_t> marker = ConsumeTokens(provider, 8);
  const size_t limit = provider->ConsumeIntegralInRange<size_t>(0, 600);
  const std::vector<int32_t> out =
      molt_ai::TruncateTokensMiddle(tokens, limit, marker);
  if (tokens.size() <= limit) {
    CHECK(out == tokens);
  } else {
    CHECK(out.size() == limit);
    if (limit <= marker.size()) {
      CHECK(std::equal(out.begin(), out.end(), tokens.end() - limit));
    } else {
      // head + marker + tail, the head a prefix and the tail a suffix.
      const size_t keep = limit - marker.size();
      const size_t head = keep / 3;
      CHECK(std::equal(out.begin(), out.begin() + head, tokens.begin()));
      CHECK(std::equal(marker.begin(), marker.end(), out.begin() + head));
      CHECK(std::equal(out.begin() + head + marker.size(), out.end(),
                       tokens.end() - (keep - head)));
    }
  }
  const std::vector<int32_t> other = ConsumeTokens(provider, 64);
  const size_t common = molt_ai::CommonPrefixLength(tokens, other);
  CHECK(common <= std::min(tokens.size(), other.size()));
  CHECK(std::equal(tokens.begin(), tokens.begin() + common, other.begin()));
  if (common < tokens.size() && common < other.size()) {
    CHECK(tokens[common] != other[common]);
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  FuzzedDataProvider provider(data, size);
  if (provider.ConsumeBool()) {
    CheckNeutralize(&provider);
  } else {
    CheckTokens(&provider);
  }
  return 0;
}
