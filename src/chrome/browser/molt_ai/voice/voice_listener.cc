// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_listener.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/synchronization/lock.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/single_thread_task_runner_thread_mode.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "base/thread_annotations.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "chrome/browser/molt_ai/common/helper_binary.h"
#include "chrome/browser/molt_ai/common/helper_resident.h"

#if BUILDFLAG(IS_WIN)
#include "chrome/browser/molt_ai/common/helper_sandbox_win_policy.h"
#endif

namespace molt_ai {
namespace voice {

namespace {

std::optional<VoiceListener::Paths>& PathsForTesting() {
  static base::NoDestructor<std::optional<VoiceListener::Paths>> paths;
  return *paths;
}

// Where scripts/bundle-speech.sh puts the helper and its models.
base::FilePath BundledSpeechDir() {
#if BUILDFLAG(IS_MAC)
  base::FilePath exe;
  if (!base::PathService::Get(base::FILE_EXE, &exe)) {
    return base::FilePath();
  }
  // MoltBrowser.app/Contents/MacOS/<exe> -> .../Contents/Resources/speech
  return exe.DirName().DirName().AppendASCII("Resources").AppendASCII("speech");
#else
  base::FilePath exe_dir;
  if (!base::PathService::Get(base::DIR_EXE, &exe_dir)) {
    return base::FilePath();
  }
  return exe_dir.AppendASCII("speech");
#endif
}

// An exit before the helper's own code ran: its sandbox couldn't be set up
// (Linux: helper_sandbox_linux's kSetupFailedExitCode; macOS: sandbox-exec's
// sysexits codes) or, on Windows, a library it needs couldn't be loaded.
// Starting it again would fail the same way.
bool IsStartFailure(int exit_code) {
#if BUILDFLAG(IS_LINUX)
  return exit_code == 125;
#elif BUILDFLAG(IS_MAC)
  return exit_code >= 64 && exit_code <= 78;
#elif BUILDFLAG(IS_WIN)
  return !helper_sandbox_win::DescribeStartFailure(
              static_cast<uint32_t>(exit_code))
              .empty();
#else
  return false;
#endif
}

std::string DescribeExit(std::optional<int> exit_code) {
  if (!exit_code) {
    return "it stopped answering";
  }
  if (*exit_code == -1) {
    return "it crashed";
  }
  return "it exited (code " + std::to_string(*exit_code) + ")";
}

base::TaskTraits HelperThreadTraits() {
  // A dedicated thread per helper: reading and writing block for as long as
  // it runs. It must not hold up the browser's shutdown.
  return {base::MayBlock(), base::TaskPriority::USER_BLOCKING,
          base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN};
}

int64_t ToMs(base::TimeTicks ticks) {
  return (ticks - base::TimeTicks()).InMilliseconds();
}

int64_t NowMs() {
  return ToMs(base::TimeTicks::Now());
}

}  // namespace

// One helper process: its threads, what is queued for it, and the process
// itself. The last reference goes on one of its own threads or a pool
// thread (ResidentHelper's destructor blocks while it reaps the process).
class VoiceListener::Connection
    : public base::RefCountedThreadSafe<VoiceListener::Connection> {
 public:
  Connection()
      : reader_(base::ThreadPool::CreateSingleThreadTaskRunner(
            HelperThreadTraits(),
            base::SingleThreadTaskRunnerThreadMode::DEDICATED)),
        writer_(base::ThreadPool::CreateSingleThreadTaskRunner(
            HelperThreadTraits(),
            base::SingleThreadTaskRunnerThreadMode::DEDICATED)) {}

  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  const scoped_refptr<base::SingleThreadTaskRunner>& reader() const {
    return reader_;
  }
  const scoped_refptr<base::SingleThreadTaskRunner>& writer() const {
    return writer_;
  }

  // On the reader thread: starts the helper, then reads it until it ends.
  // Everything goes to |listener| on |owner|, tagged with |generation|.
  void Run(base::CommandLine cmd,
           ResidentHelperPolicy policy,
           scoped_refptr<base::SequencedTaskRunner> owner,
           base::WeakPtr<VoiceListener> listener,
           int generation) {
    std::string error;
    std::unique_ptr<ResidentHelper> started =
        ResidentHelper::Start(cmd, policy, &error);
    if (!started) {
      owner->PostTask(
          FROM_HERE,
          base::BindOnce(&VoiceListener::OnEnded, listener, generation,
                         StopReason::kStartFailed, std::move(error), -2));
      return;
    }
    ResidentHelper* helper = started.get();
    {
      base::AutoLock lock(lock_);
      helper_ = std::move(started);
      if (killed_) {
        helper->Kill();
      }
    }
    owner->PostTask(FROM_HERE,
                    base::BindOnce(&VoiceListener::OnLaunched, listener,
                                   generation, helper->confinement()));

    HelperLineReader reader;
    HelperLineBudget budget;
    StopReason reason = StopReason::kExited;
    std::string why;
    std::array<uint8_t, 4096> buffer;
    while (reason == StopReason::kExited) {
      const int bytes = helper->Read(buffer);
      if (bytes <= 0) {
        break;
      }
      std::vector<std::string> lines;
      const bool ok = reader.Feed(
          std::string_view(reinterpret_cast<const char*>(buffer.data()),
                           static_cast<size_t>(bytes)),
          &lines);
      for (std::string& line : lines) {
        if (!budget.Take(NowMs())) {
          reason = StopReason::kProtocolError;
          why = "it wrote too many lines";
          break;
        }
        ParsedHelperLine parsed = ParseHelperLine(line);
        if (parsed.error != HelperLineError::kNone) {
          reason = StopReason::kProtocolError;
          why = "it wrote a line that isn't the protocol (" +
                std::string(HelperLineErrorName(parsed.error)) + ")";
          break;
        }
        owner->PostTask(FROM_HERE,
                        base::BindOnce(&VoiceListener::OnLine, listener,
                                       generation, std::move(parsed.event)));
      }
      if (!ok && reason == StopReason::kExited) {
        reason = StopReason::kProtocolError;
        why = "it wrote a line longer than the protocol allows";
      }
    }
    if (reason != StopReason::kExited) {
      helper->Kill();
    }
    std::optional<int> exit_code = helper->WaitForExit(base::Seconds(3));
    if (!exit_code) {
      helper->Kill();
      exit_code = helper->WaitForExit(base::Seconds(2));
    }
    if (reason == StopReason::kExited) {
      why = DescribeExit(exit_code);
    }
    owner->PostTask(
        FROM_HERE,
        base::BindOnce(&VoiceListener::OnEnded, listener, generation, reason,
                       std::move(why), exit_code.value_or(-1)));
  }

  // On the writer thread.
  void Write(std::string frame, bool counted) {
    if (counted) {
      queued_.fetch_sub(frame.size());
    }
    ResidentHelper* helper = Helper();
    if (!helper || input_closed_ || input_broken_) {
      return;
    }
    if (!helper->Write(base::as_byte_span(frame))) {
      input_broken_ = true;
    }
  }

  void CloseInput() {
    input_closed_ = true;
    if (ResidentHelper* helper = Helper()) {
      helper->CloseStdin();
    }
  }

  void CheckLimits(scoped_refptr<base::SequencedTaskRunner> owner,
                   base::WeakPtr<VoiceListener> listener,
                   int generation) {
    ResidentHelper* helper = Helper();
    std::string why;
    if (helper && !helper->CheckLimits(base::TimeTicks::Now(), &why)) {
      owner->PostTask(FROM_HERE,
                      base::BindOnce(&VoiceListener::OnLimitsChecked, listener,
                                     generation, std::move(why)));
    }
  }

  // Any thread.
  void Kill() {
    base::AutoLock lock(lock_);
    killed_ = true;
    if (helper_) {
      helper_->Kill();
    }
  }

  void AddQueued(size_t bytes) { queued_.fetch_add(bytes); }
  size_t queued() const { return queued_.load(); }

 private:
  friend class base::RefCountedThreadSafe<Connection>;
  ~Connection() = default;

  ResidentHelper* Helper() {
    base::AutoLock lock(lock_);
    return helper_.get();
  }

  const scoped_refptr<base::SingleThreadTaskRunner> reader_;
  const scoped_refptr<base::SingleThreadTaskRunner> writer_;
  base::Lock lock_;
  // Set once, then kept until this goes: the reader and the writer use it
  // at the same time (ResidentHelper allows that).
  std::unique_ptr<ResidentHelper> helper_ GUARDED_BY(lock_);
  bool killed_ GUARDED_BY(lock_) = false;
  // The writer thread's.
  bool input_closed_ = false;
  bool input_broken_ = false;
  std::atomic<size_t> queued_{0};
};

// Where the microphone's frames go: the ready helper's writer, if any.
class VoiceListener::Sink : public VoiceListener::AudioSink {
 public:
  Sink() = default;

