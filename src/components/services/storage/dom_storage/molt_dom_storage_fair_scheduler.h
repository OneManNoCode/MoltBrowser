// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: the storage service's sequence taken in turn per site, for a
// partition that keeps DOM storage per site (MoltNet's). The rule is in
// molt_dom_storage_fair_queue.h.
//
// LocalStorageImpl and SessionStorageImpl each make one scheduler when told
// to keep caches per site, and bind each storage area's pipes (the area's
// receivers and its observers' remotes: StorageAreaImpl,
// SessionStorageAreaImpl) to a MoltDomStorageSiteTaskRunner of the area's
// site (MoltDomStorageSiteOf()). Mojo then posts the tasks that read and
// handle the area's messages, and those that report a closed pipe, to that
// runner: they wait in the site's queue, and one task at a time on the
// storage sequence runs the next task whose turn it is, measuring how long
// it takes. Everything still runs on the storage sequence, in the order
// mojo posted it for each site; only the order between sites changes.
//
// Header-only (with out-of-class inline definitions), so the storage
// service's hooks need no new build target.

#ifndef COMPONENTS_SERVICES_STORAGE_DOM_STORAGE_MOLT_DOM_STORAGE_FAIR_SCHEDULER_H_
#define COMPONENTS_SERVICES_STORAGE_DOM_STORAGE_MOLT_DOM_STORAGE_FAIR_SCHEDULER_H_

#include <cstdint>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/location.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/synchronization/lock.h"
#include "base/task/sequenced_task_runner.h"
#include "base/thread_annotations.h"
#include "base/time/time.h"
#include "components/services/storage/dom_storage/molt_dom_storage_fair_queue.h"

namespace storage {

class MoltDomStorageFairScheduler
    : public base::RefCountedThreadSafe<MoltDomStorageFairScheduler> {
 public:
  // `sequence`: the storage service's sequence, which runs every task.
  explicit MoltDomStorageFairScheduler(
      scoped_refptr<base::SequencedTaskRunner> sequence);
  MoltDomStorageFairScheduler(const MoltDomStorageFairScheduler&) = delete;
  MoltDomStorageFairScheduler& operator=(const MoltDomStorageFairScheduler&) =
      delete;

  // A task runner whose tasks wait in `site`'s queue. Every runner of one
  // site shares the site's queue.
  scoped_refptr<base::SequencedTaskRunner> RunnerFor(std::string site);

  // Queues `task` for `site`. Any thread.
  void Post(std::string site, base::OnceClosure task);

  const scoped_refptr<base::SequencedTaskRunner>& sequence() const {
    return sequence_;
  }

 private:
  friend class base::RefCountedThreadSafe<MoltDomStorageFairScheduler>;
  ~MoltDomStorageFairScheduler();

  // Runs the next task on the storage sequence, and asks for another turn
  // while tasks wait.
  void RunNext();
  void PostRunNext();

  const scoped_refptr<base::SequencedTaskRunner> sequence_;
  base::Lock lock_;
  MoltDomStorageFairQueue<base::OnceClosure> queue_ GUARDED_BY(lock_);
  // Whether a RunNext() is posted or running.
  bool run_posted_ GUARDED_BY(lock_) = false;
};

// A site's view of the storage sequence: its tasks run there, in the order
// they were posted, when the site's turn comes.
class MoltDomStorageSiteTaskRunner : public base::SequencedTaskRunner {
 public:
  MoltDomStorageSiteTaskRunner(
      scoped_refptr<MoltDomStorageFairScheduler> scheduler,
      std::string site);
  MoltDomStorageSiteTaskRunner(const MoltDomStorageSiteTaskRunner&) = delete;
  MoltDomStorageSiteTaskRunner& operator=(const MoltDomStorageSiteTaskRunner&) =
      delete;

  // base::SequencedTaskRunner:
  bool PostDelayedTask(const base::Location& from_here,
                       base::OnceClosure task,
                       base::TimeDelta delay) override;
  bool PostNonNestableDelayedTask(const base::Location& from_here,
                                  base::OnceClosure task,
                                  base::TimeDelta delay) override;
  bool RunsTasksInCurrentSequence() const override;

