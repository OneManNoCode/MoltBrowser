// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: MoltNet's connection budget and SOCKS handshake limits, one of
// each per HttpNetworkSession that has per-site socket pools (MoltNet's
// network context; molt_per_site_socket_pool_policy.h says why and holds the
// numbers and decisions, std-only). Header-only: //net's sources are
// Chromium's BUILD.gn, which MoltBrowser doesn't change.
//
//  * MoltBudgetSiteFor(): which site a network key's connections count
//    against.
//  * MoltConnectionBudget: all per-site pools of the session (normal and
//    WebSocket) together stay within kMoltNetConnectionBudget. A pool asks it
//    before each new socket (ClientSocketPool::MoltSocketGate, hooked into
//    ClientSocketPool::UpdateExpandabilityBeforeAllocation()); a refused
//    pool's request waits in the pool as for the pool's own limit, and is
//    retried when a socket is released anywhere, the waiting site using the
//    fewest connections first. While the budget is full, an idle connection
//    of a site using more than the waiting one is closed for it.
//  * MoltSocksHandshakeLimiter: each site's SOCKS handshakes in progress
//    (SOCKSConnectJob waits for a slot before it connects to Tor).

#ifndef NET_SOCKET_MOLT_CONNECTION_BUDGET_H_
#define NET_SOCKET_MOLT_CONNECTION_BUDGET_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/location.h"
#include "base/memory/ptr_util.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "net/base/network_anonymization_key.h"
#include "net/base/request_priority.h"
#include "net/base/schemeful_site.h"
#include "net/socket/molt_per_site_socket_pool_policy.h"
#include "url/gurl.h"

namespace net {

// The site a network key's connections count against in MoltNet's context:
// the key's top-level site, for its first-party and third-party keys alike;
// for an opaque top-level site (a page its server sandboxed), the site the
// opaque origin was derived from. nullopt for the one-off keys no site can
// be named for (certificate fetches: net::IsolationInfo::CreateTransient()).
using MoltBudgetSite = std::optional<SchemefulSite>;

inline MoltBudgetSite MoltBudgetSiteFor(const NetworkAnonymizationKey& key) {
  const std::optional<SchemefulSite>& top_frame_site = key.GetTopFrameSite();
  if (!top_frame_site) {
    return std::nullopt;
  }
  if (!top_frame_site->opaque()) {
    return *top_frame_site;
  }
  // SchemefulSite doesn't expose an opaque origin's precursor outside a few
  // friends; its debug string names it (url::Origin::GetDebugString()).
  const std::string debug_string = top_frame_site->GetDebugString();
  const std::optional<std::string_view> precursor =
      MoltOpaqueOriginPrecursor(debug_string);
  if (!precursor) {
    return std::nullopt;
  }
  const GURL url{std::string(*precursor)};
  if (!url.is_valid()) {
    return std::nullopt;
  }
  SchemefulSite site(url);
  if (site.opaque()) {
    return std::nullopt;
  }
  return site;
}

class MoltConnectionBudget {
 public:
  // The per-site pools of one pool type that draw on the budget
  // (MoltPerSiteSocketPools).
  class Member {
   public:
    // Adds each site's sockets in use (handed out, idle or connecting).
    virtual void MoltAddLoads(
        std::map<MoltBudgetSite, size_t>& loads) const = 0;
    // Whether a request of `site` waits for a socket slot.
    virtual bool MoltHasStalledRequest(const MoltBudgetSite& site) const = 0;
    // Lets `site`'s pools retry the requests that wait for a slot.
    virtual void MoltRetryStalledRequests(const MoltBudgetSite& site) = 0;
    // Closes one idle socket of `site`; true if one was closed.
    virtual bool MoltCloseOneIdleSocket(const MoltBudgetSite& site) = 0;
    // Asks `site`'s HTTP/2 sessions to close if they are idle (they close
    // later); true if it has any to ask.
    virtual bool MoltCloseIdleSessions(const MoltBudgetSite& site) = 0;

   protected:
    virtual ~Member() = default;
  };

  explicit MoltConnectionBudget(size_t budget) : budget_(budget) {}
  MoltConnectionBudget(const MoltConnectionBudget&) = delete;
  MoltConnectionBudget& operator=(const MoltConnectionBudget&) = delete;
  ~MoltConnectionBudget() = default;

  void AddMember(Member* member) { members_.push_back(member); }
  void RemoveMember(Member* member) {
    std::erase(members_, member);
    std::erase_if(waiters_,
                  [member](const Waiter& w) { return w.member == member; });
  }

  // Whether `site`'s pool in `member` may open one more socket now. If not,
  // the pool's request waits, and the budget gets back to it.
  bool MayAllocate(Member* member, const MoltBudgetSite& site) {
    const Loads loads = GetLoads();
    const std::optional<size_t> smallest_waiting =
        SmallestLiveWaiter(loads, member, site);
    if (MoltBudgetAdmits(loads.total, budget_, LoadOf(loads, site),
                         smallest_waiting)) {
      RemoveWaiter(member, site);
      return true;
    }
    AddWaiter(member, site);
    PostPreempt(base::TimeDelta());
    return false;
  }

