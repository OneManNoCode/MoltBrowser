// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Voice mode's own voice, from the browser's side (docs/design/
// TOUCHLESS_BROWSING.md §6.6, docs/design/VOICE_HELPER_PROTOCOL.md):
// BundledVoice starts the resident `molt-voice` helper confined
// (common/helper_resident.h: its model directory readable, nothing
// writable, no network, its memory and CPU bounded), sends it what voice
// mode says, and plays the audio it sends back through Chromium's audio
// service.
//
//   - The audio is browser output: an audio::OutputDevice on the default
//     output device, created from the audio service's StreamFactory, like
//     the earcons (voice_audio.h). With Chrome-wide echo cancellation the
//     audio service's OutputDeviceMixerManager mixes every such stream for
//     that device and hands the mix to the microphone's echo canceller as
//     its reference; with the platform's echo canceller the reference is
//     the system's whole output, which holds it too. So, unlike the
//     platform's voices, the echo canceller removes this voice from what
//     the microphone hears, and the user can interrupt it by voice
//     (voice_session_logic.h).
//   - Untrusted output: every frame passes molt_voice::FrameReader (the
//     header checked first) and VoiceStreamChecker (the protocol's order,
//     the per-request limit). Anything else ends the helper.
//   - Requests are sent one or two at a time (VoiceSpeech throttles); each
//     finishes when its DONE arrived and its audio played out, or at once
//     when Cancel() drops it (a CANCEL goes to the helper, the player's
//     buffer is flushed, late audio for it is dropped).
//   - Failures: no READY within kReadyTimeout, a crash, a protocol error,
//     a limit, audio that stops playing (the output device failed), or a
//     helper that stops answering a request (voice_speech_protocol.h's
//     StoppedAnswering(); voice mode would otherwise speak for good):
//     the helper ends, the unfinished requests go back to VoiceSpeech for
//     the platform's voices, and the helper is started again after
//     kRestartDelay, at most kMaxFailures times in a row.
//   - The helper is started as `molt-voice --models <model directory>`;
//     only the bundled copy (helper_binary.h), never one from PATH.
//
// UI thread; the helper's reading and writing run on dedicated threads,
// the player on a sequence of its own (its device renders on the audio
// thread).

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_SPEECH_BUNDLED_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_SPEECH_BUNDLED_H_

#include <stddef.h>
#include <stdint.h>

#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/threading/sequence_bound.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "chrome/browser/molt_ai/voice/voice_helper/voice_frames.h"
#include "chrome/browser/molt_ai/voice/voice_speech_protocol.h"

namespace molt_ai {
namespace voice {

class BundledVoice {
 public:
  struct Paths {
    base::FilePath helper;
    base::FilePath models;
  };

  class Delegate {
   public:
    // The helper is ready: Speak() works now.
    virtual void OnBundledVoiceReady() = 0;
    // Request |id| finished: its audio played out (or it had none).
    virtual void OnBundledVoiceFinished(uint32_t id) = 0;
    // The helper went away; |unfinished| are the requests that hadn't
    // finished, oldest first. Another helper starts after a delay unless
    // |gave_up|.
    virtual void OnBundledVoiceLost(std::vector<uint32_t> unfinished,
                                    bool gave_up,
                                    const std::string& why) = 0;

   protected:
    virtual ~Delegate() = default;
  };

  static constexpr base::TimeDelta kReadyTimeout = base::Seconds(30);
  static constexpr base::TimeDelta kRestartDelay = base::Seconds(2);
  static constexpr int kMaxFailures = 2;
  static constexpr base::TimeDelta kStableRunTime = base::Seconds(60);
  static constexpr base::TimeDelta kShutdownGrace = base::Milliseconds(500);
  // Audio waiting to be played that doesn't move for this long: the output
  // device failed.
  static constexpr base::TimeDelta kOutputStallTimeout = base::Seconds(4);
  // How often the played audio is looked at while some is waiting.
  static constexpr base::TimeDelta kPlaybackPoll = base::Milliseconds(40);

  // The bundled helper and voice; nullopt, with |why|, when they aren't
  // there. Blocking (looks at files).
  static std::optional<Paths> FindBundled(std::string* why);
  // Tests: where FindBundled() looks instead (a fake helper).
  static void SetPathsForTesting(std::optional<Paths> paths);

  BundledVoice(Delegate* delegate, Paths paths);
  BundledVoice(const BundledVoice&) = delete;
  BundledVoice& operator=(const BundledVoice&) = delete;
  ~BundledVoice();

  void Start();
  // Ends the helper (SHUTDOWN); unfinished requests are dropped.
  void Stop();
  bool started() const { return started_; }
  bool ready() const { return ready_; }

  // Says |text| (at most molt_voice::kMaxSpeakTextBytesInFrame bytes) at
  // |rate_percent|; ready() only. The id names it in the delegate's calls.
  uint32_t Speak(const std::string& text, int rate_percent);
  // Drops every unfinished request now: silence at once. No delegate call.
  void Cancel();
  // Requests sent and not finished.
  size_t unfinished() const { return requests_.size(); }
  // Requests the helper hasn't answered yet (cancelled ones included).
  size_t synthesizing() const { return checker_.outstanding(); }

  // The confinement the helper got (for logs and tests).
  const std::string& confinement() const { return confinement_; }

 private:
  class Connection;
  class Player;
  class Counter;

  struct Request {
    uint32_t id = 0;
    bool done = false;
    // The player's total of samples once all of this one's are in.
    uint64_t end_sample = 0;
  };

  void Launch();
  void OnLaunched(int generation, std::string confinement);
  void OnFrame(int generation, molt_voice::Frame frame);
  void OnEnded(int generation, std::string why, int exit_code);
  void OnWatchdog();
  void OnLimitsChecked(int generation, std::string why);
  void OnPlaybackPoll();
  void Fail(const std::string& why);
  void EndConnection();
  void Send(std::string frame);

  SEQUENCE_CHECKER(sequence_checker_);
  raw_ptr<Delegate> delegate_;
  const Paths paths_;
  scoped_refptr<Connection> connection_;
  scoped_refptr<Counter> played_;
  base::SequenceBound<Player> player_;
  VoiceStreamChecker checker_;
  bool started_ = false;
  bool ready_ = false;
  int generation_ = 0;
  int failures_ = 0;
  uint32_t next_id_ = 1;
  // Unfinished requests, oldest first (cancelled ones are gone).
  std::deque<Request> requests_;
  // Samples handed to the player so far.
  uint64_t pushed_ = 0;
  uint64_t last_played_ = 0;
  base::TimeTicks played_moved_at_;
  // When the oldest request the helper hasn't answered was sent, or the
  // helper last wrote AUDIO or DONE (StoppedAnswering()).
  base::TimeTicks answered_at_;
  base::TimeTicks launched_at_;
  base::TimeTicks ready_at_;
  std::string confinement_;
  base::RepeatingTimer watchdog_;
  base::RepeatingTimer playback_poll_;
  base::OneShotTimer restart_timer_;
  base::WeakPtrFactory<BundledVoice> weak_factory_{this};
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_SPEECH_BUNDLED_H_
