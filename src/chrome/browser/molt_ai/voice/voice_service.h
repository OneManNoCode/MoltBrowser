// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// VoiceService — local speech-to-text via whisper.cpp.
//
// The user records audio in the side-panel chat via WebAudio. The
// audio (16-kHz mono WAV bytes) is shipped to this service over an
// IPC, which writes it to a temp file, shells out to a bundled (or
// system-installed) `whisper-cli` binary, and returns the
// transcription as plaintext.
//
// Resolution order for the binary:
//   1. MoltBrowser.app/Contents/Resources/whisper/whisper-cli
//      (bundled via scripts/bundle-whisper.sh)
//   2. /opt/homebrew/bin/whisper-cli, /usr/local/bin/whisper-cli,
//      /usr/bin/whisper-cli  (system install fallback)
// Same fall-through pattern as TorManager.
//
// Privacy: all audio stays on the user's machine. We write to a
// dedicated temp file under TMPDIR with a random suffix, delete it
// after transcription, and never network anything.
//
// Threading: public methods are UI-thread. Actual subprocess invoke
// runs on a MayBlock thread-pool task; completion bounces back.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_SERVICE_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_SERVICE_H_

#include <string>

#include "base/files/file_path.h"
#include "base/functional/callback.h"

namespace base {
template <typename T>
class NoDestructor;
}  // namespace base

namespace molt_ai {
namespace voice {

struct TranscribeResult {
  TranscribeResult();
  TranscribeResult(const TranscribeResult&);
  TranscribeResult(TranscribeResult&&);
  TranscribeResult& operator=(const TranscribeResult&);
  TranscribeResult& operator=(TranscribeResult&&);
  ~TranscribeResult();

  bool success = false;
  std::string text;
  std::string error;        // human-readable on failure
  std::string binary_path;  // which whisper we used
  std::string binary_source;// "bundled" | "system" | "none"
  int duration_ms = 0;
};

class VoiceService {
 public:
  static VoiceService* Get();

  VoiceService(const VoiceService&) = delete;
  VoiceService& operator=(const VoiceService&) = delete;

  // Find the whisper binary. Empty FilePath if not available.
  base::FilePath ResolveWhisperBinary() const;
  bool IsUsingBundledWhisper() const;
  base::FilePath GetBundledWhisperDir() const;
  // Path of the model file we hand whisper-cli. Resolved to the
  // bundled ggml-tiny.en.bin under Resources/whisper/.
  base::FilePath GetBundledModelPath() const;

  // Transcribe |wav_bytes| (16-kHz mono WAV format) and return the
  // result via |on_done| on the UI thread.
  void Transcribe(std::string wav_bytes,
                  base::OnceCallback<void(TranscribeResult)> on_done);

 private:
  friend class base::NoDestructor<VoiceService>;
  VoiceService();
  ~VoiceService();

  // Expected path of the bundled whisper binary next to the bundle dir
  // (whisper-cli.exe/whisper.exe on Windows, whisper-cli elsewhere).
  // Empty FilePath if there is no bundle dir on this platform.
  base::FilePath BundledWhisperPath() const;
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_SERVICE_H_