 private:
  ~MoltDomStorageSiteTaskRunner() override;

  const scoped_refptr<MoltDomStorageFairScheduler> scheduler_;
  const std::string site_;
};

// ---- Definitions ----

inline MoltDomStorageFairScheduler::MoltDomStorageFairScheduler(
    scoped_refptr<base::SequencedTaskRunner> sequence)
    : sequence_(std::move(sequence)) {}

inline MoltDomStorageFairScheduler::~MoltDomStorageFairScheduler() = default;

inline scoped_refptr<base::SequencedTaskRunner>
MoltDomStorageFairScheduler::RunnerFor(std::string site) {
  return base::MakeRefCounted<MoltDomStorageSiteTaskRunner>(
      base::WrapRefCounted(this), std::move(site));
}

inline void MoltDomStorageFairScheduler::Post(std::string site,
                                              base::OnceClosure task) {
  bool post = false;
  {
    base::AutoLock lock(lock_);
    queue_.Push(site, std::move(task));
    post = !run_posted_;
    run_posted_ = true;
  }
  if (post) {
    PostRunNext();
  }
}

inline void MoltDomStorageFairScheduler::PostRunNext() {
  // Non-nestable: the sites' tasks never run in a nested loop.
  if (!sequence_->PostNonNestableTask(
          FROM_HERE, base::BindOnce(&MoltDomStorageFairScheduler::RunNext,
                                    base::WrapRefCounted(this)))) {
    // The sequence is shutting down: nothing more runs.
    base::AutoLock lock(lock_);
    run_posted_ = false;
  }
}

inline void MoltDomStorageFairScheduler::RunNext() {
  std::string site;
  base::OnceClosure task;
  {
    base::AutoLock lock(lock_);
    if (!queue_.Pop(&site, &task)) {
      run_posted_ = false;
      return;
    }
  }
  const base::TimeTicks start = base::TimeTicks::Now();
  std::move(task).Run();
  const int64_t microseconds =
      (base::TimeTicks::Now() - start).InMicroseconds();
  bool more = false;
  {
    base::AutoLock lock(lock_);
    queue_.Charge(site, microseconds);
    more = queue_.HasReadyTasks();
    run_posted_ = more;
  }
  if (more) {
    PostRunNext();
  }
}

inline MoltDomStorageSiteTaskRunner::MoltDomStorageSiteTaskRunner(
    scoped_refptr<MoltDomStorageFairScheduler> scheduler,
    std::string site)
    : scheduler_(std::move(scheduler)), site_(std::move(site)) {}

inline MoltDomStorageSiteTaskRunner::~MoltDomStorageSiteTaskRunner() = default;

inline bool MoltDomStorageSiteTaskRunner::PostDelayedTask(
    const base::Location& from_here,
    base::OnceClosure task,
    base::TimeDelta delay) {
  if (delay.is_positive()) {
    // Into the site's queue once it is due.
    return scheduler_->sequence()->PostDelayedTask(
        from_here,
        base::BindOnce(&MoltDomStorageFairScheduler::Post, scheduler_, site_,
                       std::move(task)),
        delay);
  }
  scheduler_->Post(site_, std::move(task));
  return true;
}

inline bool MoltDomStorageSiteTaskRunner::PostNonNestableDelayedTask(
    const base::Location& from_here,
    base::OnceClosure task,
    base::TimeDelta delay) {
  // The sites' tasks never run nested (PostRunNext()).
  return PostDelayedTask(from_here, std::move(task), delay);
}

inline bool MoltDomStorageSiteTaskRunner::RunsTasksInCurrentSequence() const {
  return scheduler_->sequence()->RunsTasksInCurrentSequence();
}

}  // namespace storage

#endif  // COMPONENTS_SERVICES_STORAGE_DOM_STORAGE_MOLT_DOM_STORAGE_FAIR_SCHEDULER_H_
