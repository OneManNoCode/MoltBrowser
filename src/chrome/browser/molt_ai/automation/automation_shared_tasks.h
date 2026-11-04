// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The shared storage's work for the callers on the UI thread
// (molt://ai-automation, the AI chat's saves, the recorder, the run window,
// the runner, the scheduler and its service): never a file read or
// written on the UI thread. Each posts its blocking part, run with an
// AutomationStorage of |dir| (the shared storage's directory,
// AutomationStorage::SharedDirectory() or the profile's automation
// service's, taken on the calling sequence: no I/O):
//   - a change (a save, an update, a delete, an audit line) to
//     AutomationStorage::SharedSequence(), in the order posted, which holds
//     shutdown until it is written;
//   - a read, after the changes posted before it (a page reloaded after a
//     save lists what it saved), to AutomationStorage::SharedReadSequence(),
//     which never holds shutdown (molt_ai/common/molt_file_sequences.h);
// then replies with its result on the calling sequence.
//
// A task is a callback whose last argument is the storage:
//   PostSharedChange(dir, base::BindOnce(&SaveIt, std::move(script)),
//                    base::BindOnce(&Page::OnSaved, weak_ptr));
// with bool SaveIt(Script script, AutomationStorage& storage).

#ifndef CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_SHARED_TASKS_H_
#define CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_SHARED_TASKS_H_

#include <string>
#include <utility>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/molt_ai/automation/automation_storage.h"
#include "chrome/browser/molt_ai/common/molt_file_sequences.h"

namespace molt_ai {
namespace automation {

namespace internal {

template <typename R>
R RunWithSharedStorage(const base::FilePath& dir,
                       base::OnceCallback<R(AutomationStorage&)> task) {
  AutomationStorage storage(dir);
  return std::move(task).Run(storage);
}

}  // namespace internal

// A change: |task| on SharedSequence(), then |reply| with its result here.
template <typename R, typename ReplyArg>
void PostSharedChange(const base::FilePath& dir,
                      base::OnceCallback<R(AutomationStorage&)> task,
                      base::OnceCallback<void(ReplyArg)> reply) {
  AutomationStorage::SharedSequence()->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&internal::RunWithSharedStorage<R>, dir, std::move(task)),
      std::move(reply));
}

// A change nothing waits for.
inline void PostSharedChange(
    const base::FilePath& dir,
    base::OnceCallback<void(AutomationStorage&)> task) {
  AutomationStorage::SharedSequence()->PostTask(
      FROM_HERE, base::BindOnce(&internal::RunWithSharedStorage<void>, dir,
                                std::move(task)));
}

// A read: |task| on SharedReadSequence() after the changes posted before,
// then |reply| with its result here.
template <typename R, typename ReplyArg>
void PostSharedRead(const base::FilePath& dir,
                    base::OnceCallback<R(AutomationStorage&)> task,
                    base::OnceCallback<void(ReplyArg)> reply) {
  PostReadAfterChanges(
      AutomationStorage::SharedSequence(),
      AutomationStorage::SharedReadSequence(), FROM_HERE,
      base::BindOnce(&internal::RunWithSharedStorage<R>, dir, std::move(task)),
      std::move(reply));
}

namespace internal {

inline void AppendSharedAudit(const std::string& id,
                              const std::string& event,
                              const std::string& extra,
                              AutomationStorage& storage) {
  storage.AppendAudit(id, event, extra);
}

}  // namespace internal

// An audit line (AutomationStorage::AppendAudit()), in order with the
// changes.
inline void PostSharedAudit(const base::FilePath& dir,
                            std::string id,
                            std::string event,
                            std::string extra) {
  PostSharedChange(dir,
                   base::BindOnce(&internal::AppendSharedAudit, std::move(id),
                                  std::move(event), std::move(extra)));
}

}  // namespace automation
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_SHARED_TASKS_H_
