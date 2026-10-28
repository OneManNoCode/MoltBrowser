// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: what one tab's or site's sessionStorage work can make others
// wait for, in a partition taken in turn per site (MoltNet's:
// molt_dom_storage_own_thread.h), and for how long.
//
// The namespace jobs. A copy or a deletion of a tab's namespace too big for
// one task is made a chunk a task (molt_dom_storage_split_work.h), and the
// control messages that name its namespaces wait until it ends. At round 30
// these jobs ran one at a time for the whole partition, in the order they
// came, in the control's turn: a tab's job waited for every job before it,
// whichever tab's (a site that opens and closes popups over a namespace of
// thousands of keys queued one after another), and so did the messages that
// name its namespaces (a bind of a popup's copy). Here a job waits only for
// the jobs before it that name one of its namespaces (a copy names its
// source and its destination, a deletion its namespace:
// MoltDomStorageNamespaceJobs), and takes its steps in the turn of its own
// namespace (a copy's source's, a deletion's: MoltDomStorageNamespaceGroup()),
// as that namespace's pipes do, at a namespace's weight. What round 30
// ordered is kept: jobs that share a namespace run one at a time, in the
// order they came, and every control message that names a job's namespace
// waits for it, in the order it came; jobs of different namespaces touch
// nothing of each other's but the maps they share, which each step leaves
// whole.
//
// A copy that waits for its sites' turns. A copy the browser makes at once
// (a tab duplicated: CloneNamespace() with kImmediate), and the copies a
// namespace makes as it goes while tabs it opened wait for theirs, wait for
// a turn of each site with areas in the namespace (or a bind or a deletion
// for it queued), so the copy holds what those sites queued before it; the
// messages that name the namespaces wait meanwhile. A site with a backlog
// (one flooding its own areas) made the copy, and the duplicated tab's
// pages of every site, wait for all of it. Here the wait ends
// kMoltDomStorageTurnsWaitMs after the turns are asked for, whatever turns
// are still to come (MoltDomStorageTurnWaits): a site whose turn hasn't
// come by then is copied as its areas are, and its writes still queued then
// land in the source only, as Chromium allows for a copy the browser makes
// (SessionStorageCloneType::kImmediate: "strict ordering against mutations
// is irrelevant"). A deletion of the source's areas asked for before the
// copy (Clear-Site-Data, the browser's settings) still applies to the copy:
// what it deletes is deleted from the copy at once, and from the source in
// its site's turn. Making the copy itself independent of a site's backlog
// would mean populating the copy a site at a time, in each site's turn,
// with every message that reads or copies the whole namespace (the
// renderer's own Clone(), a copy of the copy, its deletion) ordered after
// each site's part: a namespace's population per site in Chromium's
// SessionStorageNamespaceImpl, which this doesn't change. A site that keeps
// up with its share of the sequence has its turn long before the wait ends.
//
// Std-only, so molt_ai_unittests and a standalone harness test it.

#ifndef COMPONENTS_SERVICES_STORAGE_DOM_STORAGE_MOLT_DOM_STORAGE_WAITS_H_
#define COMPONENTS_SERVICES_STORAGE_DOM_STORAGE_MOLT_DOM_STORAGE_WAITS_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace storage {

// A partition's namespace jobs, in the order they came: a job may run once
// no job before it names one of its namespaces.
class MoltDomStorageNamespaceJobs {
 public:
  MoltDomStorageNamespaceJobs() = default;
  MoltDomStorageNamespaceJobs(const MoltDomStorageNamespaceJobs&) = delete;
  MoltDomStorageNamespaceJobs& operator=(const MoltDomStorageNamespaceJobs&) =
      delete;

  // Adds a job naming namespaces `a` and `b` (a deletion: its namespace
  // twice), after every job there; its id, greater than theirs.
  uint64_t Add(std::string_view a, std::string_view b) {
    const uint64_t id = next_id_++;
    Job& job = jobs_[id];
    job.a = std::string(a);
    job.b = std::string(b);
    queues_[job.a].push_back(id);
    if (job.b != job.a) {
      queues_[job.b].push_back(id);
    }
    return id;
  }

