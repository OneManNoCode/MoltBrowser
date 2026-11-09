// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_audio_samples.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace molt_ai {
namespace voice {

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Note {
  double hz;
  int ms;
};

// Appends |note| at |level| (0..1 of full scale) with raised-cosine edges
// (an attack of 4 ms, a release of a third of the note), then |gap_ms| of
// silence.
void AppendNote(const Note& note,
                double level,
                int gap_ms,
                std::vector<int16_t>* out) {
  const size_t count = static_cast<size_t>(kEarconSampleRate) *
                       static_cast<size_t>(note.ms) / 1000;
  const size_t attack = static_cast<size_t>(kEarconSampleRate) * 4 / 1000;
  const size_t release = count / 3;
  for (size_t i = 0; i < count; ++i) {
    double envelope = 1;
    if (i < attack) {
      envelope = 0.5 - 0.5 * std::cos(kPi * static_cast<double>(i) /
                                      static_cast<double>(attack));
    } else if (i + release >= count) {
      const double into = static_cast<double>(i + release - count) /
                          static_cast<double>(release);
      envelope = 0.5 + 0.5 * std::cos(kPi * into);
    }
    // A touch of the second harmonic makes it less piercing than a pure
    // sine at the same loudness.
    const double t = static_cast<double>(i) / kEarconSampleRate;
    const double wave = 0.85 * std::sin(2 * kPi * note.hz * t) +
                        0.15 * std::sin(4 * kPi * note.hz * t);
    out->push_back(
        static_cast<int16_t>(std::lround(wave * envelope * level * 32767.0)));
  }
  out->insert(out->end(),
              static_cast<size_t>(kEarconSampleRate) *
                  static_cast<size_t>(gap_ms) / 1000,
              int16_t{0});
}

}  // namespace

int16_t FloatToS16(float sample) {
  if (!(sample == sample)) {
    return 0;  // NaN
  }
  const float clamped = std::clamp(sample, -1.0f, 1.0f);
  const int64_t value = std::lround(clamped * 32767.0f);
  return static_cast<int16_t>(std::clamp<int64_t>(value, -32768, 32767));
}

AudioFrameChunker::AudioFrameChunker(size_t frame_samples)
    : frame_samples_(std::max<size_t>(frame_samples, 1)) {
  pending_.reserve(frame_samples_);
}

AudioFrameChunker::~AudioFrameChunker() = default;

void AudioFrameChunker::Add(std::span<const float> samples,
                            std::vector<std::vector<int16_t>>* frames) {
  for (const float sample : samples) {
    pending_.push_back(FloatToS16(sample));
    if (pending_.size() == frame_samples_) {
      frames->push_back(std::move(pending_));
      pending_ = std::vector<int16_t>();
      pending_.reserve(frame_samples_);
    }
  }
}

void AudioFrameChunker::Reset() {
  pending_.clear();
}

std::vector<int16_t> MakeEarcon(EarconKind kind) {
  // C5, E5, G5, A5, E6.
  constexpr double kC5 = 523.25;
  constexpr double kE5 = 659.25;
  constexpr double kG5 = 783.99;
  constexpr double kA5 = 880.00;
  constexpr double kE6 = 1318.51;
  constexpr double kFull = 0.3;
  std::vector<int16_t> out;
  out.reserve(kMaxEarconSamples);
  switch (kind) {
    case EarconKind::kOff:
      AppendNote({kG5, 70}, kFull, 15, &out);
      AppendNote({kE5, 70}, kFull, 15, &out);
      AppendNote({kC5, 110}, kFull, 0, &out);
      break;
    case EarconKind::kWaiting:
      AppendNote({kE5, 70}, kFull * 0.6, 20, &out);
      AppendNote({kC5, 100}, kFull * 0.6, 0, &out);
      break;
    case EarconKind::kListening:
      AppendNote({kC5, 70}, kFull, 15, &out);
      AppendNote({kG5, 110}, kFull, 0, &out);
      break;
    case EarconKind::kThinking:
      AppendNote({kA5, 45}, kFull * 0.8, 0, &out);
      break;
    case EarconKind::kSpeaking:
      AppendNote({kE6, 35}, kFull * 0.4, 0, &out);
      break;
  }
  out.resize(std::min(out.size(), kMaxEarconSamples));
  return out;
}

MicrophoneLevelHistory::MicrophoneLevelHistory() {
  // No slot's number: never matched.
  for (std::atomic<uint64_t> &slot : slots_) {
    slot.store(std::numeric_limits<uint64_t>::max(), std::memory_order_relaxed);
  }
}

MicrophoneLevelHistory::~MicrophoneLevelHistory() = default;

void MicrophoneLevelHistory::Note(int64_t start_us, int64_t duration_us,
                                  float peak) {
  constexpr int64_t kMaxSlot = (int64_t{1} << (64 - kPeakBits)) - 2;
  if (start_us < 0 || duration_us < 0 || !std::isfinite(peak) ||
      start_us > kMaxSlot * kLevelSlotUs - duration_us) {
    return;
  }
  const uint64_t millionths =
      static_cast<uint64_t>(std::clamp(peak, 0.0f, 1.0f) * 1e6f);
  const int64_t first = start_us / kLevelSlotUs;
  const int64_t last =
      (start_us + std::max<int64_t>(duration_us - 1, 0)) / kLevelSlotUs;
  const int64_t latest = latest_slot_.load(std::memory_order_relaxed);
  if (last < latest) {
    return; // out of order: what is held is later
  }
  // A buffer longer than the history keeps only its last slots.
  const int64_t from =
      std::max(first, last - static_cast<int64_t>(kLevelSlots) + 1);
  for (int64_t slot = from; slot <= last; ++slot) {
    std::atomic<uint64_t> &cell =
        slots_[static_cast<size_t>(slot) % kLevelSlots];
    const uint64_t number = static_cast<uint64_t>(slot);
    uint64_t value = (number << kPeakBits) | millionths;
    const uint64_t held = cell.load(std::memory_order_relaxed);
    if ((held >> kPeakBits) == number) {
      value = std::max(value, held); // the same slot: the louder
    }
    cell.store(value, std::memory_order_relaxed);
  }
  latest_slot_.store(last, std::memory_order_release);
}

std::optional<float>
MicrophoneLevelHistory::LoudestBetween(int64_t from_us, int64_t to_us) const {
  const int64_t latest = latest_slot_.load(std::memory_order_acquire);
  if (from_us < 0 || to_us < from_us || latest < 0) {
    return std::nullopt;
  }
  const int64_t first = from_us / kLevelSlotUs;
  const int64_t last = std::min(to_us / kLevelSlotUs, latest);
  // Reaching back past what is held, or all of it later: not known.
  if (first <= latest - static_cast<int64_t>(kLevelSlots) || last < first) {
    return std::nullopt;
  }
  constexpr uint64_t kPeakMask = (uint64_t{1} << kPeakBits) - 1;
  std::optional<float> loudest;
  for (int64_t slot = first; slot <= last; ++slot) {
    const uint64_t held = slots_[static_cast<size_t>(slot) % kLevelSlots].load(
        std::memory_order_relaxed);
    if ((held >> kPeakBits) != static_cast<uint64_t>(slot)) {
      continue; // not captured, or written over since
    }
    const float peak = static_cast<float>(held & kPeakMask) / 1e6f;
    loudest = std::max(loudest.value_or(0.0f), peak);
  }
  return loudest;
}

}  // namespace voice
}  // namespace molt_ai