  void Push(const std::vector<int16_t>& samples,
            base::TimeTicks end_captured) override {
    std::string frame = EncodeAudioFrame(samples);
    if (frame.empty()) {
      return;
    }
    base::AutoLock lock(lock_);
    if (!connection_) {
      return;
    }
    // Backed up: dropped (the watchdog notices a helper that stopped
    // reading).
    if (connection_->queued() + frame.size() > kMaxQueuedAudioBytes) {
      return;
    }
    // In the order the writer sends them.
    clock_.OnSent(samples.size(), ToMs(end_captured));
    connection_->AddQueued(frame.size());
    connection_->writer()->PostTask(
        FROM_HERE, base::BindOnce(&Connection::Write, connection_,
                                  std::move(frame), /*counted=*/true));
  }

  void Attach(scoped_refptr<Connection> connection) {
    base::AutoLock lock(lock_);
    connection_ = std::move(connection);
    // A helper's clock starts at its first sample.
    clock_.Reset();
  }

  // HelperAudioClock::LagMs() for the attached helper.
  std::optional<int64_t> LagMs(int64_t audio_ms, int64_t now_ms) {
    base::AutoLock lock(lock_);
    return clock_.LagMs(audio_ms, now_ms);
  }

  size_t queued() {
    base::AutoLock lock(lock_);
    return connection_ ? connection_->queued() : 0;
  }

