// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_speech_bundled.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <string>
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
#include "content/public/browser/audio_service.h"
#include "media/audio/audio_device_description.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_glitch_info.h"
#include "media/base/audio_parameters.h"
#include "media/base/audio_renderer_sink.h"
#include "media/base/channel_layout.h"
#include "media/mojo/mojom/audio_stream_factory.mojom.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "services/audio/public/cpp/output_device.h"

namespace molt_ai {
namespace voice {

namespace {

std::optional<BundledVoice::Paths>& PathsForTesting() {
  static base::NoDestructor<std::optional<BundledVoice::Paths>> paths;
  return *paths;
}

// Where scripts/bundle-speech.sh --parts voice puts the helper and voice.
base::FilePath BundledVoiceDir() {
#if BUILDFLAG(IS_MAC)
  base::FilePath exe;
  if (!base::PathService::Get(base::FILE_EXE, &exe)) {
    return base::FilePath();
  }
  // MoltBrowser.app/Contents/MacOS/<exe> -> .../Contents/Resources/voice
  return exe.DirName().DirName().AppendASCII("Resources").AppendASCII("voice");
#else
  base::FilePath exe_dir;
  if (!base::PathService::Get(base::DIR_EXE, &exe_dir)) {
    return base::FilePath();
  }
  return exe_dir.AppendASCII("voice");
#endif
}

base::TaskTraits HelperThreadTraits() {
  // A dedicated thread per helper: reading and writing block for as long as
  // it runs. It must not hold up the browser's shutdown.
  return {base::MayBlock(), base::TaskPriority::USER_BLOCKING,
          base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN};
}

std::string DescribeExit(std::optional<int> exit_code) {
  if (!exit_code) {
    return "it stopped answering";
  }
  if (*exit_code == -1) {
    return "it crashed";
  }
  if (*exit_code == -2) {
    return "it couldn't be started confined";
  }
  return "it exited (code " + std::to_string(*exit_code) + ")";
}

mojo::PendingRemote<media::mojom::AudioStreamFactory> StreamFactory() {
  mojo::PendingRemote<media::mojom::AudioStreamFactory> factory;
  content::GetAudioServiceStreamFactoryBinder().Run(
      factory.InitWithNewPipeAndPassReceiver());
  return factory;
}

}  // namespace

// The samples the player has rendered (or dropped): read on the UI thread,
// written on the player's sequence and the audio thread.
class BundledVoice::Counter : public base::RefCountedThreadSafe<Counter> {
 public:
  Counter() = default;
  Counter(const Counter&) = delete;
  Counter& operator=(const Counter&) = delete;

  void Add(uint64_t samples) { value_.fetch_add(samples); }
  uint64_t value() const { return value_.load(); }

 private:
  friend class base::RefCountedThreadSafe<Counter>;
  ~Counter() = default;

  std::atomic<uint64_t> value_{0};
};

// Plays what the helper said through an audio::OutputDevice on the default
// output device (so the echo canceller has it as reference). Its own
// sequence; Render() runs on the audio thread.
class BundledVoice::Player : public media::AudioRendererSink::RenderCallback {
 public:
  explicit Player(scoped_refptr<Counter> played) : played_(std::move(played)) {}
  Player(const Player&) = delete;
  Player& operator=(const Player&) = delete;
  ~Player() override = default;

