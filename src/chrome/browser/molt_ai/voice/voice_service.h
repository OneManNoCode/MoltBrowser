// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// VoiceService — local speech-to-text via whisper.cpp.
//
// The user records audio in the side-panel chat via WebAudio. The
// audio (16-kHz mono WAV bytes) is shipped to this service over an
// IPC. It checks the recording (16 kHz mono 16-bit PCM, bounded length:
// recorded_wav.h), writes a WAV of its own with those samples to a temp
// file, shells out to a bundled (or system-installed) `whisper-cli`
// binary, and returns the transcription as plaintext. whisper-cli never
// reads the bytes the page sent.
//
// Resolution (FindHelperBinary, helper_binary.h — same as TorManager and
// OcrService):
//   1. the bundled copy (scripts/bundle-whisper.sh):
//      MoltBrowser.app/Contents/Resources/whisper/whisper-cli on macOS,
//      <install dir>/whisper/whisper-cli[.exe] on Linux and Windows;
//   2. Linux only: /usr/bin/whisper-cli, if root owns it and every
//      directory above it.
// No Homebrew, /usr/local or PATH fallback (audit S18).
//
// Privacy: all audio stays on the user's machine. We write to a
// dedicated temp file under TMPDIR with a random suffix, delete it
// after transcription, and never network anything.
//
// whisper-cli parses the audio (untrusted bytes) under the helper sandbox
// (helper_sandbox.h): it may read its own directory and model and the
// audio, write only a private per-call scratch directory, never use the
// network, and is killed after 300 s. It runs on the CPU (-ng): the
// sandbox gives it no GPU.
//
// Threading: public methods are UI-thread. Actual subprocess invoke
// runs on a MayBlock thread-pool task; completion bounces back.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_SERVICE_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_SERVICE_H_

#include <string>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "chrome/browser/molt_ai/common/helper_binary.h"

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

  // Transcribe |wav_bytes| (16-kHz mono 16-bit PCM WAV, as the chat page
  // records it; anything else fails with recorded_wav.h's reason) and
  // return the result via |on_done| on the UI thread.
  void Transcribe(std::string wav_bytes,
                  base::OnceCallback<void(TranscribeResult)> on_done);

 private:
  friend class base::NoDestructor<VoiceService>;
  VoiceService();
  ~VoiceService();

  // The whisper-cli to run (bundled, else on Linux a root-owned
  // /usr/bin/whisper-cli), or why there is none.
  molt_ai::HelperBinary FindWhisper() const;
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_SERVICE_H_
