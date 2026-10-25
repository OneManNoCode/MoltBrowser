// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: MoltNet's connection budget and SOCKS handshake budget, the
// share of Tor's capacity MoltNet's network contexts draw on
// (molt_per_site_socket_pool_policy.h says why and holds the numbers and
// decisions, std-only). Header-only: //net's sources are Chromium's
// BUILD.gn, which MoltBrowser doesn't change.
//
//  * MoltBudgetSiteFor(): which site a network key's connections count
//    against.
//  * MoltTorCapacity: one for the process, shared by every HttpNetworkSession
//    that has per-site socket pools (MoltNet's network contexts), because
//    they all go to the one Tor MoltBrowser runs: an identity that New
//    Identity is ending and the next one draw on the same budgets. Each
//    session's sites count as sites of their own (MoltBudgetOwner).
//  * MoltConnectionBudget: all per-site pools (normal and WebSocket, every
//    session) together stay within kMoltNetConnectionBudget, in two parts:
//    the certificate-fetch pools within kMoltCertFetchConnectionBudget,
//    every other pool within kMoltMainConnectionBudget (a budget of each
//    size in MoltTorCapacity). A pool asks its part
//    before each new socket (ClientSocketPool::MoltSocketGate, hooked into
//    ClientSocketPool::UpdateExpandabilityBeforeAllocation()); a refused
//    pool's request waits in the pool as for the pool's own limit, and is
//    retried when a socket is released anywhere, the waiting site using the
//    fewest connections first. While the budget is full, an idle connection
//    of a site using more than the waiting one is closed for it.
//  * MoltSocksHandshakeBudget, and a session's view of it,
//    MoltSocksHandshakeLimiter: SOCKS handshakes in progress, by weight,
//    per site and for all sites together (SOCKSConnectJob waits for its
//    site's slots before it connects to Tor).

#ifndef NET_SOCKET_MOLT_CONNECTION_BUDGET_H_
#define NET_SOCKET_MOLT_CONNECTION_BUDGET_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/location.h"
#include "base/memory/ptr_util.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "net/base/network_anonymization_key.h"
#include "net/base/request_priority.h"
#include "net/base/schemeful_site.h"
#include "net/socket/molt_per_site_socket_pool_policy.h"
#include "url/gurl.h"
#include "url/scheme_host_port.h"

namespace net {

// The site a network key's connections count against in MoltNet's context:
// the key's top-level site, for its first-party and third-party keys alike;
// for an opaque top-level site (a page its server sandboxed), the site the
// opaque origin was derived from. nullopt for the keys no site can be named
// for (a one-off key derived from nothing: net::IsolationInfo::
// CreateTransient()).
using MoltBudgetSite = std::optional<SchemefulSite>;

inline MoltBudgetSite MoltBudgetSiteFor(const NetworkAnonymizationKey& key) {
  const std::optional<SchemefulSite>& top_frame_site = key.GetTopFrameSite();
  if (!top_frame_site) {
    return std::nullopt;
  }
  if (!top_frame_site->opaque()) {
    return *top_frame_site;
  }
  const url::SchemeHostPort precursor =
      top_frame_site->MoltGetTupleOrPrecursorTupleIfOpaque();
  if (!precursor.IsValid()) {
    return std::nullopt;
  }
  SchemefulSite site(precursor.GetURL());
  if (site.opaque()) {
    return std::nullopt;
  }
  return site;
}

// A site of one session (one network context): what the budgets count, and
// keep apart.
using MoltBudgetOwner = std::pair<uintptr_t, MoltBudgetSite>;

class MoltConnectionBudget {
 public:
  // The per-site pools of one pool type of one session that draw on the
  // budget (MoltPerSiteSocketPools).
  class Member {
   public:
    // Adds each site's sockets in use (handed out, idle or connecting) to
    // `loads`, and to `within_limits` those within its pools' limits: what
    // the budget counts (sockets past a limit come out of Tor's margin,
    // kMoltIgnoreLimitsOverflow).
    virtual void MoltAddLoads(std::map<MoltBudgetSite, size_t>& loads,
                              size_t& within_limits) const = 0;
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

  // `owner` identifies the member's session: the same for its normal and
  // WebSocket pools, different for every other session.
  void AddMember(Member* member, uintptr_t owner) {
    members_.push_back(MemberEntry{member, owner});
  }
  void RemoveMember(Member* member) {
    std::erase_if(members_,
                  [member](const MemberEntry& m) {
                    return m.member == member;
                  });
    std::erase_if(waiters_,
                  [member](const Waiter& w) { return w.member == member; });
  }