  void Push(int sample_rate, std::vector<int16_t> samples) {
    if (samples.empty()) {
      return;
    }
    if (device_ && sample_rate != sample_rate_) {
      Flush();
      device_.reset();
      playing_ = false;
    }
    {
      base::AutoLock lock(lock_);
      if (read_ > 0 && read_ * 2 >= buffer_.size()) {
        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + static_cast<ptrdiff_t>(read_));
        read_ = 0;
      }
      buffer_.insert(buffer_.end(), samples.begin(), samples.end());
    }
    if (!device_) {
      sample_rate_ = sample_rate;
      // 20 ms buffers.
      device_ = std::make_unique<audio::OutputDevice>(
          StreamFactory(),
          media::AudioParameters(media::AudioParameters::AUDIO_PCM_LOW_LATENCY,
                                 media::ChannelLayoutConfig::Mono(),
                                 sample_rate, sample_rate / 50),
          this, media::AudioDeviceDescription::kDefaultDeviceId);
    }
    if (!playing_) {
      device_->Play();
      playing_ = true;
    }
    idle_timer_.Start(
        FROM_HERE, kIdleCheck,
        base::BindRepeating(&Player::PauseWhenIdle, base::Unretained(this)));
  }

  // Drops what waits: silence now.
  void Flush() {
    base::AutoLock lock(lock_);
    played_->Add(buffer_.size() - read_);
    buffer_.clear();
    read_ = 0;
  }

  // media::AudioRendererSink::RenderCallback (the audio thread):
  int Render(base::TimeDelta delay,
             base::TimeTicks delay_timestamp,
             const media::AudioGlitchInfo& glitch_info,
             media::AudioBus* dest) override {
    base::span<float> out = dest->channel(0);
    size_t taken = 0;
    {
      base::AutoLock lock(lock_);
      const size_t available = buffer_.size() - read_;
      taken = std::min(available, out.size());
      for (size_t i = 0; i < taken; ++i) {
        out[i] = static_cast<float>(buffer_[read_ + i]) / 32768.0f;
      }
      read_ += taken;
    }
    for (size_t i = taken; i < out.size(); ++i) {
      out[i] = 0.0f;
    }
    if (taken) {
      played_->Add(taken);
    }
    return static_cast<int>(out.size());
  }

  void OnRenderError() override {
    // BundledVoice notices the audio stopped moving.
  }

 private:
  static constexpr base::TimeDelta kIdleCheck = base::Milliseconds(500);

  // Pauses the stream once everything played.
  void PauseWhenIdle() {
    bool empty = false;
    {
      base::AutoLock lock(lock_);
      empty = read_ == buffer_.size();
    }
    if (empty && playing_ && device_) {
      device_->Pause();
      playing_ = false;
      idle_timer_.Stop();
    }
  }

