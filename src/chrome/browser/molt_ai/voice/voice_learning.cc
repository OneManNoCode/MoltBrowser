// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_learning.h"

#include <memory>
#include <utility>

#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/location.h"
#include "base/memory/ptr_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/threading/scoped_blocking_call.h"
#include "base/time/time.h"
#include "chrome/browser/molt_ai/automation/automation_security.h"
#include "chrome/browser/molt_ai/automation/automation_storage.h"
#include "chrome/browser/molt_ai/automation/automation_voice_steps.h"
#include "chrome/browser/molt_ai/voice/voice_routines.h"
#include "chrome/browser/profiles/profile.h"

namespace molt_ai {
namespace voice {

namespace {

using automation::AutomationSecurity;
using automation::AutomationStorage;
using automation::Script;

// The profile's user data key.
const char kUserDataKey[] = "MoltVoiceLearning";

// Changes are written this long after the first one.
constexpr base::TimeDelta kWriteDelay = base::Seconds(2);

int64_t NowUnix() {
  return base::Time::Now().ToTimeT();
}

struct ReadResult {
  LearnedData data;
};

ReadResult ReadLearningFile(const base::FilePath& path) {
  base::ScopedBlockingCall blocking(FROM_HERE, base::BlockingType::MAY_BLOCK);
  ReadResult result;
  std::string json;
  if (base::ReadFileToStringWithMaxSize(path, &json, kMaxLearningFileBytes)) {
    result.data = ParseLearning(json, NowUnix());
  }
  return result;
}

void WriteLearningFile(const base::FilePath& path, const std::string& json) {
  base::ScopedBlockingCall blocking(FROM_HERE, base::BlockingType::MAY_BLOCK);
  if (!json.empty()) {
    base::ImportantFileWriter::WriteFileAtomically(path, json);
  }
}

// The automation storage, at |root| when tests set one.
std::unique_ptr<AutomationStorage> Storage(const base::FilePath& root) {
  auto storage = std::make_unique<AutomationStorage>();
  if (!root.empty()) {
    storage->SetRootForTesting(root);
  }
  return storage;
}

// The voice routines in storage (blocking).
std::vector<Script> VoiceRoutines(AutomationStorage& storage) {
  std::vector<Script> routines;
  for (Script& script : storage.ListAll()) {
    if (automation::HasVoiceSteps(script)) {
      routines.push_back(std::move(script));
    }
  }
  return routines;
}

std::vector<RoutineSummary> ListVoiceRoutines(const base::FilePath& root) {
  base::ScopedBlockingCall blocking(FROM_HERE, base::BlockingType::MAY_BLOCK);
  std::vector<RoutineSummary> out;
  std::unique_ptr<AutomationStorage> storage = Storage(root);
  for (const Script& script : VoiceRoutines(*storage)) {
    RoutineSummary summary;
    summary.id = script.id;
    summary.name = script.name;
    summary.steps = static_cast<int>(script.steps.size());
    summary.needs_sites_confirmed = script.security.sites_unconfirmed;
    out.push_back(std::move(summary));
  }
  return out;
}

// The voice routine |name| names (or with id |id|), from |routines|.
const Script* FindRoutine(const std::vector<Script>& routines,
                          const std::string& name,
                          const std::string& id) {
  std::vector<std::string> names;
  for (const Script& script : routines) {
    if (!id.empty() && script.id == id) {
      return &script;
    }
    names.push_back(script.name);
  }
  if (name.empty()) {
    return nullptr;
  }
  const std::string* found = FindRoutineName(names, name);
  for (const Script& script : routines) {
    if (found && script.name == *found) {
      return &script;
    }
  }
  return nullptr;
}

std::pair<bool, std::string> SaveRoutineBlocking(
    const base::FilePath& root,
    const std::string& id,
    const std::string& name,
    const std::vector<RoutineStep>& steps) {
  base::ScopedBlockingCall blocking(FROM_HERE, base::BlockingType::MAY_BLOCK);
  std::unique_ptr<AutomationStorage> storage = Storage(root);
  if (FindRoutine(VoiceRoutines(*storage), name, std::string())) {
    return {false, "You already have a routine called " + name + "."};
  }
  BuiltRoutine built = BuildRoutine(id, name, steps, NowUnix());
  if (!built.script) {
    return {false, built.refusal};
  }
  if (!storage->Save(*built.script)) {
    return {false, "I couldn't save it."};
  }
  storage->AppendAudit(id, "voice_routine_saved",
                       built.script->security.sites_unconfirmed
                           ? "draft: the planner's steps"
                           : "");
  if (built.script->security.trust == automation::TrustLevel::APPROVED) {
    storage->AppendAudit(id, "trust_changed",
                         "casual -> approved (taught by voice)");
  }
  return {true, std::string()};
}

std::optional<RoutineToRun> LoadRoutineBlocking(const base::FilePath& root,
                                                const std::string& name) {
  base::ScopedBlockingCall blocking(FROM_HERE, base::BlockingType::MAY_BLOCK);
  std::unique_ptr<AutomationStorage> storage = Storage(root);
  const std::vector<Script> routines = VoiceRoutines(*storage);
  const Script* script = FindRoutine(routines, name, std::string());
  if (!script) {
    return std::nullopt;
  }
  RoutineToRun run;
  run.id = script->id;
  run.name = script->name;
  ReadRoutineResult read = ReadRoutine(*script);
  run.ok = read.ok;
  run.why = read.why;
  run.steps = std::move(read.steps);
  run.needs_sites_confirmed = read.needs_sites_confirmed;
  run.policy = AutomationSecurity::EffectivePolicy(*script);
  run.sites = AutomationSecurity::HostsReferencedBy(script->steps);
  return run;
}

bool DeleteRoutineBlocking(const base::FilePath& root,
                           const std::string& name,
                           const std::string& id) {
  base::ScopedBlockingCall blocking(FROM_HERE, base::BlockingType::MAY_BLOCK);
  std::unique_ptr<AutomationStorage> storage = Storage(root);
  const std::vector<Script> routines = VoiceRoutines(*storage);
  const Script* script = FindRoutine(routines, name, id);
  if (!script) {
    return false;
  }
  const std::string found = script->id;
  if (!storage->Delete(found)) {
    return false;
  }
  storage->AppendAudit(found, "voice_routine_deleted", "");
  return true;
}

bool ConfirmSitesBlocking(const base::FilePath& root,
                          const std::string& id,
                          const std::vector<std::string>& sites) {
  base::ScopedBlockingCall blocking(FROM_HERE, base::BlockingType::MAY_BLOCK);
  std::unique_ptr<AutomationStorage> storage = Storage(root);
  std::optional<Script> script = storage->Load(id);
  if (!script || !automation::HasVoiceSteps(*script) ||
      !AutomationSecurity::ConfirmDraftSites(*script, sites) ||
      !storage->Save(*script)) {
    return false;
  }
  std::string list;
  for (const std::string& host : script->security.domain_whitelist) {
    list += (list.empty() ? "" : " ") + host;
  }
  storage->AppendAudit(id, "draft_sites_confirmed", list + " (by voice)");
  return true;
}

void DeleteAllRoutinesBlocking(const base::FilePath& root) {
  base::ScopedBlockingCall blocking(FROM_HERE, base::BlockingType::MAY_BLOCK);
  std::unique_ptr<AutomationStorage> storage = Storage(root);
  for (const Script& script : VoiceRoutines(*storage)) {
    if (storage->Delete(script.id)) {
      storage->AppendAudit(script.id, "voice_routine_deleted",
                           "forget everything");
    }
  }
}

}  // namespace

RoutineToRun::RoutineToRun() = default;
RoutineToRun::~RoutineToRun() = default;
RoutineToRun::RoutineToRun(RoutineToRun&&) = default;
RoutineToRun& RoutineToRun::operator=(RoutineToRun&&) = default;

// static
VoiceLearning* VoiceLearning::Get(Profile* profile) {
  // Never off the record: MoltNet's profiles are, and so is a private
  // window's.
  if (!profile || profile->IsOffTheRecord()) {
    return nullptr;
  }
  auto* learning =
      static_cast<VoiceLearning*>(profile->GetUserData(kUserDataKey));
  if (!learning) {
    auto owned = base::WrapUnique(new VoiceLearning(profile));
    learning = owned.get();
    profile->SetUserData(kUserDataKey, std::move(owned));
  }
  return learning;
}

VoiceLearning::VoiceLearning(Profile* profile)
    : path_(profile->GetPath().AppendASCII(kVoiceLearningFileName)),
      task_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::BLOCK_SHUTDOWN})) {
  task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&ReadLearningFile, path_),
      base::BindOnce(
          [](base::WeakPtr<VoiceLearning> self, ReadResult result) {
            if (self) {
              const bool learn = result.data.learn;
              self->OnRead(std::move(result.data), learn);
            }
          },
          weak_factory_.GetWeakPtr()));
  RefreshRoutines();
}