  // A socket was released somewhere (or connect jobs were cancelled).
  void OnReleased() {
    if (!waiters_.empty()) {
      PostWake();
    }
  }

  size_t budget() const { return budget_; }
  size_t TotalInUseForTesting() const { return GetLoads().total; }
  size_t WaiterCountForTesting() const { return waiters_.size(); }

 private:
  struct Loads {
    size_t total = 0;
    std::map<MoltBudgetSite, size_t> per_site;
  };
  struct Waiter {
    raw_ptr<Member> member;
    MoltBudgetSite site;
  };

  Loads GetLoads() const {
    Loads loads;
    for (Member* member : members_) {
      member->MoltAddLoads(loads.per_site);
    }
    for (const auto& [site, in_use] : loads.per_site) {
      loads.total += in_use;
    }
    return loads;
  }

  static size_t LoadOf(const Loads& loads, const MoltBudgetSite& site) {
    auto it = loads.per_site.find(site);
    return it == loads.per_site.end() ? 0 : it->second;
  }

  bool IsMember(const Member* member) const {
    for (const Member* m : members_) {
      if (m == member) {
        return true;
      }
    }
    return false;
  }

  void AddWaiter(Member* member, const MoltBudgetSite& site) {
    for (const Waiter& w : waiters_) {
      if (w.member == member && w.site == site) {
        return;
      }
    }
    waiters_.push_back(Waiter{member, site});
  }

  void RemoveWaiter(Member* member, const MoltBudgetSite& site) {
    std::erase_if(waiters_, [&](const Waiter& w) {
      return w.member == member && w.site == site;
    });
  }

  // Forgets waiters whose requests went (cancelled, or served otherwise).
  void PruneDeadWaiters() {
    std::erase_if(waiters_, [](const Waiter& w) {
      return !w.member->MoltHasStalledRequest(w.site);
    });
  }

  // The fewest connections a site other than (`member`, `site`) that still
  // waits uses.
  std::optional<size_t> SmallestLiveWaiter(const Loads& loads,
                                           const Member* member,
                                           const MoltBudgetSite& site) {
    PruneDeadWaiters();
    std::optional<size_t> smallest;
    for (const Waiter& w : waiters_) {
      if (w.member == member && w.site == site) {
        continue;
      }
      const size_t in_use = LoadOf(loads, w.site);
      if (!smallest || in_use < *smallest) {
        smallest = in_use;
      }
    }
    return smallest;
  }

  void PostWake() {
    if (wake_posted_) {
      return;
    }
    wake_posted_ = true;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&MoltConnectionBudget::Wake,
                                  weak_factory_.GetWeakPtr()));
  }

  void PostPreempt(base::TimeDelta delay) {
    if (preempt_posted_) {
      return;
    }
    preempt_posted_ = true;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&MoltConnectionBudget::Preempt,
                       weak_factory_.GetWeakPtr()),
        delay);
  }

  // Gives room in the budget to the waiting sites, the one using the fewest
  // first.
  void Wake() {
    wake_posted_ = false;
    PruneDeadWaiters();
    const Loads loads = GetLoads();
    if (loads.total >= budget_ || waiters_.empty()) {
      return;
    }
    const std::vector<Waiter> waiters = waiters_;
    std::vector<size_t> in_use;
    for (const Waiter& w : waiters) {
      in_use.push_back(LoadOf(loads, w.site));
    }
    for (size_t i : MoltWakeOrder(in_use)) {
      const Waiter& w = waiters[i];
      if (!IsMember(w.member)) {
        continue;
      }
      if (GetLoads().total >= budget_) {
        break;
      }
      // Retrying asks MayAllocate() again, which puts it back if it still
      // can't have one.
      RemoveWaiter(w.member, w.site);
      w.member->MoltRetryStalledRequests(w.site);
    }
  }

  // While the budget is full: closes an idle connection of the site using
  // the most, if it uses more than one more than the waiting site using the
  // fewest; releasing it wakes the waiting site.
  void Preempt() {
    preempt_posted_ = false;
    PruneDeadWaiters();
    if (waiters_.empty()) {
      return;
    }
    const Loads loads = GetLoads();
    if (loads.total < budget_) {
      PostWake();
      return;
    }
    size_t waiter_in_use = LoadOf(loads, waiters_.front().site);
    for (const Waiter& w : waiters_) {
      waiter_in_use = std::min(waiter_in_use, LoadOf(loads, w.site));
    }
    std::vector<MoltBudgetSite> sites;
    std::vector<size_t> in_use;
    for (const auto& [site, n] : loads.per_site) {
      sites.push_back(site);
      in_use.push_back(n);
    }
    const std::vector<size_t> order = MoltPreemptionOrder(in_use, waiter_in_use);
    const std::vector<Member*> members(members_.begin(), members_.end());
    for (size_t i : order) {
      for (Member* member : members) {
        if (IsMember(member) && member->MoltCloseOneIdleSocket(sites[i])) {
          return;
        }
      }
    }
    for (size_t i : order) {
      bool asked = false;
      for (Member* member : members) {
        if (IsMember(member) && member->MoltCloseIdleSessions(sites[i])) {
          asked = true;
        }
      }
      if (asked) {
        // Idle HTTP/2 sessions close a little later; look again then.
        PostPreempt(base::Seconds(1));
        return;
      }
    }
  }

  const size_t budget_;
  std::vector<raw_ptr<Member>> members_;
  // Oldest first.
  std::vector<Waiter> waiters_;
  bool wake_posted_ = false;
  bool preempt_posted_ = false;
  base::WeakPtrFactory<MoltConnectionBudget> weak_factory_{this};
};