  // Whether job `id` may run: it is first among the jobs that name each of
  // its namespaces.
  bool MayRun(uint64_t id) const {
    auto it = jobs_.find(id);
    return it != jobs_.end() && IsFirst(it->second.a, id) &&
           IsFirst(it->second.b, id);
  }

  // Job `id` ended. Returns the jobs that may run now and couldn't before,
  // in the order they came.
  std::vector<uint64_t> Done(uint64_t id) {
    auto it = jobs_.find(id);
    if (it == jobs_.end()) {
      return {};
    }
    const Job job = std::move(it->second);
    jobs_.erase(it);
    std::vector<uint64_t> ready;
    for (const std::string& name : {job.a, job.b}) {
      auto queue = queues_.find(name);
      if (queue == queues_.end()) {
        // `b` the same as `a`, already done.
        continue;
      }
      std::deque<uint64_t>& ids = queue->second;
      ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
      if (ids.empty()) {
        queues_.erase(queue);
        continue;
      }
      const uint64_t next = ids.front();
      if (MayRun(next) &&
          std::find(ready.begin(), ready.end(), next) == ready.end()) {
        ready.push_back(next);
      }
    }
    std::sort(ready.begin(), ready.end());
    return ready;
  }

  // Whether a job (under way or waiting) names `namespace_id`.
  bool Names(std::string_view namespace_id) const {
    return queues_.find(namespace_id) != queues_.end();
  }

  size_t size() const { return jobs_.size(); }

 private:
  struct Job {
    std::string a;
    std::string b;
  };

  bool IsFirst(const std::string& name, uint64_t id) const {
    auto queue = queues_.find(name);
    return queue != queues_.end() && !queue->second.empty() &&
           queue->second.front() == id;
  }

  std::map<uint64_t, Job> jobs_;
  // For each namespace, the jobs that name it, in the order they came.
  std::map<std::string, std::deque<uint64_t>, std::less<>> queues_;
  uint64_t next_id_ = 0;
};

// How long a copy waits for its sites' turns at most, once it has asked for
// them.
inline constexpr int64_t kMoltDomStorageTurnsWaitMs = 1000;

// Copies waiting for their sites' turns, by the caller's ids: each is over
// once, at its last turn or when its time is up, whichever comes first.
class MoltDomStorageTurnWaits {
 public:
  MoltDomStorageTurnWaits() = default;
  MoltDomStorageTurnWaits(const MoltDomStorageTurnWaits&) = delete;
  MoltDomStorageTurnWaits& operator=(const MoltDomStorageTurnWaits&) = delete;

  // Wait `id` waits for `turns` turns (more than none).
  void Wait(uint64_t id, size_t turns) { turns_left_[id] = turns; }

  // A turn of wait `id` came: whether the wait is over now (its last turn,
  // and it wasn't over before).
  bool TurnTaken(uint64_t id) {
    auto it = turns_left_.find(id);
    if (it == turns_left_.end() || it->second == 0) {
      return false;
    }
    return --it->second == 0;
  }

  // Wait `id`'s time is up: whether it is over now (it wasn't before).
  bool TimedOut(uint64_t id) {
    auto it = turns_left_.find(id);
    if (it == turns_left_.end() || it->second == 0) {
      return false;
    }
    it->second = 0;
    return true;
  }

  // Wait `id`'s operation ran: it is forgotten, and its turns still to come
  // count for nothing.
  void Forget(uint64_t id) { turns_left_.erase(id); }

  size_t size() const { return turns_left_.size(); }

 private:
  std::map<uint64_t, size_t> turns_left_;
};

}  // namespace storage

#endif  // COMPONENTS_SERVICES_STORAGE_DOM_STORAGE_MOLT_DOM_STORAGE_WAITS_H_