 private:
  ~Sink() override = default;

  base::Lock lock_;
  scoped_refptr<Connection> connection_ GUARDED_BY(lock_);
  // What went to that helper, and when it was captured.
  HelperAudioClock clock_ GUARDED_BY(lock_);
};

// static
std::optional<VoiceListener::Paths> VoiceListener::FindBundled(
    std::string* why) {
  if (PathsForTesting()) {
    return PathsForTesting();
  }
  const base::FilePath dir = BundledSpeechDir();
#if BUILDFLAG(IS_WIN)
  const std::vector<base::FilePath> names = {
      base::FilePath(FILE_PATH_LITERAL("molt-speech.exe"))};
#else
  const std::vector<base::FilePath> names = {
      base::FilePath(FILE_PATH_LITERAL("molt-speech"))};
#endif
  // The bundled copy only: no system path, no PATH (helper_binary.h).
  const HelperBinary found =
      FindHelperBinary(dir, names, /*linux_system_paths=*/{});
  if (!found.found()) {
    LOG(WARNING) << "[MoltVoice] no speech helper: " << found.error;
    *why = "voice mode's speech recognition isn't installed";
    return std::nullopt;
  }
  const base::FilePath models = dir.AppendASCII("models");
  if (!base::DirectoryExists(models)) {
    *why = "voice mode's speech models aren't installed";
    return std::nullopt;
  }
  return Paths{found.path, models};
}

// static
void VoiceListener::SetPathsForTesting(std::optional<Paths> paths) {
  PathsForTesting() = std::move(paths);
}

VoiceListener::VoiceListener(Delegate* delegate, Paths paths)
    : delegate_(delegate),
      paths_(std::move(paths)),
      sink_(base::MakeRefCounted<Sink>()) {}

VoiceListener::~VoiceListener() {
  Stop();
}

scoped_refptr<VoiceListener::AudioSink> VoiceListener::audio_sink() const {
  return sink_;
}

void VoiceListener::Start() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (started_) {
    return;
  }
  started_ = true;
  failures_.Clear();
  Launch();
}

void VoiceListener::Stop() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  started_ = false;
  restart_timer_.Stop();
  watchdog_.Stop();
  if (!connection_) {
    return;
  }
  // SHUTDOWN, end of file, and a kill if it lingers.
  Send(EncodeHelperFrame(HelperFrameType::kShutdown, std::string_view()));
  connection_->writer()->PostTask(
      FROM_HERE, base::BindOnce(&Connection::CloseInput, connection_));
  base::ThreadPool::PostDelayedTask(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(&Connection::Kill, connection_), kShutdownGrace);
  EndConnection(StopReason::kExited, std::string());
}