  // Whether `site`'s pool in `member` may open one more socket now. If not,
  // the pool's request waits, and the budget gets back to it.
  bool MayAllocate(Member* member, const MoltBudgetSite& site) {
    const Loads loads = GetLoads();
    const MoltBudgetOwner owner{OwnerOf(member), site};
    const std::optional<size_t> smallest_waiting =
        SmallestLiveWaiter(loads, member, site);
    if (MoltBudgetAdmits(loads.total, budget_, LoadOf(loads, owner),
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
  size_t MemberCountForTesting() const { return members_.size(); }

 private:
  struct MemberEntry {
    raw_ptr<Member> member;
    uintptr_t owner;
  };
  struct Loads {
    // What the budget counts: sockets within their pools' limits.
    size_t total = 0;
    // Each owner's sockets, past a limit too: a site's own count.
    std::map<MoltBudgetOwner, size_t> per_owner;
  };
  struct Waiter {
    raw_ptr<Member> member;
    MoltBudgetSite site;
  };

  uintptr_t OwnerOf(const Member* member) const {
    for (const MemberEntry& m : members_) {
      if (m.member == member) {
        return m.owner;
      }
    }
    return 0;
  }

  Loads GetLoads() const {
    Loads loads;
    for (const MemberEntry& m : members_) {
      std::map<MoltBudgetSite, size_t> member_loads;
      size_t within_limits = 0;
      m.member->MoltAddLoads(member_loads, within_limits);
      for (auto& [site, in_use] : member_loads) {
        loads.per_owner[MoltBudgetOwner{m.owner, site}] += in_use;
      }
      loads.total += within_limits;
    }
    return loads;
  }

  static size_t LoadOf(const Loads& loads, const MoltBudgetOwner& owner) {
    auto it = loads.per_owner.find(owner);
    return it == loads.per_owner.end() ? 0 : it->second;
  }

  bool IsMember(const Member* member) const {
    for (const MemberEntry& m : members_) {
      if (m.member == member) {
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

  // The fewest connections an owner other than (`member`'s session, `site`)
  // that still waits uses.
  std::optional<size_t> SmallestLiveWaiter(const Loads& loads,
                                           const Member* member,
                                           const MoltBudgetSite& site) {
    PruneDeadWaiters();
    const MoltBudgetOwner self{OwnerOf(member), site};
    std::optional<size_t> smallest;
    for (const Waiter& w : waiters_) {
      const MoltBudgetOwner owner{OwnerOf(w.member), w.site};
      if (owner == self) {
        continue;
      }
      const size_t in_use = LoadOf(loads, owner);
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
      in_use.push_back(LoadOf(loads, {OwnerOf(w.member), w.site}));
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

  // While the budget is full: closes an idle connection of the owner using
  // the most, if it uses more than one more than the waiting owner using
  // the fewest; releasing it wakes the waiting site.
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
    size_t waiter_in_use = SIZE_MAX;
    for (const Waiter& w : waiters_) {
      waiter_in_use =
          std::min(waiter_in_use, LoadOf(loads, {OwnerOf(w.member), w.site}));
    }
    std::vector<MoltBudgetOwner> owners;
    std::vector<size_t> in_use;
    for (const auto& [owner, n] : loads.per_owner) {
      owners.push_back(owner);
      in_use.push_back(n);
    }
    const std::vector<size_t> order =
        MoltPreemptionOrder(in_use, waiter_in_use);
    std::vector<MemberEntry> members(members_.begin(), members_.end());
    for (size_t i : order) {
      for (const MemberEntry& m : members) {
        if (m.owner == owners[i].first && IsMember(m.member) &&
            m.member->MoltCloseOneIdleSocket(owners[i].second)) {
          return;
        }
      }
    }
    for (size_t i : order) {
      bool asked = false;
      for (const MemberEntry& m : members) {
        if (m.owner == owners[i].first && IsMember(m.member) &&
            m.member->MoltCloseIdleSessions(owners[i].second)) {
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
  std::vector<MemberEntry> members_;
  // Oldest first.
  std::vector<Waiter> waiters_;
  bool wake_posted_ = false;
  bool preempt_posted_ = false;
  base::WeakPtrFactory<MoltConnectionBudget> weak_factory_{this};
};

// SOCKS handshakes to MoltNet's Tor in progress, by weight
// (MoltSocksHandshakeWeight()), for each site of each session
// (MoltSocksHandshakeLimit()) and for all of them together
// (kMoltSocksHandshakeBudget), so that the circuits MoltNet's sites can make
// Tor build at a time stay below Tor's own limit, and one site, or a group,
// can't keep another's from being built.
class MoltSocksHandshakeBudget {
 public:
  // A handshake's slots, held by a SOCKSConnectJob from before it connects
  // to Tor until its handshake is done. Destroying it gives the slots back,
  // or leaves the queue.
  class Slot {
   public:
    Slot(const Slot&) = delete;
    Slot& operator=(const Slot&) = delete;
    ~Slot() {
      if (budget_) {
        budget_->OnSlotGone(*this);
      }
    }

    bool granted() const { return granted_; }

    void SetPriority(RequestPriority priority) {
      if (budget_ && !granted_) {
        budget_->OnGranted(budget_->slots_.SetPriority(
            owner_, id_, static_cast<int>(priority)));
      }
    }

   private:
    friend class MoltSocksHandshakeBudget;

    Slot(base::WeakPtr<MoltSocksHandshakeBudget> budget,
         MoltBudgetOwner owner,
         size_t weight,
         uint64_t id,
         base::OnceClosure on_granted)
        : budget_(std::move(budget)),
          owner_(std::move(owner)),
          weight_(weight),
          id_(id),
          on_granted_(std::move(on_granted)) {}

    void RunGranted() {
      if (on_granted_) {
        std::move(on_granted_).Run();
      }
    }

    base::WeakPtr<MoltSocksHandshakeBudget> budget_;
    const MoltBudgetOwner owner_;
    const size_t weight_;
    const uint64_t id_;
    bool granted_ = false;
    base::OnceClosure on_granted_;
    base::WeakPtrFactory<Slot> weak_factory_{this};
  };

  explicit MoltSocksHandshakeBudget(size_t budget) : slots_(budget) {}
  MoltSocksHandshakeBudget(const MoltSocksHandshakeBudget&) = delete;
  MoltSocksHandshakeBudget& operator=(const MoltSocksHandshakeBudget&) =
      delete;
  ~MoltSocksHandshakeBudget() = default;

  // Slots of `weight` for a handshake of `owner`: granted() at once if the
  // owner and the budget have room and no owner holding fewer waits, else
  // `on_granted` runs (posted) when they are given.
  std::unique_ptr<Slot> Acquire(const MoltBudgetOwner& owner,
                                size_t weight,
                                RequestPriority priority,
                                base::OnceClosure on_granted) {
    const uint64_t id = next_id_++;
    const size_t limit = MoltSocksHandshakeLimit(!owner.second.has_value());
    const size_t w = std::clamp<size_t>(weight, 1, limit);
    std::unique_ptr<Slot> slot = base::WrapUnique(new Slot(
        weak_factory_.GetWeakPtr(), owner, w, id, std::move(on_granted)));
    waiting_[id] = slot.get();
    OnGranted(
        slots_.Acquire(owner, limit, w, id, static_cast<int>(priority)),
        id);
    return slot;
  }

  size_t InProgressForTesting(const MoltBudgetOwner& owner) const {
    return slots_.InUse(owner);
  }
  size_t WaitingForTesting(const MoltBudgetOwner& owner) const {
    return slots_.Waiting(owner);
  }
  size_t TotalInProgressForTesting() const { return slots_.Total(); }

 private:
  void OnSlotGone(Slot& slot) {
    if (!slot.granted_) {
      waiting_.erase(slot.id_);
      OnGranted(slots_.Cancel(slot.owner_, slot.id_));
      return;
    }
    OnGranted(slots_.Release(slot.owner_, slot.weight_));
  }

  // Marks `granted` (every queued id has a live Slot in `waiting_`: a Slot
  // leaves the queue when it goes) and lets them go on, posted; `self` was
  // granted within its own Acquire(), which reports it at once.
  void OnGranted(const std::vector<uint64_t>& granted,
                 std::optional<uint64_t> self = std::nullopt) {
    for (uint64_t id : granted) {
      auto it = waiting_.find(id);
      if (it == waiting_.end()) {
        continue;
      }
      Slot* slot = it->second;
      waiting_.erase(it);
      slot->granted_ = true;
      if (self && *self == id) {
        continue;
      }
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE,
          base::BindOnce(&Slot::RunGranted, slot->weak_factory_.GetWeakPtr()));
    }
  }

  MoltWeightedSlots<MoltBudgetOwner, uint64_t> slots_;
  std::map<uint64_t, raw_ptr<Slot>> waiting_;
  uint64_t next_id_ = 1;
  base::WeakPtrFactory<MoltSocksHandshakeBudget> weak_factory_{this};
};

// Tor's capacity as MoltNet's network contexts share it: one for the
// process, made when the first session with per-site pools needs it and
// gone with the last.
class MoltTorCapacity : public base::RefCounted<MoltTorCapacity> {
 public:
  // The process's, made if there is none.
  static scoped_refptr<MoltTorCapacity> GetShared() {
    if (MoltTorCapacity* shared = SharedInstance()) {
      return base::WrapRefCounted(shared);
    }
    scoped_refptr<MoltTorCapacity> created =
        base::MakeRefCounted<MoltTorCapacity>();
    SharedInstance() = created.get();
    return created;
  }

  MoltTorCapacity()
      : connection_budget_(kMoltMainConnectionBudget),
        cert_fetch_budget_(kMoltCertFetchConnectionBudget),
        handshake_budget_(kMoltSocksHandshakeBudget) {}
  MoltTorCapacity(const MoltTorCapacity&) = delete;
  MoltTorCapacity& operator=(const MoltTorCapacity&) = delete;

  // Every per-site pool but the certificate-fetch pools draws on
  // connection_budget(); those on cert_fetch_budget(), kept apart so that
  // connections waiting for their certificate checks never hold back the
  // downloads the checks wait for.
  MoltConnectionBudget& connection_budget() { return connection_budget_; }
  MoltConnectionBudget& cert_fetch_budget() { return cert_fetch_budget_; }
  MoltSocksHandshakeBudget& handshake_budget() { return handshake_budget_; }

 private:
  friend class base::RefCounted<MoltTorCapacity>;
  ~MoltTorCapacity() {
    if (SharedInstance() == this) {
      SharedInstance() = nullptr;
    }
  }

  static MoltTorCapacity*& SharedInstance() {
    static MoltTorCapacity* instance = nullptr;
    return instance;
  }

  MoltConnectionBudget connection_budget_;
  MoltConnectionBudget cert_fetch_budget_;
  MoltSocksHandshakeBudget handshake_budget_;
};

// One session's handshake slots: its sites are owners of their own in the
// process's MoltSocksHandshakeBudget (CommonConnectJobParams::
// molt_socks_handshake_limiter, read by SOCKSConnectJob).
class MoltSocksHandshakeLimiter {
 public:
  using Slot = MoltSocksHandshakeBudget::Slot;

  // `owner` identifies the session.
  MoltSocksHandshakeLimiter(scoped_refptr<MoltTorCapacity> capacity,
                            uintptr_t owner)
      : capacity_(std::move(capacity)), owner_(owner) {}
  MoltSocksHandshakeLimiter(const MoltSocksHandshakeLimiter&) = delete;
  MoltSocksHandshakeLimiter& operator=(const MoltSocksHandshakeLimiter&) =
      delete;
  ~MoltSocksHandshakeLimiter() = default;

  // Slots of `weight` for a handshake of `site` (MoltSocksHandshakeBudget::
  // Acquire()).
  std::unique_ptr<Slot> Acquire(const MoltBudgetSite& site,
                                size_t weight,
                                RequestPriority priority,
                                base::OnceClosure on_granted) {
    return capacity_->handshake_budget().Acquire(
        MoltBudgetOwner{owner_, site}, weight, priority,
        std::move(on_granted));
  }

  size_t InProgressForTesting(const MoltBudgetSite& site) const {
    return capacity_->handshake_budget().InProgressForTesting(
        MoltBudgetOwner{owner_, site});
  }
  size_t WaitingForTesting(const MoltBudgetSite& site) const {
    return capacity_->handshake_budget().WaitingForTesting(
        MoltBudgetOwner{owner_, site});
  }

 private:
  const scoped_refptr<MoltTorCapacity> capacity_;
  const uintptr_t owner_;
};

}  // namespace net

#endif  // NET_SOCKET_MOLT_CONNECTION_BUDGET_H_
