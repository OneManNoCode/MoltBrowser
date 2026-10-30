// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The browser's side of the voice helper protocol (docs/design/
// VOICE_HELPER_PROTOCOL.md §3, §5): molt-voice's frames read as untrusted.
// A helper that page text took over could write anything, so every frame
// is checked before its audio is played:
//
//   - READY first and once, with protocol 1 and a sample rate of 8 to
//     48 kHz;
//   - AUDIO and DONE only for the oldest request sent and not yet done
//     (requests are answered in order, each by exactly one DONE);
//   - at most kMaxRequestAudioMs of audio per request, and a DONE whose
//     audio_ms is what arrived;
//   - ERROR at any time ends the helper.
//
// Anything else ends the helper (voice_speech_bundled.h restarts it, then
// falls back to the platform's voices). Std-only: molt_ai_unittests and
// molt_voice_speech_fuzzer build the same code.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_SPEECH_PROTOCOL_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_SPEECH_PROTOCOL_H_

#include <stddef.h>
#include <stdint.h>

#include <deque>
#include <string>
#include <vector>

#include "chrome/browser/molt_ai/voice/voice_helper/voice_frames.h"

namespace molt_ai {
namespace voice {

// The helper's own limit is 360 s per request; a little slack for
// rounding.
inline constexpr int64_t kMaxRequestAudioMs = 361000;

// A TTS rate (1 normal; voice_speech.h's 0.1 to 10) as SPEAK's rate in
// percent, within the protocol's 50 to 250.
int VoiceRatePercent(double rate);

// How long the helper may send nothing while a request it was sent waits
// for its audio or its DONE and the browser has nothing left to play. The
// helper paces itself only while audio waits to be played, and synthesizes
// a unit in well under a second (VOICE_HELPER_PROTOCOL.md §4.3, §8.4).
inline constexpr int64_t kMaxAnswerGapMs = 20000;

// Whether the helper has stopped answering: |outstanding| requests it
// hasn't answered with a DONE, everything handed to the player played
// (|played_samples| >= |pushed_samples|), and nothing from it for
// kMaxAnswerGapMs (|since_progress_ms|: since the oldest outstanding
// request was sent, or its last AUDIO or DONE). Such a helper would keep
// voice mode speaking for good: it is ended.
bool StoppedAnswering(size_t outstanding,
                      uint64_t pushed_samples,
                      uint64_t played_samples,
                      int64_t since_progress_ms);

enum class VoiceEventType {
  kReady,
  kAudio,
  kDone,
  kError,
};

struct VoiceEvent {
  VoiceEvent();
  VoiceEvent(const VoiceEvent&);
  VoiceEvent& operator=(const VoiceEvent&);
  VoiceEvent(VoiceEvent&&);
  VoiceEvent& operator=(VoiceEvent&&);
  ~VoiceEvent();

  VoiceEventType type = VoiceEventType::kError;
  // kReady.
  molt_voice::ReadyInfo ready;
  // kAudio: the request and its samples (16-bit, mono, at the READY rate).
  uint32_t id = 0;
  std::vector<int16_t> samples;
  // kDone.
  molt_voice::DoneInfo done;
  // kError (printable ASCII).
  molt_voice::ErrorCode code = molt_voice::ErrorCode::kInternal;
  std::string message;
};

class VoiceStreamChecker {
 public:
  VoiceStreamChecker();
  VoiceStreamChecker(const VoiceStreamChecker&) = delete;
  VoiceStreamChecker& operator=(const VoiceStreamChecker&) = delete;
  ~VoiceStreamChecker();

  // A new helper: nothing sent, not ready.
  void Reset();

  // The browser sent SPEAK |id|.
  void Sent(uint32_t id);

  // One frame from the helper (already whole, from a FrameReader of
  // direction kFromHelper). False, with |why| (no helper text in it), if it
  // breaks the protocol: the helper must then be stopped.
  bool Accept(const molt_voice::Frame& frame,
              VoiceEvent* event,
              std::string* why);

  bool ready() const { return ready_; }
  int sample_rate() const { return sample_rate_; }
  // SPEAKs sent and not yet answered by a DONE.
  size_t outstanding() const { return outstanding_.size(); }

 private:
  bool ready_ = false;
  int sample_rate_ = 0;
  std::deque<uint32_t> outstanding_;
  // Samples of the oldest outstanding request so far.
  uint64_t head_samples_ = 0;
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_SPEECH_PROTOCOL_H_