void VoiceListener::Launch() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  ++generation_;
  ready_ = false;
  resets_sent_ = 0;
  sequence_ = HelperEventSequence();
  stalled_since_ = base::TimeTicks();
  connection_ = base::MakeRefCounted<Connection>();
  launched_at_ = base::TimeTicks::Now();
  failures_.OnStarted();

  // The helper gets the path of its model directory, and nothing else
  // (SPEECH_HELPER_PROTOCOL.md §1).
  base::CommandLine cmd(paths_.helper);
  cmd.AppendArg("--models");
  cmd.AppendArgPath(paths_.models);
  ResidentHelperPolicy policy;
  policy.read_only_paths = {paths_.models};

  connection_->reader()->PostTask(
      FROM_HERE, base::BindOnce(&Connection::Run, connection_, std::move(cmd),
                                std::move(policy),
                                base::SequencedTaskRunner::GetCurrentDefault(),
                                weak_factory_.GetWeakPtr(), generation_));
  watchdog_.Start(
      FROM_HERE, base::Seconds(1),
      base::BindRepeating(&VoiceListener::OnWatchdog, base::Unretained(this)));
}

void VoiceListener::OnLaunched(int generation, std::string confinement) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (generation != generation_) {
    return;
  }
  confinement_ = std::move(confinement);
}

void VoiceListener::OnLine(int generation, HelperEvent event) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (generation != generation_ || !connection_) {
    return;
  }
  // Checked before anything is dropped: the helper's own order holds
  // whatever the browser sent it.
  if (!sequence_.Accept(event, resets_sent_)) {
    connection_->Kill();
    EndConnection(StopReason::kProtocolError,
                  sequence_.ready() ? "it wrote events out of order"
                                    : "it spoke before it was ready");
    return;
  }
  if (event.type == HelperEventType::kReady) {
    ready_ = true;
    failures_.OnReady(NowMs());
    sink_->Attach(connection_);
    delegate_->OnListenerReady(event);
    return;
  }
  // What it wrote before it read our last RESET is about a dropped
  // utterance.
  if (event.epoch >= 0 && event.epoch < resets_sent_) {
    return;
  }
  // Where its audio_ms falls in what it was sent, by the microphone's clock.
  event.lag_ms = event.audio_ms >= 0 ? sink_->LagMs(event.audio_ms, NowMs())
                                     : std::nullopt;
  const bool error = event.type == HelperEventType::kError;
  base::WeakPtr<VoiceListener> self = weak_factory_.GetWeakPtr();
  delegate_->OnListenerEvent(event);
  // The helper exits after an `error` (SPEECH_HELPER_PROTOCOL.md §3.7),
  // passed on once: it ends now, so one that writes more and carries on is
  // restarted, with the backoff, like a crash.
  if (error && self && connection_) {
    connection_->Kill();
    EndConnection(StopReason::kExited, "it reported an error");
  }
}