VoiceLearning::~VoiceLearning() {
  // What changed since the last write goes now (the sequence blocks
  // shutdown until it is written).
  if (write_timer_.IsRunning()) {
    write_timer_.Stop();
    Write();
  }
}

void VoiceLearning::OnRead(LearnedData data, bool enabled) {
  // Nothing changes before the file came (learning waits for it, and the
  // rest is queued with WhenLoaded()).
  data_ = std::move(data);
  enabled_ = enabled;
  loaded_ = true;
  std::vector<base::OnceClosure> waiting;
  waiting.swap(when_loaded_);
  for (base::OnceClosure& done : waiting) {
    std::move(done).Run();
  }
  changed_callbacks_.Notify();
}

void VoiceLearning::WhenLoaded(base::OnceClosure done) {
  if (loaded_) {
    std::move(done).Run();
    return;
  }
  when_loaded_.push_back(std::move(done));
}

void VoiceLearning::SetEnabled(bool enabled) {
  if (!loaded_) {
    WhenLoaded(base::BindOnce(&VoiceLearning::SetEnabled,
                              weak_factory_.GetWeakPtr(), enabled));
    return;
  }
  if (enabled_ == enabled) {
    return;
  }
  enabled_ = enabled;
  Changed();
}

void VoiceLearning::NoteName(std::string_view phrase) {
  if (enabled() && data_.vocabulary.NoteName(phrase, NowUnix())) {
    Changed();
  }
}

