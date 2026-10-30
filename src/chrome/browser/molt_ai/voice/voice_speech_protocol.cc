// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_speech_protocol.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace molt_ai {
namespace voice {

int VoiceRatePercent(double rate) {
  if (!(rate == rate)) {
    return 100;  // NaN
  }
  const double percent = std::round(rate * 100.0);
  return static_cast<int>(
      std::clamp(percent, static_cast<double>(molt_voice::kMinRatePercent),
                 static_cast<double>(molt_voice::kMaxRatePercent)));
}

bool StoppedAnswering(size_t outstanding,
                      uint64_t pushed_samples,
                      uint64_t played_samples,
                      int64_t since_progress_ms) {
  return outstanding > 0 && played_samples >= pushed_samples &&
         since_progress_ms >= kMaxAnswerGapMs;
}

VoiceEvent::VoiceEvent() = default;
VoiceEvent::VoiceEvent(const VoiceEvent&) = default;
VoiceEvent& VoiceEvent::operator=(const VoiceEvent&) = default;
VoiceEvent::VoiceEvent(VoiceEvent&&) = default;
VoiceEvent& VoiceEvent::operator=(VoiceEvent&&) = default;
VoiceEvent::~VoiceEvent() = default;

VoiceStreamChecker::VoiceStreamChecker() = default;
VoiceStreamChecker::~VoiceStreamChecker() = default;

void VoiceStreamChecker::Reset() {
  ready_ = false;
  sample_rate_ = 0;
  outstanding_.clear();
  head_samples_ = 0;
}

void VoiceStreamChecker::Sent(uint32_t id) {
  outstanding_.push_back(id);
}

bool VoiceStreamChecker::Accept(const molt_voice::Frame& frame,
                                VoiceEvent* event,
                                std::string* why) {
  using molt_voice::FromHelper;
  *event = VoiceEvent();
  switch (static_cast<FromHelper>(frame.type)) {
    case FromHelper::kReady:
      if (ready_) {
        *why = "it said READY twice";
        return false;
      }
      if (!molt_voice::ParseReady(frame.payload, &event->ready)) {
        *why = "its READY is malformed";
        return false;
      }
      if (event->ready.protocol != molt_voice::kProtocolVersion) {
        *why = "it speaks protocol " + std::to_string(event->ready.protocol);
        return false;
      }
      ready_ = true;
      sample_rate_ = event->ready.sample_rate;
      event->type = VoiceEventType::kReady;
      return true;
    case FromHelper::kAudio: {
      if (!ready_) {
        *why = "it wrote audio before READY";
        return false;
      }
      if (!molt_voice::ParseAudio(frame.payload, &event->id, &event->samples)) {
        *why = "its AUDIO is malformed";
        return false;
      }
      if (outstanding_.empty() || event->id != outstanding_.front()) {
        *why = "it wrote audio for a request that isn't being said";
        return false;
      }
      head_samples_ += event->samples.size();
      const uint64_t max_samples = static_cast<uint64_t>(kMaxRequestAudioMs) *
                                   static_cast<uint64_t>(sample_rate_) / 1000;
      if (head_samples_ > max_samples) {
        *why = "it wrote more audio than a request may have";
        return false;
      }
      event->type = VoiceEventType::kAudio;
      return true;
    }
    case FromHelper::kDone: {
      if (!ready_) {
        *why = "it wrote DONE before READY";
        return false;
      }
      if (!molt_voice::ParseDone(frame.payload, &event->done)) {
        *why = "its DONE is malformed";
        return false;
      }
      if (outstanding_.empty() || event->done.id != outstanding_.front()) {
        *why = "it answered a request out of order";
        return false;
      }
      const uint64_t arrived_ms =
          head_samples_ * 1000 / static_cast<uint64_t>(sample_rate_);
      if (event->done.audio_ms != arrived_ms) {
        *why = "its DONE doesn't match the audio it wrote";
        return false;
      }
      outstanding_.pop_front();
      head_samples_ = 0;
      event->id = event->done.id;
      event->type = VoiceEventType::kDone;
      return true;
    }
    case FromHelper::kError:
      if (!molt_voice::ParseError(frame.payload, &event->code,
                                  &event->message)) {
        *why = "its ERROR is malformed";
        return false;
      }
      event->type = VoiceEventType::kError;
      return true;
  }
  *why = "it wrote an unknown frame";
  return false;
}

}  // namespace voice
}  // namespace molt_ai
