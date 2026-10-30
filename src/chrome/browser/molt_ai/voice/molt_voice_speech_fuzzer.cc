// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// molt-voice's stdout as a helper a hostile input took over may write it
// (docs/design/VOICE_HELPER_PROTOCOL.md §3, §5), read the way the browser
// reads it (voice_speech_bundled.cc): molt_voice::FrameReader, then
// VoiceStreamChecker. The first byte picks how the stream is cut into
// reads and how many requests the browser has sent.
//
// Checks: frames come out whole and within the limits, an error ends the
// stream for good; an accepted READY has protocol 1 and a rate of 8 to
// 48 kHz and comes once, first; accepted AUDIO and DONE belong to the
// oldest request sent and not yet done, in order, each request's audio
// within kMaxRequestAudioMs and its DONE's length what arrived; nothing
// is accepted after a violation is reported (the browser stops the
// helper then).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "base/check.h"
#include "base/check_op.h"
#include "chrome/browser/molt_ai/voice/voice_helper/voice_frames.h"
#include "chrome/browser/molt_ai/voice/voice_speech_protocol.h"

namespace molt_ai {
namespace voice {
namespace {

void Fuzz(const uint8_t* data, size_t size, uint8_t mode) {
  molt_voice::FrameReader reader(
      molt_voice::FrameReader::Direction::kFromHelper);
  VoiceStreamChecker checker;
  std::deque<uint32_t> sent;
  const uint32_t requests = 1 + (mode & 7);
  for (uint32_t id = 1; id <= requests; ++id) {
    checker.Sent(id);
    sent.push_back(id);
  }
  const size_t step = 1 + (mode >> 3) * 97;
  bool ready = false;
  int sample_rate = 0;
  uint64_t head_samples = 0;
  for (size_t at = 0; at < size; at += step) {
    CHECK(reader.Append(data + at, std::min(step, size - at)));
    while (true) {
      molt_voice::Frame frame;
      std::string error;
      const molt_voice::FrameReader::Status status =
          reader.Next(&frame, &error);
      if (status == molt_voice::FrameReader::Status::kNeedMore) {
        break;
      }
      if (status == molt_voice::FrameReader::Status::kError) {
        CHECK(!error.empty());
        return;
      }
      CHECK_LE(frame.payload.size(), molt_voice::kMaxFramePayloadBytes);
      VoiceEvent event;
      std::string why;
      if (!checker.Accept(frame, &event, &why)) {
        CHECK(!why.empty());
        return;  // the browser stops the helper
      }
      switch (event.type) {
        case VoiceEventType::kReady:
          CHECK(!ready);
          ready = true;
          CHECK_EQ(event.ready.protocol, molt_voice::kProtocolVersion);
          CHECK_GE(event.ready.sample_rate, molt_voice::kMinSampleRate);
          CHECK_LE(event.ready.sample_rate, molt_voice::kMaxSampleRate);
          sample_rate = event.ready.sample_rate;
          for (char c : event.ready.voice) {
            CHECK(c >= 0x20 && c < 0x7F);
          }
          break;
        case VoiceEventType::kAudio:
          CHECK(ready);
          CHECK(!sent.empty());
          CHECK_EQ(event.id, sent.front());
          CHECK(!event.samples.empty());
          head_samples += event.samples.size();
          CHECK_LE(head_samples, static_cast<uint64_t>(kMaxRequestAudioMs) *
                                     static_cast<uint64_t>(sample_rate) / 1000);
          break;
        case VoiceEventType::kDone:
          CHECK(ready);
          CHECK(!sent.empty());
          CHECK_EQ(event.id, sent.front());
          CHECK_EQ(static_cast<uint64_t>(event.done.audio_ms),
                   head_samples * 1000 / static_cast<uint64_t>(sample_rate));
          CHECK_LE(static_cast<uint32_t>(event.done.status),
                   molt_voice::kMaxDoneStatus);
          sent.pop_front();
          head_samples = 0;
          // The browser sends another now and then.
          if ((mode & 0x40) && sent.size() < 4) {
            const uint32_t next =
                requests + 1 + static_cast<uint32_t>(sent.size());
            checker.Sent(next);
            sent.push_back(next);
          }
          break;
        case VoiceEventType::kError:
          for (char c : event.message) {
            CHECK(c >= 0x20 && c < 0x7F);
          }
          break;
      }
      CHECK_EQ(checker.outstanding(), sent.size());
    }
  }
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 1) {
    return 0;
  }
  molt_ai::voice::Fuzz(data + 1, size - 1, data[0]);
  return 0;
}
