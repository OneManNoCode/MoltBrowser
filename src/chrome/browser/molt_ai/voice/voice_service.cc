// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_service.h"

#include "build/build_config.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "chrome/browser/molt_ai/common/helper_binary.h"
#include "chrome/browser/molt_ai/common/helper_output.h"
#include "chrome/browser/molt_ai/common/helper_sandbox.h"
#include "chrome/browser/molt_ai/voice/recorded_wav.h"

namespace molt_ai {
namespace voice {

TranscribeResult::TranscribeResult() = default;
TranscribeResult::TranscribeResult(const TranscribeResult&) = default;
TranscribeResult::TranscribeResult(TranscribeResult&&) = default;
TranscribeResult& TranscribeResult::operator=(const TranscribeResult&) =
    default;
TranscribeResult& TranscribeResult::operator=(TranscribeResult&&) = default;
TranscribeResult::~TranscribeResult() = default;

namespace {

// Linux only: a distribution whisper.cpp package's whisper-cli, accepted only
// if root owns it and every directory above it (helper_binary.h). There is no
// Homebrew / /usr/local / PATH fallback any more (audit S18): those are
// writable without root, and whisper is fed microphone audio. Not
// /usr/bin/whisper: on Linux that name is usually OpenAI's Python CLI, which
// doesn't take whisper.cpp's arguments.
std::vector<std::string> SystemWhisperPaths() {
  return {"/usr/bin/whisper-cli"};
}

// The deadline for one transcription. Recordings are at most ~3 minutes (8
// MiB of base64 WAV, HandleTranscribeAudio); whisper's tiny model runs
// faster than real time even on slow CPUs.
constexpr base::TimeDelta kWhisperTimeout = base::Seconds(300);

// whisper's stdout and stderr kept per run, and the most of its .txt read
// back: an hour of speech is well under 1 MiB of text.
constexpr size_t kMaxWhisperOutputBytes = 1u << 20;

// Sync, blocking: write WAV to a temp file, run whisper-cli, capture
// stdout, delete the temp file. Returns the transcription text or
// an empty string on failure (with |err| set).
TranscribeResult RunWhisperBlocking(std::string wav_bytes,
                                     base::FilePath bin,
                                     base::FilePath model) {
  TranscribeResult r;
  r.binary_path = bin.AsUTF8Unsafe();
  const base::TimeTicks t0 = base::TimeTicks::Now();

  if (bin.value().empty()) {
    r.error = "no whisper binary available";
    return r;
  }
  // |bin| is the canonical path FindHelperBinary accepted (never a bare name
  // for a PATH search).
  if (!bin.IsAbsolute() || !base::PathExists(bin)) {
    r.error = "whisper binary not found at " + bin.AsUTF8Unsafe();
    return r;
  }
  if (wav_bytes.empty()) {
    r.error = "no audio bytes";
    return r;
  }
  // The page's recording is checked here, and whisper-cli reads a WAV the
  // browser writes itself (recorded_wav.h): never what the page sent.
  std::string_view samples;
  const RecordedWavError wav_error = ParseRecordedWav(wav_bytes, &samples);
  if (wav_error != RecordedWavError::kNone) {
    r.error = std::string(DescribeRecordedWavError(wav_error));
    return r;
  }
  const std::string canonical_wav = CanonicalWav(samples);

  // Create a private 0700 scratch directory so the audio + the .txt
  // output whisper-cli writes alongside it can't be raced by a local-
  // user attacker placing symlinks. Code-review MEDIUM #5+#6.
  base::FilePath scratch_dir;
  if (!base::CreateNewTempDirectory(FILE_PATH_LITERAL("molt_voice"),
                                     &scratch_dir)) {
    r.error = "could not create scratch directory";
    return r;
  }
  base::FilePath wav_path = scratch_dir.AppendASCII("input.wav");
  if (!base::WriteFile(wav_path, canonical_wav)) {
    r.error = "could not write temp audio";
    base::DeletePathRecursively(scratch_dir);
    return r;
  }

  // Build the command line. -nt = no timestamps (clean text output),
  // -np = no prints (suppress chatter on stderr), -l auto = auto
  // language detect, -of <stem> writes <stem>.txt next to the audio.
  // -ng = CPU only: the sandbox gives whisper no GPU (Metal, /dev/dri), so
  // don't let it probe one.
  base::FilePath out_stem = wav_path.RemoveExtension();
  base::CommandLine cmd(bin);
  if (!model.value().empty()) {
    cmd.AppendArg("-m");
    cmd.AppendArgPath(model);
  }
  cmd.AppendArg("-nt");
  cmd.AppendArg("-np");
  cmd.AppendArg("-ng");
  cmd.AppendArg("-l");
  cmd.AppendArg("auto");
  cmd.AppendArg("-otxt");
  cmd.AppendArg("-of");
  cmd.AppendArgPath(out_stem);
  cmd.AppendArg("-f");
  cmd.AppendArgPath(wav_path);

  // Sandboxed (helper_sandbox.h): it reads its own directory (and model)
  // and the audio, writes only the scratch directory, has no network, and
  // is killed after kWhisperTimeout.
  molt_ai::HelperSandboxPolicy policy;
  policy.read_only_paths.push_back(bin.DirName());
  if (!model.value().empty()) {
    policy.read_only_paths.push_back(model);
  }
  policy.read_only_paths.push_back(wav_path);
  policy.writable_dir = scratch_dir;
  const molt_ai::HelperRunResult run = molt_ai::RunSandboxedHelper(
      cmd, policy, kWhisperTimeout, kMaxWhisperOutputBytes);
  const std::string failure = molt_ai::DescribeHelperRunFailure(
      run, "whisper", "failed to launch whisper subprocess");
  if (!failure.empty()) {
    r.error = failure;
    base::DeletePathRecursively(scratch_dir);
    return r;
  }

  // Read the transcription file whisper-cli wrote next to the audio (only
  // if it is a plain file it made).
  base::FilePath txt_path = out_stem.AddExtensionASCII(".txt");
  std::string transcription;
  if (!molt_ai::ReadHelperOutputFile(txt_path, kMaxWhisperOutputBytes,
                                     &transcription)) {
    // Older whisper builds dump stdout directly. Use that as fallback.
    transcription = run.output;
  }
  base::DeletePathRecursively(scratch_dir);

  // Trim leading/trailing whitespace.
  std::string trimmed;
  base::TrimWhitespaceASCII(transcription, base::TRIM_ALL, &trimmed);
  r.success = !trimmed.empty();
  // whisper's output is untrusted bytes: valid UTF-8 for base::Value, and
  // bounded (an hour of speech is well under 1 MiB of text).
  r.text = SanitizeHelperText(trimmed, 1u << 20);
  r.duration_ms =
      static_cast<int>((base::TimeTicks::Now() - t0).InMilliseconds());
  if (!r.success && r.error.empty()) {
    r.error = "transcription empty (silence or model couldn't decode)";
  }
  return r;
}

}  // namespace

// static
VoiceService* VoiceService::Get() {
  static base::NoDestructor<VoiceService> instance;
  return instance.get();
}

VoiceService::VoiceService() = default;
VoiceService::~VoiceService() = default;

base::FilePath VoiceService::GetBundledWhisperDir() const {
#if BUILDFLAG(IS_MAC)
  base::FilePath exe;
  if (!base::PathService::Get(base::FILE_EXE, &exe)) return base::FilePath();
  // MoltBrowser.app/Contents/MacOS/<exe>  →  .../Contents/Resources/whisper
  return exe.DirName().DirName().AppendASCII("Resources")
      .AppendASCII("whisper");
#else
  // Windows and Linux ship a flat install dir, so the bundle lives next to
  // the executable: <DIR_EXE>/whisper/ (whisper-cli[.exe] +
  // ggml-tiny.en.bin).
  base::FilePath exe_dir;
  if (!base::PathService::Get(base::DIR_EXE, &exe_dir)) return base::FilePath();
  return exe_dir.Append(FILE_PATH_LITERAL("whisper"));
#endif
}

base::FilePath VoiceService::GetBundledModelPath() const {
  base::FilePath dir = GetBundledWhisperDir();
  if (dir.value().empty()) return base::FilePath();
  // bundle-whisper.sh drops a ggml-tiny.en.bin alongside the binary.
  return dir.AppendASCII("ggml-tiny.en.bin");
}

molt_ai::HelperBinary VoiceService::FindWhisper() const {
#if BUILDFLAG(IS_WIN)
  // whisper.cpp's CLI is `whisper-cli.exe`; older builds shipped it as
  // `whisper.exe`. Prefer the current name, fall back to the legacy one.
  const std::vector<base::FilePath> names = {
      base::FilePath(FILE_PATH_LITERAL("whisper-cli.exe")),
      base::FilePath(FILE_PATH_LITERAL("whisper.exe"))};
#else
  const std::vector<base::FilePath> names = {
      base::FilePath(FILE_PATH_LITERAL("whisper-cli"))};
#endif
  return molt_ai::FindHelperBinary(GetBundledWhisperDir(), names,
                                   SystemWhisperPaths());
}

base::FilePath VoiceService::ResolveWhisperBinary() const {
  return FindWhisper().path;
}

bool VoiceService::IsUsingBundledWhisper() const {
  return FindWhisper().bundled;
}

void VoiceService::Transcribe(
    std::string wav_bytes,
    base::OnceCallback<void(TranscribeResult)> on_done) {
  const molt_ai::HelperBinary found = FindWhisper();
  if (!found.found()) {
    LOG(WARNING) << "[MoltVoice] no usable whisper-cli: " << found.error;
  }
  const base::FilePath& bin = found.path;
  const bool is_bundled = found.bundled;
  // Only pass model if we're using the bundled binary — system
  // installs have their own model path conventions and our bundled
  // ggml-tiny.en.bin path won't exist for them.
  base::FilePath model =
      is_bundled ? GetBundledModelPath() : base::FilePath();
  std::string src =
      bin.value().empty() ? "none" : (is_bundled ? "bundled" : "system");
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&RunWhisperBlocking, std::move(wav_bytes), bin, model),
      base::BindOnce(
          [](std::string src,
             base::OnceCallback<void(TranscribeResult)> cb,
             TranscribeResult r) {
            r.binary_source = src;
            std::move(cb).Run(std::move(r));
          },
          src, std::move(on_done)));
}

}  // namespace voice
}  // namespace molt_ai