void VoiceLearning::NoteCorrection(std::string_view heard,
                                   std::string_view meant) {
  if (enabled() && data_.vocabulary.NoteCorrection(heard, meant, NowUnix())) {
    Changed();
  }
}

void VoiceLearning::NoteCorrectionUsed(std::string_view heard) {
  if (enabled()) {
    data_.vocabulary.NoteCorrectionUsed(heard, NowUnix());
    Changed();
  }
}

void VoiceLearning::HintWorked(std::string_view host,
                               SitePart part,
                               std::string_view name) {
  if (!enabled()) {
    return;
  }
  data_.hints.Worked(host, part, name, NowUnix());
  Changed();
}

void VoiceLearning::HintFailed(std::string_view host, SitePart part) {
  if (!enabled() || !data_.hints.Find(host)) {
    return;
  }
  data_.hints.Failed(host, part);
  Changed();
}

size_t VoiceLearning::ForgetWord(std::string_view phrase) {
  const size_t forgotten = data_.vocabulary.Forget(phrase);
  if (forgotten) {
    Changed();
  }
  return forgotten;
}

bool VoiceLearning::ForgetHint(std::string_view host) {
  const bool forgotten = data_.hints.Forget(host);
  if (forgotten) {
    Changed();
  }
  return forgotten;
}

void VoiceLearning::ForgetEverything(base::OnceClosure done) {
  if (!loaded_) {
    WhenLoaded(base::BindOnce(&VoiceLearning::ForgetEverything,
                              weak_factory_.GetWeakPtr(), std::move(done)));
    return;
  }
  data_.vocabulary.Clear();
  data_.hints.Clear();
  Changed();
  task_runner_->PostTaskAndReply(
      FROM_HERE, base::BindOnce(&DeleteAllRoutinesBlocking, automations_root_),
      base::BindOnce(
          [](base::WeakPtr<VoiceLearning> self, base::OnceClosure done) {
            if (self) {
              self->RefreshRoutines();
            }
            std::move(done).Run();
          },
          weak_factory_.GetWeakPtr(), std::move(done)));
}