  const scoped_refptr<Counter> played_;
  // What Render() reads; destroyed after the device (whose audio thread
  // calls Render() until it goes).
  base::Lock lock_;
  std::vector<int16_t> buffer_ GUARDED_BY(lock_);
  size_t read_ GUARDED_BY(lock_) = 0;
  int sample_rate_ = 0;
  bool playing_ = false;
  std::unique_ptr<audio::OutputDevice> device_;
  // Destroyed first: it calls into the device.
  base::RepeatingTimer idle_timer_;
};

// One helper process: its threads, and the process itself. The last
// reference goes on one of its own threads (ResidentHelper's destructor
// blocks while it reaps the process).
class BundledVoice::Connection
    : public base::RefCountedThreadSafe<BundledVoice::Connection> {
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

  // On the reader thread: starts the helper, then reads its frames until it
  // ends. Everything goes to |voice| on |owner|, tagged with |generation|.
  void Run(base::CommandLine cmd,
           ResidentHelperPolicy policy,
           scoped_refptr<base::SequencedTaskRunner> owner,
           base::WeakPtr<BundledVoice> voice,
           int generation) {
    std::string error;
    std::unique_ptr<ResidentHelper> started =
        ResidentHelper::Start(cmd, policy, &error);
    if (!started) {
      LOG(WARNING) << "[MoltVoice] voice helper: " << error;
      owner->PostTask(FROM_HERE, base::BindOnce(&BundledVoice::OnEnded, voice,
                                                generation, std::string(), -2));
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
                    base::BindOnce(&BundledVoice::OnLaunched, voice, generation,
                                   helper->confinement()));

    molt_voice::FrameReader reader(
        molt_voice::FrameReader::Direction::kFromHelper);
    std::vector<uint8_t> buffer(molt_voice::kFrameHeaderBytes +
                                molt_voice::kMaxFramePayloadBytes);
    std::string why;
    bool broken = false;
    while (!broken) {
      const int bytes = helper->Read(buffer);
      if (bytes <= 0) {
        break;
      }
      reader.Append(buffer.data(), static_cast<size_t>(bytes));
      while (true) {
        molt_voice::Frame frame;
        const molt_voice::FrameReader::Status status =
            reader.Next(&frame, &why);
        if (status == molt_voice::FrameReader::Status::kNeedMore) {
          break;
        }
        if (status == molt_voice::FrameReader::Status::kError) {
          why = "it wrote a frame that isn't the protocol (" + why + ")";
          broken = true;
          break;
        }
        owner->PostTask(
            FROM_HERE, base::BindOnce(&BundledVoice::OnFrame, voice, generation,
                                      std::move(frame)));
      }
    }
    if (broken) {
      helper->Kill();
    }
    std::optional<int> exit_code = helper->WaitForExit(base::Seconds(3));
    if (!exit_code) {
      helper->Kill();
      exit_code = helper->WaitForExit(base::Seconds(2));
    }
    if (!broken) {
      why = DescribeExit(exit_code);
    }
    owner->PostTask(FROM_HERE,
                    base::BindOnce(&BundledVoice::OnEnded, voice, generation,
                                   std::move(why), exit_code.value_or(-1)));
  }

  // On the writer thread.
  void Write(std::string frame) {
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
                   base::WeakPtr<BundledVoice> voice,
                   int generation) {
    ResidentHelper* helper = Helper();
    std::string why;
    if (helper && !helper->CheckLimits(base::TimeTicks::Now(), &why)) {
      owner->PostTask(FROM_HERE,
                      base::BindOnce(&BundledVoice::OnLimitsChecked, voice,
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
};

// static
std::optional<BundledVoice::Paths> BundledVoice::FindBundled(std::string* why) {
  if (PathsForTesting()) {
    return PathsForTesting();
  }
  const base::FilePath dir = BundledVoiceDir();
#if BUILDFLAG(IS_WIN)
  const std::vector<base::FilePath> names = {
      base::FilePath(FILE_PATH_LITERAL("molt-voice.exe"))};
#else
  const std::vector<base::FilePath> names = {
      base::FilePath(FILE_PATH_LITERAL("molt-voice"))};
#endif
  // The bundled copy only: no system path, no PATH (helper_binary.h).
  const HelperBinary found =
      FindHelperBinary(dir, names, /*linux_system_paths=*/{});
  if (!found.found()) {
    *why = "voice mode's own voice isn't installed";
    return std::nullopt;
  }
  const base::FilePath models = dir.AppendASCII("models");
  if (!base::DirectoryExists(models)) {
    *why = "voice mode's voice model isn't installed";
    return std::nullopt;
  }
  return Paths{found.path, models};
}

// static
void BundledVoice::SetPathsForTesting(std::optional<Paths> paths) {
  PathsForTesting() = std::move(paths);
}

BundledVoice::BundledVoice(Delegate* delegate, Paths paths)
    : delegate_(delegate),
      paths_(std::move(paths)),
      played_(base::MakeRefCounted<Counter>()),
      player_(base::ThreadPool::CreateSequencedTaskRunner(
                  {base::MayBlock(), base::TaskPriority::USER_BLOCKING,
                   base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN}),
              played_) {}

BundledVoice::~BundledVoice() {
  Stop();
}

void BundledVoice::Start() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (started_) {
    return;
  }
  started_ = true;
  failures_ = 0;
  Launch();
}

void BundledVoice::Stop() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  started_ = false;
  restart_timer_.Stop();
  Cancel();
  if (!connection_) {
    return;
  }
  // SHUTDOWN, end of file, and a kill if it lingers.
  Send(molt_voice::EncodeShutdown());
  connection_->writer()->PostTask(
      FROM_HERE, base::BindOnce(&Connection::CloseInput, connection_));
  base::ThreadPool::PostDelayedTask(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(&Connection::Kill, connection_), kShutdownGrace);
  EndConnection();
}

void BundledVoice::Launch() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  ++generation_;
  ready_ = false;
  checker_.Reset();
  connection_ = base::MakeRefCounted<Connection>();
  launched_at_ = base::TimeTicks::Now();

  // The helper gets the path of its model directory, and nothing else
  // (VOICE_HELPER_PROTOCOL.md §1).
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
      base::BindRepeating(&BundledVoice::OnWatchdog, base::Unretained(this)));
}

void BundledVoice::OnLaunched(int generation, std::string confinement) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (generation == generation_) {
    confinement_ = std::move(confinement);
  }
}

