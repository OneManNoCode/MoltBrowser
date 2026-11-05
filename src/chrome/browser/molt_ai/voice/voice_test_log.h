// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Voice mode's test log, for the end-to-end spoken test kit
// (tools/voice_e2e/, docs/VOICE_E2E.md): with
//
//   --molt-voice-test-log=<absolute path>
//
// on the browser's command line, each window's voice loop appends one JSON
// line per event to that file (voice_test_log_lines.h has the lines): what
// reached the loop (typed or heard, and the command it was understood as),
// what voice mode said, its activity and the microphone, and the prompts
// and questions that opened and closed. The kit reads it to tell what was
// misheard from what went wrong after.
//
// A test switch, as Chromium's own (--use-fake-ui-for-media-stream,
// --log-net-log): nothing is logged without it, and it is never set by
// the browser itself. Like --moltnet-tor-binary, it takes an absolute path
// only; the file is written only as a regular file, never through a link
// (molt_regular_file.h), and created readable by its owner alone. No line
// ever holds a confirmation word (only the twins of what is said with
// one), and MoltNet windows log nothing at all.
//
// One per window, made and fed by its VoiceController (an observer of
// what it says, and the loop's state at each caption change); the lines
// are made and written on one sequence for the whole browser, in order.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_TEST_LOG_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_TEST_LOG_H_

#include <memory>
#include <string>

#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/threading/sequence_bound.h"
#include "chrome/browser/molt_ai/voice/voice_controller.h"

namespace molt_ai {
namespace voice {

inline constexpr char kVoiceTestLogSwitch[] = "molt-voice-test-log";

class VoiceTestLog : public VoiceController::Observer {
 public:
  // The log the command line asks for: --molt-voice-test-log with an
  // absolute path. Empty without it (or with a relative path).
  static base::FilePath PathFromCommandLine();

  // A log for |controller|'s window, which observes it; null without the
  // switch, and for a MoltNet window.
  static std::unique_ptr<VoiceTestLog> MaybeCreate(VoiceController* controller,
                                                   bool moltnet);

  VoiceTestLog(const VoiceTestLog&) = delete;
  VoiceTestLog& operator=(const VoiceTestLog&) = delete;
  ~VoiceTestLog() override;

  // An utterance reached the loop (before it is planned), understood as
  // |kind|; |state| is the conversation it came in.
  void OnHeard(const std::string& text,
               VoiceCommandKind kind,
               const HeardEvidence& evidence,
               const ConversationState& state);

  // The page map an utterance was planned on (its items' labels, lists and
  // media), and whether the page's tree had a selection and its text could
  // be read.
  void OnMap(const PageMap& map,
             bool selection,
             bool selected_text,
             const ConversationState& state);

  // The loop's state as the caption bar shows it.
  void OnLoop(VoiceActivity activity,
              bool mic,
              bool waiting,
              const ConversationState& state);

  // VoiceController::Observer:
  void OnVoiceSpeech(const std::string& text) override;
  void OnVoiceControllerDestroying(VoiceController* controller) override;

 private:
  class Writer;

  VoiceTestLog(VoiceController* controller, const base::FilePath& path);

  raw_ptr<VoiceController> controller_;
  base::SequenceBound<Writer> writer_;
  // What OnLoop() last passed on: the same again posts nothing.
  VoiceActivity last_activity_ = VoiceActivity::kOff;
  bool last_mic_ = false;
  bool last_waiting_ = false;
  int last_prompt_id_ = 0;
  bool last_confirming_ = false;
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_TEST_LOG_H_