void VoiceListener::OnEnded(int generation,
                            StopReason reason,
                            std::string why,
                            int exit_code) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (generation != generation_) {
    return;
  }
  if (!ready_ && IsStartFailure(exit_code)) {
    reason = StopReason::kStartFailed;
    why = "its sandbox couldn't be set up";
  }
  EndConnection(reason, why);
}

void VoiceListener::OnLimitsChecked(int generation, std::string why) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (generation != generation_ || !connection_) {
    return;
  }
  LOG(WARNING) << "[MoltVoice] speech helper stopped: " << why;
  connection_->Kill();
  EndConnection(StopReason::kOverLimit, why);
}

void VoiceListener::OnWatchdog() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!connection_) {
    return;
  }
  const base::TimeTicks now = base::TimeTicks::Now();
  if (!ready_ && now - launched_at_ >= kReadyTimeout) {
    connection_->Kill();
    EndConnection(StopReason::kNotReady,
                  "it didn't get ready in " +
                      std::to_string(kReadyTimeout.InSeconds()) + " s");
    return;
  }
  // Its input backed up: it stopped reading.
  if (sink_->queued() + kMaxQueuedAudioBytes / 8 > kMaxQueuedAudioBytes) {
    if (stalled_since_.is_null()) {
      stalled_since_ = now;
    } else if (now - stalled_since_ >= kStallTimeout) {
      connection_->Kill();
      EndConnection(StopReason::kStalled, "it stopped reading its input");
      return;
    }
  } else {
    stalled_since_ = base::TimeTicks();
  }
  connection_->writer()->PostTask(
      FROM_HERE, base::BindOnce(&Connection::CheckLimits, connection_,
                                base::SequencedTaskRunner::GetCurrentDefault(),
                                weak_factory_.GetWeakPtr(), generation_));
}

void VoiceListener::EndConnection(StopReason reason, const std::string& why) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Events of this helper still on their way are dropped.
  ++generation_;
  ready_ = false;
  watchdog_.Stop();
  sink_->Attach(nullptr);
  if (connection_) {
    // Its threads let it go: never this one (the helper is reaped there).
    scoped_refptr<base::SingleThreadTaskRunner> reader = connection_->reader();
    reader->ReleaseSoon(FROM_HERE, std::move(connection_));
  }
  if (!started_) {
    return;
  }
  // However it ended (HelperFailureCount: a helper that ran a while starts
  // the count afresh).
  const int failures = failures_.OnEnded(NowMs());
  LOG(WARNING) << "[MoltVoice] speech helper ended: " << why;
  if (reason == StopReason::kStartFailed || failures_.exhausted()) {
    started_ = false;
    delegate_->OnListenerGaveUp(why);
    return;
  }
  const size_t delay = std::min<size_t>(static_cast<size_t>(failures - 1),
                                        std::size(kRestartDelays) - 1);
  delegate_->OnListenerLost(reason, why, /*restarting=*/true);
  restart_timer_.Start(
      FROM_HERE, kRestartDelays[delay],
      base::BindOnce(&VoiceListener::Launch, base::Unretained(this)));
}

void VoiceListener::SendConfig(const HelperConfig& config) {
  Send(EncodeHelperFrame(HelperFrameType::kConfig, EncodeConfigJson(config)));
}

void VoiceListener::SendReset() {
  if (!connection_) {
    return;
  }
  ++resets_sent_;
  Send(EncodeHelperFrame(HelperFrameType::kReset, std::string_view()));
}

void VoiceListener::Send(std::string frame) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!connection_ || frame.empty()) {
    return;
  }
  connection_->writer()->PostTask(
      FROM_HERE, base::BindOnce(&Connection::Write, connection_,
                                std::move(frame), /*counted=*/false));
}

}  // namespace voice
}  // namespace molt_ai