uint32_t BundledVoice::Speak(const std::string& text, int rate_percent) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const uint32_t id = next_id_++;
  if (!ready_ || !connection_) {
    return id;
  }
  Send(molt_voice::EncodeSpeak(id, rate_percent, text));
  checker_.Sent(id);
  Request request;
  request.id = id;
  requests_.push_back(request);
  if (!playback_poll_.IsRunning()) {
    played_moved_at_ = base::TimeTicks::Now();
    playback_poll_.Start(FROM_HERE, kPlaybackPoll,
                         base::BindRepeating(&BundledVoice::OnPlaybackPoll,
                                             base::Unretained(this)));
  }
  return id;
}

void BundledVoice::Cancel() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (requests_.empty()) {
    return;
  }
  requests_.clear();
  if (connection_ && checker_.outstanding() > 0) {
    Send(molt_voice::EncodeCancel());
  }
  // Everything handed over so far is dropped: the counter catches up with
  // |pushed_| (the flush runs after the pushes posted before it).
  player_.AsyncCall(&Player::Flush);
}

void BundledVoice::OnFrame(int generation, molt_voice::Frame frame) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (generation != generation_ || !connection_) {
    return;
  }
  VoiceEvent event;
  std::string why;
  if (!checker_.Accept(frame, &event, &why)) {
    Fail(why);
    return;
  }
  switch (event.type) {
    case VoiceEventType::kReady: {
      ready_ = true;
      ready_at_ = base::TimeTicks::Now();
      LOG(INFO) << "[MoltVoice] voice helper ready: " << event.ready.voice
                << ", " << event.ready.sample_rate << " Hz, "
                << (ready_at_ - launched_at_).InMilliseconds() << " ms, "
                << confinement_;
      base::WeakPtr<BundledVoice> self = weak_factory_.GetWeakPtr();
      delegate_->OnBundledVoiceReady();
      return;
    }
    case VoiceEventType::kAudio: {
      // A cancelled request's audio is dropped.
      const bool wanted =
          std::any_of(requests_.begin(), requests_.end(),
                      [&event](const Request& r) { return r.id == event.id; });
      if (!wanted || event.samples.empty()) {
        return;
      }
      if (last_played_ >= pushed_) {
        played_moved_at_ = base::TimeTicks::Now();
      }
      pushed_ += event.samples.size();
      player_.AsyncCall(&Player::Push)
          .WithArgs(checker_.sample_rate(), std::move(event.samples));
      return;
    }
    case VoiceEventType::kDone: {
      for (Request& request : requests_) {
        if (request.id == event.id) {
          request.done = true;
          request.end_sample = pushed_;
          DVLOG(1) << "[MoltVoice] request " << event.id << ": status "
                   << static_cast<int>(event.done.status) << ", audio "
                   << event.done.audio_ms << " ms, first audio "
                   << event.done.first_audio_ms << " ms, synthesis "
                   << event.done.synth_ms << " ms";
        }
      }
      OnPlaybackPoll();
      return;
    }
    case VoiceEventType::kError:
      LOG(WARNING) << "[MoltVoice] voice helper error "
                   << static_cast<int>(event.code) << ": " << event.message;
      return;
  }
}

