// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_service.h"

#include <unistd.h>

#include <utility>

#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "build/build_config.h"

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

// System install paths we'll consult after the bundled binary. We
// look for both common names — Homebrew installs as `whisper-cli`
// (from whisper.cpp formula) but some users have the older `whisper`
// name still wired up.
const char* const kCandidatePaths[] = {
    "/opt/homebrew/bin/whisper-cli",
    "/usr/local/bin/whisper-cli",
    "/opt/homebrew/bin/whisper",
    "/usr/local/bin/whisper",
    "/usr/bin/whisper",
};

// Sync, blocking: write WAV to a temp file, run whisper-cli, capture
// stdout, delete the temp file. Returns the transcription text or
// an empty string on failure (with |err| set).
TranscribeResult RunWhisperBlocking(std::string wav_bytes,
                                     base::FilePath bin,
                                     base::FilePath model) {
  TranscribeResult r;
  r.binary_path = bin.value();
  const base::TimeTicks t0 = base::TimeTicks::Now();

  if (bin.value().empty()) {
    r.error = "no whisper binary available";
    return r;
  }
  if (!base::PathExists(bin)) {
    r.error = "whisper binary not found at " + bin.value();
    return r;
  }
  if (wav_bytes.empty()) {
    r.error = "no audio bytes";
    return r;
  }

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
  if (!base::WriteFile(wav_path, wav_bytes)) {
    r.error = "could not write temp audio";
    base::DeletePathRecursively(scratch_dir);
    return r;
  }

  // Build the command line. -nt = no timestamps (clean text output),
  // -np = no prints (suppress chatter on stderr), -l auto = auto
  // language detect, -of <stem> writes <stem>.txt next to the audio.
  base::FilePath out_stem = wav_path.RemoveExtension();
  base::CommandLine cmd(bin);
  if (!model.value().empty()) {
    cmd.AppendArg("-m");
    cmd.AppendArgPath(model);
  }
  cmd.AppendArg("-nt");
  cmd.AppendArg("-np");
  cmd.AppendArg("-l");
  cmd.AppendArg("auto");
  cmd.AppendArg("-otxt");
  cmd.AppendArg("-of");
  cmd.AppendArgPath(out_stem);
  cmd.AppendArg("-f");
  cmd.AppendArgPath(wav_path);

  std::string combined_output;
  int exit_code = -1;
  bool launched =
      base::GetAppOutputWithExitCode(cmd, &combined_output, &exit_code);
  if (!launched) {
    r.error = "failed to launch whisper subprocess";
    base::DeletePathRecursively(scratch_dir);
    return r;
  }
  if (exit_code != 0) {
    r.error = "whisper exit=" + std::to_string(exit_code) + ": " +
              combined_output.substr(0, 200);
    base::DeletePathRecursively(scratch_dir);
    return r;
  }

  // Read the transcription file whisper-cli wrote next to the audio.
  base::FilePath txt_path = out_stem.AddExtensionASCII(".txt");
  std::string transcription;
  if (base::PathExists(txt_path)) {
    base::ReadFileToString(txt_path, &transcription);
  } else {
    // Older whisper builds dump stdout directly. Use that as fallback.
    transcription = combined_output;
  }
  base::DeletePathRecursively(scratch_dir);

  // Trim leading/trailing whitespace.
  std::string trimmed;
  base::TrimWhitespaceASCII(transcription, base::TRIM_ALL, &trimmed);
  r.success = !trimmed.empty();
  r.text = trimmed;
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
  return base::FilePath();
#endif
}

base::FilePath VoiceService::GetBundledModelPath() const {
  base::FilePath dir = GetBundledWhisperDir();
  if (dir.value().empty()) return base::FilePath();
  // bundle-whisper.sh drops a ggml-tiny.en.bin alongside the binary.
  return dir.AppendASCII("ggml-tiny.en.bin");
}

base::FilePath VoiceService::ResolveWhisperBinary() const {
  base::FilePath bundled = GetBundledWhisperDir().AppendASCII("whisper-cli");
  if (!bundled.value().empty() && base::PathExists(bundled) &&
      access(bundled.value().c_str(), X_OK) == 0) {
    return bundled;
  }
  for (const char* p : kCandidatePaths) {
    base::FilePath fp(p);
    if (base::PathExists(fp) &&
        access(fp.value().c_str(), X_OK) == 0) {
      return fp;
    }
  }
  return base::FilePath();
}

bool VoiceService::IsUsingBundledWhisper() const {
  base::FilePath bundled = GetBundledWhisperDir().AppendASCII("whisper-cli");
  base::FilePath resolved = ResolveWhisperBinary();
  return !bundled.value().empty() && !resolved.value().empty() &&
         resolved.value() == bundled.value();
}

void VoiceService::Transcribe(
    std::string wav_bytes,
    base::OnceCallback<void(TranscribeResult)> on_done) {
  base::FilePath bin = ResolveWhisperBinary();
  bool is_bundled = IsUsingBundledWhisper();
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