// Each site's SOCKS handshakes to MoltNet's Tor in progress
// (MoltSocksHandshakeLimit()), so one site can make Tor build only a few
// circuits at a time.
class MoltSocksHandshakeLimiter {
 public:
  // A site's handshake slot, held by a SOCKSConnectJob from before it
  // connects to Tor until its handshake is done. Destroying it gives the
  // slot back, or leaves the queue.
  class Slot {
   public:
    Slot(const Slot&) = delete;
    Slot& operator=(const Slot&) = delete;
    ~Slot() {
      if (limiter_) {
        limiter_->OnSlotGone(*this);
      }
    }

    bool granted() const { return granted_; }

    void SetPriority(RequestPriority priority) {
      if (limiter_ && !granted_) {
        limiter_->queue_.SetPriority(site_, id_, static_cast<int>(priority));
      }
    }

   private:
    friend class MoltSocksHandshakeLimiter;

    Slot(base::WeakPtr<MoltSocksHandshakeLimiter> limiter,
         MoltBudgetSite site,
         uint64_t id,
         base::OnceClosure on_granted)
        : limiter_(std::move(limiter)),
          site_(std::move(site)),
          id_(id),
          on_granted_(std::move(on_granted)) {}

    void RunGranted() {
      if (on_granted_) {
        std::move(on_granted_).Run();
      }
    }

    base::WeakPtr<MoltSocksHandshakeLimiter> limiter_;
    const MoltBudgetSite site_;
    const uint64_t id_;
    bool granted_ = false;
    base::OnceClosure on_granted_;
    base::WeakPtrFactory<Slot> weak_factory_{this};
  };

  MoltSocksHandshakeLimiter() = default;
  MoltSocksHandshakeLimiter(const MoltSocksHandshakeLimiter&) = delete;
  MoltSocksHandshakeLimiter& operator=(const MoltSocksHandshakeLimiter&) =
      delete;
  ~MoltSocksHandshakeLimiter() = default;

  // A slot for a handshake of `site`: granted() at once if the site has
  // fewer than its limit in progress, else `on_granted` runs (posted) when
  // one is given back.
  std::unique_ptr<Slot> Acquire(const MoltBudgetSite& site,
                                RequestPriority priority,
                                base::OnceClosure on_granted) {
    const uint64_t id = next_id_++;
    std::unique_ptr<Slot> slot = base::WrapUnique(
        new Slot(weak_factory_.GetWeakPtr(), site, id, std::move(on_granted)));
    if (queue_.Acquire(site, MoltSocksHandshakeLimit(!site.has_value()), id,
                       static_cast<int>(priority))) {
      slot->granted_ = true;
    } else {
      waiting_[id] = slot.get();
    }
    return slot;
  }

  size_t InProgressForTesting(const MoltBudgetSite& site) const {
    return queue_.InUse(site);
  }
  size_t WaitingForTesting(const MoltBudgetSite& site) const {
    return queue_.Waiting(site);
  }

 private:
  void OnSlotGone(Slot& slot) {
    if (!slot.granted_) {
      queue_.Cancel(slot.site_, slot.id_);
      waiting_.erase(slot.id_);
      return;
    }
    // Hand the slot to the next one waiting, if any. Every queued id has a
    // live Slot in `waiting_` (a Slot leaves the queue when it goes).
    while (std::optional<uint64_t> next = queue_.Release(slot.site_)) {
      auto it = waiting_.find(*next);
      if (it == waiting_.end()) {
        continue;
      }
      Slot* granted = it->second;
      waiting_.erase(it);
      granted->granted_ = true;
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, base::BindOnce(&Slot::RunGranted,
                                    granted->weak_factory_.GetWeakPtr()));
      return;
    }
  }

  MoltSlotQueue<MoltBudgetSite, uint64_t> queue_;
  std::map<uint64_t, raw_ptr<Slot>> waiting_;
  uint64_t next_id_ = 1;
  base::WeakPtrFactory<MoltSocksHandshakeLimiter> weak_factory_{this};
};

}  // namespace net

#endif  // NET_SOCKET_MOLT_CONNECTION_BUDGET_H_