void BundledVoice::OnPlaybackPoll() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const uint64_t played = played_->value();
  const base::TimeTicks now = base::TimeTicks::Now();
  if (played != last_played_) {
    last_played_ = played;
    played_moved_at_ = now;
  }
  base::WeakPtr<BundledVoice> self = weak_factory_.GetWeakPtr();
  while (self && !requests_.empty() && requests_.front().done &&
         played >= requests_.front().end_sample) {
    const uint32_t id = requests_.front().id;
    requests_.pop_front();
    delegate_->OnBundledVoiceFinished(id);
  }
  if (!self) {
    return;
  }
  // Audio waits and doesn't move: the output device failed.
  if (pushed_ > played && now - played_moved_at_ >= kOutputStallTimeout) {
    Fail("its audio stopped playing");
    return;
  }
  if (requests_.empty() && pushed_ <= played) {
    playback_poll_.Stop();
  }
}

void BundledVoice::OnEnded(int generation, std::string why, int exit_code) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (generation != generation_) {
    return;
  }
  Fail(why.empty() ? DescribeExit(exit_code) : why);
}

void BundledVoice::OnLimitsChecked(int generation, std::string why) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (generation != generation_ || !connection_) {
    return;
  }
  Fail(why);
}

void BundledVoice::OnWatchdog() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!connection_) {
    return;
  }
  if (!ready_ && base::TimeTicks::Now() - launched_at_ >= kReadyTimeout) {
    Fail("it didn't get ready in " + std::to_string(kReadyTimeout.InSeconds()) +
         " s");
    return;
  }
  connection_->writer()->PostTask(
      FROM_HERE, base::BindOnce(&Connection::CheckLimits, connection_,
                                base::SequencedTaskRunner::GetCurrentDefault(),
                                weak_factory_.GetWeakPtr(), generation_));
}

void BundledVoice::Fail(const std::string& why) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  LOG(WARNING) << "[MoltVoice] voice helper ended: " << why;
  if (connection_) {
    connection_->Kill();
  }
  const bool was_stable = ready_ && !ready_at_.is_null() &&
                          base::TimeTicks::Now() - ready_at_ >= kStableRunTime;
  EndConnection();
  std::vector<uint32_t> unfinished;
  for (const Request& request : requests_) {
    unfinished.push_back(request.id);
  }
  requests_.clear();
  player_.AsyncCall(&Player::Flush);
  if (!started_) {
    return;
  }
  if (was_stable) {
    failures_ = 0;
  }
  ++failures_;
  const bool gave_up = failures_ >= kMaxFailures;
  if (gave_up) {
    started_ = false;
  } else {
    restart_timer_.Start(
        FROM_HERE, kRestartDelay,
        base::BindOnce(&BundledVoice::Launch, base::Unretained(this)));
  }
  delegate_->OnBundledVoiceLost(std::move(unfinished), gave_up, why);
}

void BundledVoice::EndConnection() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Frames of this helper still on their way are dropped.
  ++generation_;
  ready_ = false;
  checker_.Reset();
  watchdog_.Stop();
  if (connection_) {
    // Its threads let it go: never this one (the helper is reaped there).
    scoped_refptr<base::SingleThreadTaskRunner> reader = connection_->reader();
    reader->ReleaseSoon(FROM_HERE, std::move(connection_));
  }
}

void BundledVoice::Send(std::string frame) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!connection_ || frame.empty()) {
    return;
  }
  connection_->writer()->PostTask(
      FROM_HERE,
      base::BindOnce(&Connection::Write, connection_, std::move(frame)));
}

}  // namespace voice
}  // namespace molt_ai
