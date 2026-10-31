// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_learning.h"

#include <algorithm>
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
#include "chrome/browser/molt_ai/voice/voice_routine_store.h"
#include "chrome/browser/profiles/profile.h"

namespace molt_ai {
namespace voice {

namespace {

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

// The blocking parts run on the learning's sequence, each with a store of
// the profile's routines and the shared storage (|shared_root| when tests
// set one).
struct Listed {
  std::vector<RoutineSummary> routines;
  std::vector<RoutineSummary> unclaimed;
};

Listed ListBlocking(const base::FilePath& routines_dir,
                    const base::FilePath& shared_root) {
  base::ScopedBlockingCall blocking(FROM_HERE, base::BlockingType::MAY_BLOCK);
  VoiceRoutineStore store(routines_dir, shared_root);
  Listed listed;
  listed.routines = store.Summaries();
  listed.unclaimed = store.Unclaimed();
  return listed;
}

RoutineSaveResult SaveBlocking(const base::FilePath& routines_dir,
                               const base::FilePath& shared_root,
                               const std::string& id,
                               const std::string& name,
                               const std::vector<RoutineStep>& steps,
                               bool replace) {
  base::ScopedBlockingCall blocking(FROM_HERE, base::BlockingType::MAY_BLOCK);
  VoiceRoutineStore store(routines_dir, shared_root);
  return store.Save(id, name, steps, replace, NowUnix());
}

std::optional<RoutineToRun> LoadBlocking(const base::FilePath& routines_dir,
                                         const base::FilePath& shared_root,
                                         const std::string& name,
                                         const std::string& id) {
  base::ScopedBlockingCall blocking(FROM_HERE, base::BlockingType::MAY_BLOCK);
  VoiceRoutineStore store(routines_dir, shared_root);
  return store.Load(name, id);
}

bool DeleteBlocking(const base::FilePath& routines_dir,
                    const base::FilePath& shared_root,
                    const std::string& name,
                    const std::string& id) {
  base::ScopedBlockingCall blocking(FROM_HERE, base::BlockingType::MAY_BLOCK);
  VoiceRoutineStore store(routines_dir, shared_root);
  return store.Delete(name, id);
}

bool ConfirmSitesBlocking(const base::FilePath& routines_dir,
                          const base::FilePath& shared_root,
                          const std::string& id,
                          const std::vector<std::string>& sites) {
  base::ScopedBlockingCall blocking(FROM_HERE, base::BlockingType::MAY_BLOCK);
  VoiceRoutineStore store(routines_dir, shared_root);
  return store.ConfirmSites(id, sites);
}

void DeleteAllBlocking(const base::FilePath& routines_dir,
                       const base::FilePath& shared_root) {
  base::ScopedBlockingCall blocking(FROM_HERE, base::BlockingType::MAY_BLOCK);
  VoiceRoutineStore store(routines_dir, shared_root);
  store.DeleteAll();
}

ClaimResult ClaimBlocking(const base::FilePath& routines_dir,
                          const base::FilePath& shared_root,
                          const std::vector<std::string>& ids) {
  base::ScopedBlockingCall blocking(FROM_HERE, base::BlockingType::MAY_BLOCK);
  VoiceRoutineStore store(routines_dir, shared_root);
  return store.Claim(ids);
}

}  // namespace

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
      routines_dir_(VoiceRoutinesDir(profile->GetPath())),
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
  // Loaded once the routines are listed too.
  RefreshRoutinesThen(base::BindOnce(&VoiceLearning::OnLoaded,
                                     weak_factory_.GetWeakPtr()));
}

void VoiceLearning::OnLoaded() {
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
      FROM_HERE,
      base::BindOnce(&DeleteAllBlocking, routines_dir_, automations_root_),
      base::BindOnce(&VoiceLearning::RefreshRoutinesThen,
                     weak_factory_.GetWeakPtr(), std::move(done)));
}

void VoiceLearning::SaveRoutine(std::string name,
                                std::vector<RoutineStep> steps,
                                bool replace,
                                SavedCallback done) {
  const std::string id = "voice_" + base::NumberToString(NowUnix()) + "_" +
                         base::NumberToString(++saves_);
  task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&SaveBlocking, routines_dir_, automations_root_, id,
                     std::move(name), std::move(steps), replace),
      base::BindOnce(
          [](base::WeakPtr<VoiceLearning> self, SavedCallback done,
             RoutineSaveResult result) {
            if (self) {
              self->RefreshRoutinesThen(
                  base::BindOnce(std::move(done), std::move(result)));
            }
          },
          weak_factory_.GetWeakPtr(), std::move(done)));
}

