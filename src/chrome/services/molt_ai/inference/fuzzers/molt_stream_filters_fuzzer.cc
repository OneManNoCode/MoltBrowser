// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The inference service's output filters (engine/stream_filters.h) on what a
// model writes: any bytes, in pieces of any size, as llama_engine.cc chains
// them (Harmony -> Gemma thoughts -> <think> -> Utf8Assembler ->
// ToDisplayText -> StopSequenceFilter), including a forced end of the
// reasoning and the final flushes.
//
// Input: options from the end of the data (FuzzedDataProvider), stop
// strings and pieces as random-length strings from the front.
//
// Checks: no memory or UB error or timeout; every chunk the service would
// send is valid UTF-8 without NUL (the browser's StreamValidator refuses
// anything else); no stop string appears in what is shown; the browser's
// copy of the stop filter, fed the service's chunks, shows the same text;
// the stop filter gives the same text however its input is split, and holds
// back less than the longest stop string; the UTF-8 helpers agree with each
// other.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <fuzzer/FuzzedDataProvider.h>

#include "base/check.h"
#include "chrome/services/molt_ai/inference/engine/stream_filters.h"

namespace molt_ai {
namespace {

// molt_inference.mojom.
constexpr size_t kMaxStopSequences = 8;
constexpr size_t kMaxStopSequenceBytes = 64;
constexpr size_t kMaxPieces = 512;

bool HasNul(const std::string& s) {
  return s.find('\0') != std::string::npos;
}

void CheckUtf8Helpers(const std::string& bytes) {
  const std::string replaced = ReplaceInvalidUtf8(bytes);
  CHECK(IsValidUtf8(replaced));
  if (IsValidUtf8(bytes)) CHECK(replaced == bytes);
  CHECK(ReplaceInvalidUtf8(replaced) == replaced);
  const std::string display = ToDisplayText(bytes);
  CHECK(IsValidUtf8(display) && !HasNul(display));
  CHECK(ToDisplayText(display) == display);

  const size_t safe = Utf8SafePrefixLength(bytes);
  CHECK(safe <= bytes.size());
  // At most one incomplete character (< 4 bytes) is held back.
  CHECK(bytes.size() - safe < 4);
  // A valid text is never cut inside a character.
  if (IsValidUtf8(bytes)) CHECK(safe == bytes.size());
}

void CheckNoStopInside(const std::string& shown,
                       const std::vector<std::string>& stops) {
  for (const std::string& stop : stops) {
    CHECK(shown.find(stop) == std::string::npos)
        << "a stop string is in the shown text";
  }
}

// The text a stop filter over |stops| shows for |pieces|, checking how much
// it holds back after each one.
std::string RunStopFilter(const std::vector<std::string>& stops,
                          const std::vector<std::string>& pieces) {
  size_t longest = 0;
  for (const std::string& s : stops) longest = std::max(longest, s.size());
  StopSequenceFilter filter(stops);
  std::string shown;
  for (const std::string& piece : pieces) {
    shown += filter.Feed(piece);
    if (!filter.stopped()) {
      CHECK(filter.held_back() < std::max<size_t>(longest, 1));
    }
  }
  shown += filter.Flush();
  CHECK(filter.held_back() == 0);
  return shown;
}

}  // namespace
}  // namespace molt_ai

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace molt_ai;
  FuzzedDataProvider provider(data, size);
  const bool gemma_thoughts = provider.ConsumeBool();
  const bool thought_start_inside = provider.ConsumeBool();
  const bool think_start_inside = provider.ConsumeBool();
  // Pieces after which the reasoning budget runs out (forced answer).
  const size_t force_at = provider.ConsumeIntegralInRange<size_t>(0, 64);
  const size_t stop_count =
      provider.ConsumeIntegralInRange<size_t>(0, kMaxStopSequences);

  // As the browser and the service accept them: valid UTF-8, 1..64 bytes.
  std::vector<std::string> stops;
  for (size_t i = 0; i < stop_count; ++i) {
    std::string stop = provider.ConsumeRandomLengthString(kMaxStopSequenceBytes);
    if (!stop.empty() && IsValidUtf8(stop)) stops.push_back(std::move(stop));
  }

  std::vector<std::string> pieces;
  while (provider.remaining_bytes() > 0 && pieces.size() < kMaxPieces) {
    pieces.push_back(provider.ConsumeRandomLengthString(1024));
  }

  // The service's chain (llama_engine.cc, no grammar).
  HarmonyChannelFilter harmony;
  GemmaThoughtFilter gemma(gemma_thoughts && thought_start_inside);
  ThinkTagFilter think(think_start_inside);
  Utf8Assembler utf8;
  StopSequenceFilter stop(stops);
  std::vector<std::string> chunks;
  std::string display_all;  // the stop filter's input, in one piece
  std::string assembled;    // what Utf8Assembler passed on
  std::string utf8_input;   // what Utf8Assembler was given
  auto to_stop = [&](const std::string& display) {
    CHECK(IsValidUtf8(display) && !HasNul(display));
    display_all += display;
    std::string chunk = stop.Feed(display);
    if (!chunk.empty()) chunks.push_back(std::move(chunk));
  };
  auto feed = [&](const std::string& piece) {
    std::string text = harmony.Feed(piece);
    if (gemma_thoughts) text = gemma.Feed(text);
    text = think.Feed(text);
    utf8_input += text;
    const std::string out = utf8.Push(text);
    assembled += out;
    to_stop(ToDisplayText(out));
  };
  bool forced = false;
  for (size_t i = 0; i < pieces.size(); ++i) {
    feed(pieces[i]);
    if (i + 1 == force_at && !forced &&
        (harmony.hiding() || gemma.hiding() || think.in_think())) {
      forced = true;
      const char* force = gemma.hiding()
                              ? GemmaThoughtFilter::kForceClose
                              : (think.in_think()
                                     ? ThinkTagFilter::kForceClose
                                     : HarmonyChannelFilter::kForceFinal);
      feed(force);
      gemma.ForceClose();
      if (think.in_think()) feed(ThinkTagFilter::kCloseTag);
    }
  }
  std::string rest = gemma_thoughts ? think.Feed(gemma.Flush()) : std::string();
  rest += think.Flush();
  utf8_input += rest;
  std::string tail = utf8.Push(rest);
  tail += utf8.Flush();
  assembled += tail;
  CHECK(assembled == utf8_input) << "Utf8Assembler lost or changed bytes";
  to_stop(ToDisplayText(tail));
  std::string last = stop.Flush();
  if (!last.empty()) chunks.push_back(std::move(last));

  // Every chunk is valid UTF-8 without NUL (StreamValidator::OnChunk).
  std::string shown;
  for (const std::string& chunk : chunks) {
    CHECK(IsValidUtf8(chunk) && !HasNul(chunk)) << "a chunk the browser refuses";
    shown += chunk;
  }
  CheckNoStopInside(shown, stops);
  // The browser re-applies the stop strings to the chunks it receives.
  CHECK(RunStopFilter(stops, chunks) == shown);
  // The stop filter doesn't depend on how its input was split.
  CHECK(RunStopFilter(stops, {display_all}) == shown);
  if (!stop.stopped()) CHECK(shown == display_all);

  for (size_t i = 0; i < pieces.size() && i < 8; ++i) {
    CheckUtf8Helpers(pieces[i]);
  }
  return 0;
}
