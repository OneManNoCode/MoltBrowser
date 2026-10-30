// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The speech helper, from the browser's side (docs/design/
// TOUCHLESS_BROWSING.md §6.1, docs/design/SPEECH_HELPER_PROTOCOL.md):
// VoiceListener starts the resident `molt-speech` helper confined
// (common/helper_resident.h: its model directory readable, nothing
// writable, no network, its memory and CPU bounded), streams the
// microphone's frames to it, and reads back what it heard.
//
//   - The helper is started as `molt-speech --models <model directory>`.
//   - Untrusted output: every line passes HelperLineReader (at most
//     kMaxHelperLineBytes), HelperLineBudget (no flood) and
//     ParseHelperLine() (known types and fields only, valid UTF-8, capped,
//     text through SpeechSafeText()), then HelperEventSequence (the
//     protocol's order of events). Anything else ends the helper; it is
//     then restarted like a crash. So does an `error` line, once passed
//     on: the helper exits after one.
//   - Restarts: after a crash, a protocol error, a limit or a stall, the
//     helper starts again after kRestartDelays (1 s, then 4 s, then 16 s).
//     After kMaxFailures failures in a row it gives up and says so; so
//     does a helper whose sandbox can't be set up, or that is missing. A
//     helper that ran kStableRunTime after its `ready`, however it ended,
//     starts the count afresh (HelperFailureCount).
//   - Watchdog: no `ready` within kReadyTimeout, its memory or CPU past the
//     limits (checked every second), or its stdin backed up for
//     kStallTimeout (it stopped reading), end it.
//   - Its clock: the audio sent to the helper and when it was captured
//     (HelperAudioClock) map each event's audio_ms back to the
//     microphone's time; the event goes on with its lag behind it
//     (HelperEvent::lag_ms), for voice_session_logic.h.
//   - Threads: the helper is started on, and read from, a dedicated thread
//     that lives as long as it (some Linux kernels tie it to that thread);
//     writes go through a second dedicated thread. Audio may be
//     pushed from any thread (the capture thread) through audio_sink().
//     Everything else, and every delegate call, is on the owner's sequence
//     (the UI thread).
//   - Stop() and the destructor send SHUTDOWN, close its stdin and kill it
//     kShutdownGrace later. The helper also dies with the browser.
//
// Where the helper and models are: scripts/bundle-speech.sh puts them in
// MoltBrowser.app/Contents/Resources/speech/ on macOS and <install
// dir>/speech/ elsewhere: molt-speech[.exe] and models/. Only the bundled
// copy runs (helper_binary.h); never one from PATH.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_LISTENER_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_LISTENER_H_

#include <stddef.h>
#include <stdint.h>

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "chrome/browser/molt_ai/voice/voice_listener_protocol.h"

namespace base {
class SingleThreadTaskRunner;
}  // namespace base

namespace molt_ai {
namespace voice {

class VoiceListener {
 public:
  // Why the helper went away.
  enum class StopReason {
    kExited,         // it exited, crashed or wrote an `error`
    kStartFailed,    // it couldn't be started confined
    kProtocolError,  // it wrote something that isn't the protocol
    kOverLimit,      // its memory or CPU
    kStalled,        // it stopped reading its input
    kNotReady,       // no `ready` in time
  };

  class Delegate {
   public:
    // The helper is ready (its `ready` line): audio and CONFIG go to it
    // now. The delegate sends CONFIG.
    virtual void OnListenerReady(const HelperEvent& ready) = 0;
    // wake, speech_start, partial, final, speech_end; and error lines the
    // helper wrote before it ended.
    virtual void OnListenerEvent(const HelperEvent& event) = 0;
    // The helper went away; another starts after a delay when
    // |restarting|.
    virtual void OnListenerLost(StopReason reason,
                                const std::string& why,
                                bool restarting) = 0;
    // No more restarts: |why| says what to tell the user.
    virtual void OnListenerGaveUp(const std::string& why) = 0;

   protected:
    virtual ~Delegate() = default;
  };

  struct Paths {
    base::FilePath helper;
    base::FilePath models;
  };

  // Takes the frames the microphone gives, from any thread.
  class AudioSink : public base::RefCountedThreadSafe<AudioSink> {
   public:
    // 16 kHz mono samples, at most kMaxFramePayloadBytes / 2, the last of
    // them captured just before |end_captured| (the microphone's
    // timestamp). Dropped while no helper is ready, and while its input is
    // backed up.
    virtual void Push(const std::vector<int16_t>& samples,
                      base::TimeTicks end_captured) = 0;

   protected:
    friend class base::RefCountedThreadSafe<AudioSink>;
    virtual ~AudioSink() = default;
  };

  static constexpr std::array<base::TimeDelta, 3> kRestartDelays = {
      base::Seconds(1), base::Seconds(4), base::Seconds(16)};
  static constexpr int kMaxFailures = HelperFailureCount::kMaxFailures;
  static constexpr base::TimeDelta kStableRunTime =
      base::Milliseconds(HelperFailureCount::kStableRunMs);
  static constexpr base::TimeDelta kReadyTimeout = base::Seconds(30);
  static constexpr base::TimeDelta kStallTimeout = base::Seconds(3);
  static constexpr base::TimeDelta kShutdownGrace = base::Milliseconds(500);
  // Audio queued for the helper beyond this is dropped (one second).
  static constexpr size_t kMaxQueuedAudioBytes = 32000;

  // The bundled helper and models; nullopt, with |why|, when they aren't
  // there (a build without them).
  static std::optional<Paths> FindBundled(std::string* why);
  // Tests: where FindBundled() looks instead (a fake helper).
  static void SetPathsForTesting(std::optional<Paths> paths);

  VoiceListener(Delegate* delegate, Paths paths);
  VoiceListener(const VoiceListener&) = delete;
  VoiceListener& operator=(const VoiceListener&) = delete;
  ~VoiceListener();

  void Start();
  void Stop();
  bool started() const { return started_; }
  // A helper is ready.
  bool ready() const { return ready_; }

  void SendConfig(const HelperConfig& config);
  void SendReset();

  // Where the microphone's frames go; the same for every helper started.
  scoped_refptr<AudioSink> audio_sink() const;

  // The confinement the helper got (for logs and tests).
  const std::string& confinement() const { return confinement_; }

 private:
  class Connection;
  class Sink;

  void Launch();
  void OnLaunched(int generation, std::string confinement);
  void OnLine(int generation, HelperEvent event);
  // |exit_code|: -1 for a signal, -2 when it never started.
  void OnEnded(int generation,
               StopReason reason,
               std::string why,
               int exit_code);
  void OnWatchdog();
  void OnLimitsChecked(int generation, std::string why);
  void EndConnection(StopReason reason, const std::string& why);
  void Send(std::string frame);

  SEQUENCE_CHECKER(sequence_checker_);
  raw_ptr<Delegate> delegate_;
  const Paths paths_;
  scoped_refptr<Sink> sink_;
  scoped_refptr<Connection> connection_;
  bool started_ = false;
  bool ready_ = false;
  int generation_ = 0;
  HelperFailureCount failures_;
  // RESET frames sent to the current helper: its events from before the
  // last one (a lower "epoch") are dropped.
  int64_t resets_sent_ = 0;
  // The current helper's events so far.
  HelperEventSequence sequence_;
  base::TimeTicks launched_at_;
  base::TimeTicks stalled_since_;
  std::string confinement_;
  base::RepeatingTimer watchdog_;
  base::OneShotTimer restart_timer_;
  base::WeakPtrFactory<VoiceListener> weak_factory_{this};
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_LISTENER_H_