void VoiceLearning::LoadRoutine(
    std::string name,
    std::string id,
    base::OnceCallback<void(std::optional<RoutineToRun>)> done) {
  task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&LoadBlocking, routines_dir_, automations_root_,
                     std::move(name), std::move(id)),
      std::move(done));
}

void VoiceLearning::DeleteRoutine(std::string name,
                                  std::string id,
                                  base::OnceCallback<void(bool)> done) {
  task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&DeleteBlocking, routines_dir_, automations_root_,
                     std::move(name), std::move(id)),
      base::BindOnce(
          [](base::WeakPtr<VoiceLearning> self,
             base::OnceCallback<void(bool)> done, bool deleted) {
            if (self) {
              self->RefreshRoutinesThen(
                  base::BindOnce(std::move(done), deleted));
            }
          },
          weak_factory_.GetWeakPtr(), std::move(done)));
}

void VoiceLearning::ConfirmRoutineSites(std::string id,
                                        std::vector<std::string> sites,
                                        base::OnceCallback<void(bool)> done) {
  task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&ConfirmSitesBlocking, routines_dir_, automations_root_,
                     std::move(id), std::move(sites)),
      base::BindOnce(
          [](base::WeakPtr<VoiceLearning> self,
             base::OnceCallback<void(bool)> done, bool confirmed) {
            if (self) {
              self->RefreshRoutinesThen(
                  base::BindOnce(std::move(done), confirmed));
            }
          },
          weak_factory_.GetWeakPtr(), std::move(done)));
}

void VoiceLearning::RefreshRoutines() {
  RefreshRoutinesThen(base::DoNothing());
}

void VoiceLearning::RefreshRoutinesThen(base::OnceClosure done) {
  task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&ListBlocking, routines_dir_, automations_root_),
      base::BindOnce(
          [](base::WeakPtr<VoiceLearning> self, base::OnceClosure done,
             Listed listed) {
            if (self) {
              self->OnRoutines(std::move(done), std::move(listed.routines),
                               std::move(listed.unclaimed));
            }
          },
          weak_factory_.GetWeakPtr(), std::move(done)));
}

void VoiceLearning::OnRoutines(base::OnceClosure done,
                               std::vector<RoutineSummary> routines,
                               std::vector<RoutineSummary> unclaimed) {
  routines_ = std::move(routines);
  all_unclaimed_ = std::move(unclaimed);
  FilterUnclaimed();
  changed_callbacks_.Notify();
  std::move(done).Run();
}

void VoiceLearning::FilterUnclaimed() {
  std::vector<RoutineSummary> offered;
  size_t declined = 0;
  for (const RoutineSummary& routine : all_unclaimed_) {
    if (Declined(routine.id)) {
      ++declined;
    } else {
      offered.push_back(routine);
    }
  }
  // Another set of routines (or of names): a "yes" given to the question
  // about the old set keeps nothing.
  auto same = [](const RoutineSummary& a, const RoutineSummary& b) {
    return a.id == b.id && a.name == b.name;
  };
  if (offered.size() != unclaimed_.size() ||
      !std::equal(offered.begin(), offered.end(), unclaimed_.begin(), same)) {
    ++unclaimed_version_;
  }
  unclaimed_ = std::move(offered);
  declined_listed_ = declined;
}

bool VoiceLearning::Declined(const std::string& id) const {
  const std::vector<std::string>& declined = data_.declined_routines;
  return std::find(declined.begin(), declined.end(), id) != declined.end();
}

void VoiceLearning::ClaimRoutines(std::vector<std::string> ids,
                                  base::OnceCallback<void(ClaimResult)> done) {
  task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&ClaimBlocking, routines_dir_, automations_root_,
                     std::move(ids)),
      base::BindOnce(
          [](base::WeakPtr<VoiceLearning> self,
             base::OnceCallback<void(ClaimResult)> done, ClaimResult result) {
            if (self) {
              self->RefreshRoutinesThen(
                  base::BindOnce(std::move(done), std::move(result)));
            }
          },
          weak_factory_.GetWeakPtr(), std::move(done)));
}

void VoiceLearning::DeclineRoutines(const std::vector<std::string>& ids) {
  if (!loaded_) {
    WhenLoaded(base::BindOnce(&VoiceLearning::DeclineRoutines,
                              weak_factory_.GetWeakPtr(), ids));
    return;
  }
  if (AddDeclinedRoutines(data_.declined_routines, ids)) {
    FilterUnclaimed();
    Changed();
  }
}

void VoiceLearning::UndeclineRoutines() {
  if (!loaded_) {
    WhenLoaded(base::BindOnce(&VoiceLearning::UndeclineRoutines,
                              weak_factory_.GetWeakPtr()));
    return;
  }
  if (data_.declined_routines.empty()) {
    return;
  }
  data_.declined_routines.clear();
  FilterUnclaimed();
  Changed();
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
