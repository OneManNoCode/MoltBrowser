// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: the storage service's work for MoltNet's localStorage and
// sessionStorage, taken in turn per site.
//
// Every message a page sends to its storage areas is handled on the storage
// service's one sequence, which every site's storage operations share. For
// each message waiting on an area's pipe, mojo's Connector posts one task to
// that sequence, all at once (mojo/public/cpp/bindings/lib/connector.cc:
// ScheduleDispatchOfPendingMessagesOrWaitForMore()), so a page sending
// messages in a tight loop (writes its site's write allowance refuses,
// clear(), removals) queued thousands of tasks, and another site's read
// waited for all of them: one flooding site kept the sequence about half
// busy, two saturated it. Refusing a message doesn't help: a refusal is
// handled too.
//
// In a partition that keeps DOM storage per site (MoltNet's), each site's
// area pipes (and its observers') are bound to a task runner of the site's
// own (MoltDomStorageSiteTaskRunner), whose tasks wait in the site's queue
// here, and one task at a time on the storage sequence takes the next task
// of the site whose turn it is (MoltDomStorageFairScheduler): start-time
// fair queueing, by the time each task took. A site's next task starts at
// the virtual time its previous one finished (its start plus the time it
// took, at least a microsecond), or, for a site that had nothing queued,
// at the later of that and now (the start of the task in service). The
// task with the earliest start goes first; on a tie, a site that had
// nothing queued before one that kept tasks queued, then in the order
// they became ready. So:
//  * a site that had nothing queued, and has used no more than its share
//    of the sequence's time, goes next, after the task in service (and
//    other such sites that came first, one task each): however many
//    messages other sites have queued, its read waits for one task of
//    another site;
//  * sites with work queued share the sequence's time equally; a site
//    can't take more than its share by sending more, or by spreading its
//    messages over many areas (they share its queue);
//  * a site idle for a while gets no credit for it: it starts at the
//    current virtual time.
// What a page can still observe: the task in service when its own message
// arrives (one message of another site, the longest being a write of an
// area's whole limit), and the storage sequence's other work (other
// partitions, the browser's requests).
//
// Std-only, so molt_ai_unittests and a standalone harness test it. Not
// thread-safe: MoltDomStorageFairScheduler locks around it.

#ifndef COMPONENTS_SERVICES_STORAGE_DOM_STORAGE_MOLT_DOM_STORAGE_FAIR_QUEUE_H_
#define COMPONENTS_SERVICES_STORAGE_DOM_STORAGE_MOLT_DOM_STORAGE_FAIR_QUEUE_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace storage {

// The least a task counts, in microseconds: a site's tasks always move its
// virtual time on, however fast the clock says they were.
inline constexpr int64_t kMoltDomStorageMinTaskMicroseconds = 1;

template <typename Task>
class MoltDomStorageFairQueue {
 public:
  MoltDomStorageFairQueue() = default;
  MoltDomStorageFairQueue(const MoltDomStorageFairQueue&) = delete;
  MoltDomStorageFairQueue& operator=(const MoltDomStorageFairQueue&) = delete;

  // Queues `task` for `site`, after the site's other tasks.
  void Push(std::string_view site, Task task) {
    auto it = sites_.find(site);
    if (it == sites_.end()) {
      it = sites_.emplace(std::string(site), Site()).first;
    }
    Site& state = it->second;
    state.tasks.push_back(std::move(task));
    if (state.serving || state.ready) {
      return;
    }
    // Idle until now: it starts now, or when its last task finished.
    idle_.erase({state.finish, it->first});
    MakeReady(it->first, state,
              state.finish > virtual_time_ ? state.finish : virtual_time_,
              kWasIdle);
  }

  // Takes the next task: the first of the site with the earliest start.
  // The site is in service until Charge(). False if no site is ready.
  bool Pop(std::string* site, Task* task) {
    if (ready_.empty()) {
      return false;
    }
    const std::string name = std::get<std::string>(*ready_.begin());
    virtual_time_ = std::get<0>(*ready_.begin());
    ready_.erase(ready_.begin());
    auto it = sites_.find(name);
    Site& state = it->second;
    state.ready = false;
    state.serving = true;
    *site = name;
    *task = std::move(state.tasks.front());
    state.tasks.pop_front();
    ForgetIdleSitesBefore(virtual_time_);
    return true;
  }

  // The task Pop() gave for `site` took `microseconds`.
  void Charge(std::string_view site, int64_t microseconds) {
    auto it = sites_.find(site);
    if (it == sites_.end() || !it->second.serving) {
      return;
    }
    Site& state = it->second;
    state.serving = false;
    state.finish =
        state.start + (microseconds > kMoltDomStorageMinTaskMicroseconds
                           ? microseconds
                           : kMoltDomStorageMinTaskMicroseconds);
    if (!state.tasks.empty()) {
      MakeReady(it->first, state, state.finish, kKeptTasks);
      return;
    }
    if (state.finish <= virtual_time_) {
      sites_.erase(it);
      return;
    }
    idle_.insert({state.finish, it->first});
  }

  // Whether a site has a task ready (a site in service has none until
  // Charge()).
  bool HasReadyTasks() const { return !ready_.empty(); }

  // Sites with tasks queued, in service, or idle with a finish still ahead.
  size_t SiteCountForTesting() const { return sites_.size(); }
  int64_t VirtualTimeForTesting() const { return virtual_time_; }

 private:
  struct Site {
    std::deque<Task> tasks;
    // The virtual start of its next (or current) task, and the finish of
    // its last one.
    int64_t start = 0;
    int64_t finish = 0;
    bool ready = false;
    bool serving = false;
  };

  // On a tie, a site that had nothing queued goes before one that kept
  // tasks queued.
  static constexpr int kWasIdle = 0;
  static constexpr int kKeptTasks = 1;

  void MakeReady(const std::string& name,
                 Site& state,
                 int64_t start,
                 int tier) {
    state.start = start;
    state.ready = true;
    ready_.insert({start, tier, next_order_++, name});
  }

  // An idle site whose last task finished by `virtual_time` would start at
  // the virtual time anyway: nothing is kept for it.
  void ForgetIdleSitesBefore(int64_t virtual_time) {
    while (!idle_.empty() && idle_.begin()->first <= virtual_time) {
      sites_.erase(idle_.begin()->second);
      idle_.erase(idle_.begin());
    }
  }

  std::map<std::string, Site, std::less<>> sites_;
  // Ready sites by start, then kWasIdle before kKeptTasks, then the order
  // they became ready.
  std::set<std::tuple<int64_t, int, uint64_t, std::string>> ready_;
  // Idle sites by when their last task finished.
  std::set<std::pair<int64_t, std::string>> idle_;
  int64_t virtual_time_ = 0;
  uint64_t next_order_ = 0;
};

}  // namespace storage

#endif  // COMPONENTS_SERVICES_STORAGE_DOM_STORAGE_MOLT_DOM_STORAGE_FAIR_QUEUE_H_
