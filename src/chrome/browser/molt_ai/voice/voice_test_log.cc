// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_test_log.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/logging.h"
#include "base/memory/ptr_util.h"
#include "base/no_destructor.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "chrome/browser/molt_ai/common/molt_regular_file.h"
#include "chrome/browser/molt_ai/voice/page_map.h"
#include "chrome/browser/molt_ai/voice/voice_command.h"
#include "chrome/browser/molt_ai/voice/voice_controller_logic.h"
#include "chrome/browser/molt_ai/voice/voice_gesture.h"
#include "chrome/browser/molt_ai/voice/voice_prompts.h"
#include "chrome/browser/molt_ai/voice/voice_test_log_lines.h"

namespace molt_ai {
namespace voice {

namespace {

int64_t NowMs() {
  return base::Time::Now().InMillisecondsSinceUnixEpoch();
}

std::string_view ActivityName(VoiceActivity activity) {
  switch (activity) {
    case VoiceActivity::kOff:
      return "off";
    case VoiceActivity::kListening:
      return "listening";
    case VoiceActivity::kThinking:
      return "thinking";
    case VoiceActivity::kSpeaking:
      return "speaking";
  }
  return "off";
}

// One sequence for every window's log, so its lines stay in order.
scoped_refptr<base::SequencedTaskRunner> LogTaskRunner() {
  static base::NoDestructor<scoped_refptr<base::SequencedTaskRunner>> runner(
      base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN}));
  return *runner;
}

}  // namespace

// Makes one window's lines and appends them, on LogTaskRunner().
class VoiceTestLog::Writer {
 public:
  Writer(base::FilePath path, int window)
      : path_(std::move(path)), lines_(window) {}
  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;
  ~Writer() = default;

  void Start(int64_t ms) {
    std::vector<std::string_view> kinds;
    kinds.reserve(kVoiceCommandKindCount);
    for (size_t i = 0; i < kVoiceCommandKindCount; ++i) {
      kinds.push_back(VoiceCommandKindName(static_cast<VoiceCommandKind>(i)));
    }
    Write(lines_.Start(ms, kinds));
  }

  void Heard(int64_t ms,
             const std::string& text,
             bool from_microphone,
             bool clean,
             const std::string& intent,
             const std::string& pending_word) {
    Write(lines_.Heard(ms, text, from_microphone, clean, intent, pending_word));
  }

  void Said(int64_t ms, const std::string& text) {
    Write(lines_.Said(ms, text));
  }

  void Map(int64_t ms,
           size_t items,
           size_t lists,
           size_t media,
           bool selection,
           bool selected_text,
           const std::vector<std::string>& labels,
           const std::string& pending_word) {
    TestLogLines::MapSummary map;
    map.items = items;
    map.lists = lists;
    map.media = media;
    map.selection = selection;
    map.selected_text = selected_text;
    map.labels = labels;
    Write(lines_.Map(ms, map, pending_word));
  }

  void Microphone(int64_t ms,
                  float peak,
                  int64_t samples,
                  const std::string& echo) {
    Write(lines_.Microphone(ms, peak, samples, echo));
  }

  void Changed(int64_t ms,
               const std::string& activity,
               bool mic,
               bool waiting,
               int prompt_id,
               const std::string& prompt_kind,
               bool confirming,
               const std::string& confirm_what) {
    TestLogState state;
    state.activity = activity;
    state.mic = mic;
    state.waiting = waiting;
    state.prompt_id = prompt_id;
    state.prompt_kind = prompt_kind;
    state.confirming = confirming;
    state.confirm_what = confirm_what;
    Write(lines_.Changed(ms, state));
  }

 private:
  void Write(const std::string& lines) {
    if (lines.empty() || failed_) {
      return;
    }
    // A regular file only, never through a link (molt_regular_file.h).
    if (!AppendToMoltFile(path_, lines)) {
      failed_ = true;
      LOG(WARNING) << "[MoltVoice] the voice test log can't be written";
    }
  }

  const base::FilePath path_;
  TestLogLines lines_;
  bool failed_ = false;
};