void VoiceLearning::SaveRoutine(std::string name,
                                std::vector<RoutineStep> steps,
                                SavedCallback done) {
  const std::string id = "voice_" + base::NumberToString(NowUnix()) + "_" +
                         base::NumberToString(++saves_);
  task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&SaveRoutineBlocking, automations_root_, id,
                     std::move(name), std::move(steps)),
      base::BindOnce(
          [](base::WeakPtr<VoiceLearning> self, SavedCallback done,
             std::pair<bool, std::string> result) {
            if (self) {
              self->RefreshRoutines();
            }
            std::move(done).Run(result.first, std::move(result.second));
          },
          weak_factory_.GetWeakPtr(), std::move(done)));
}

void VoiceLearning::LoadRoutine(
    std::string name,
    base::OnceCallback<void(std::optional<RoutineToRun>)> done) {
  task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&LoadRoutineBlocking, automations_root_, std::move(name)),
      std::move(done));
}

void VoiceLearning::DeleteRoutine(std::string name,
                                  std::string id,
                                  base::OnceCallback<void(bool)> done) {
  task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&DeleteRoutineBlocking, automations_root_,
                     std::move(name), std::move(id)),
      base::BindOnce(
          [](base::WeakPtr<VoiceLearning> self,
             base::OnceCallback<void(bool)> done, bool deleted) {
            if (self) {
              self->RefreshRoutines();
            }
            std::move(done).Run(deleted);
          },
          weak_factory_.GetWeakPtr(), std::move(done)));
}

void VoiceLearning::ConfirmRoutineSites(std::string id,
                                        std::vector<std::string> sites,
                                        base::OnceCallback<void(bool)> done) {
  task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&ConfirmSitesBlocking, automations_root_, std::move(id),
                     std::move(sites)),
      base::BindOnce(
          [](base::WeakPtr<VoiceLearning> self,
             base::OnceCallback<void(bool)> done, bool confirmed) {
            if (self) {
              self->RefreshRoutines();
            }
            std::move(done).Run(confirmed);
          },
          weak_factory_.GetWeakPtr(), std::move(done)));
}

void VoiceLearning::RefreshRoutines() {
  task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&ListVoiceRoutines, automations_root_),
      base::BindOnce(&VoiceLearning::OnRoutines, weak_factory_.GetWeakPtr()));
}

void VoiceLearning::OnRoutines(std::vector<RoutineSummary> routines) {
  routines_ = std::move(routines);
  changed_callbacks_.Notify();
}

base::CallbackListSubscription VoiceLearning::AddChangedCallback(
    base::RepeatingClosure callback) {
  return changed_callbacks_.Add(std::move(callback));
}

void VoiceLearning::Changed() {
  changed_callbacks_.Notify();
  // Never over the file before it was read.
  if (loaded_ && !write_timer_.IsRunning()) {
    write_timer_.Start(FROM_HERE, kWriteDelay,
                       base::BindOnce(&VoiceLearning::Write,
                                      base::Unretained(this)));
  }
}

void VoiceLearning::Write() {
  LearnedData data = data_;
  data.learn = enabled_;
  task_runner_->PostTask(FROM_HERE, base::BindOnce(&WriteLearningFile, path_,
                                                   SerializeLearning(data)));
}

void VoiceLearning::SetAutomationsRootForTesting(const base::FilePath& root) {
  automations_root_ = root;
  RefreshRoutines();
}

void VoiceLearning::FlushForTesting(base::OnceClosure done) {
  if (write_timer_.IsRunning()) {
    write_timer_.Stop();
    Write();
  }
  task_runner_->PostTaskAndReply(FROM_HERE, base::DoNothing(),
                                 std::move(done));
}

}  // namespace voice
}  // namespace molt_ai