// static
base::FilePath VoiceTestLog::PathFromCommandLine() {
  if (!base::CommandLine::InitializedForCurrentProcess()) {
    return base::FilePath();
  }
  const base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();
  if (!command_line->HasSwitch(kVoiceTestLogSwitch)) {
    return base::FilePath();
  }
  const base::FilePath path =
      command_line->GetSwitchValuePath(kVoiceTestLogSwitch);
  if (path.empty() || !path.IsAbsolute() || path.ReferencesParent()) {
    LOG(WARNING) << "[MoltVoice] --" << kVoiceTestLogSwitch
                 << " takes an absolute path; no test log";
    return base::FilePath();
  }
  return path;
}

// static
std::unique_ptr<VoiceTestLog> VoiceTestLog::MaybeCreate(
    VoiceController* controller,
    bool moltnet) {
  if (!controller || moltnet) {
    return nullptr;
  }
  const base::FilePath path = PathFromCommandLine();
  if (path.empty()) {
    return nullptr;
  }
  std::unique_ptr<VoiceTestLog> log =
      base::WrapUnique(new VoiceTestLog(controller, path));
  controller->AddObserver(log.get());
  return log;
}

VoiceTestLog::VoiceTestLog(VoiceController* controller,
                           const base::FilePath& path)
    : controller_(controller) {
  // Windows are numbered in the order their voice mode was first used.
  static int next_window = 0;
  writer_ = base::SequenceBound<Writer>(LogTaskRunner(), path, ++next_window);
  writer_.AsyncCall(&Writer::Start).WithArgs(NowMs());
}

VoiceTestLog::~VoiceTestLog() {
  if (controller_) {
    controller_->RemoveObserver(this);
  }
}

void VoiceTestLog::OnHeard(const std::string& text,
                           VoiceCommandKind kind,
                           const HeardEvidence& evidence,
                           const ConversationState& state) {
  writer_.AsyncCall(&Writer::Heard)
      .WithArgs(NowMs(), text, evidence.from_microphone, evidence.clean,
                std::string(VoiceCommandKindName(kind)),
                state.prompt.id != 0 ? state.prompt.word : std::string());
}

void VoiceTestLog::OnMap(const PageMap& map,
                         bool selection,
                         bool selected_text,
                         const ConversationState& state) {
  std::vector<std::string> labels;
  for (const PageItem& item : map.items) {
    if (labels.size() >= kMaxTestLogLabels) {
      break;
    }
    labels.push_back(item.label);
  }
  writer_.AsyncCall(&Writer::Map)
      .WithArgs(NowMs(), map.items.size(), map.lists.size(), map.media.size(),
                selection, selected_text, std::move(labels),
                state.prompt.id != 0 ? state.prompt.word : std::string());
}

void VoiceTestLog::OnMicrophone(float peak,
                                int64_t samples,
                                std::string_view echo) {
  writer_.AsyncCall(&Writer::Microphone)
      .WithArgs(NowMs(), peak, samples, std::string(echo));
}

void VoiceTestLog::OnLoop(VoiceActivity activity,
                          bool mic,
                          bool waiting,
                          const ConversationState& state) {
  const bool prompt = state.prompt.id != 0;
  if (activity == last_activity_ && mic == last_mic_ &&
      waiting == last_waiting_ && state.prompt.id == last_prompt_id_ &&
      state.confirming == last_confirming_) {
    return;
  }
  last_activity_ = activity;
  last_mic_ = mic;
  last_waiting_ = waiting;
  last_prompt_id_ = state.prompt.id;
  last_confirming_ = state.confirming;
  writer_.AsyncCall(&Writer::Changed)
      .WithArgs(NowMs(), std::string(ActivityName(activity)), mic, waiting,
                state.prompt.id,
                prompt ? std::string(PromptKindName(state.prompt.kind))
                       : std::string(),
                state.confirming,
                state.confirming ? state.confirm_what : std::string());
}

void VoiceTestLog::OnVoiceSpeech(const std::string& text) {
  writer_.AsyncCall(&Writer::Said).WithArgs(NowMs(), text);
}

void VoiceTestLog::OnVoiceControllerDestroying(VoiceController* controller) {
  if (controller_) {
    controller_->RemoveObserver(this);
    controller_ = nullptr;
  }
}

}  // namespace voice
}  // namespace molt_ai
