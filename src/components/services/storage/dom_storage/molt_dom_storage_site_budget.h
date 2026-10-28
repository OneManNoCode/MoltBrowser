// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: a partition's per-site DOM storage budget (one for its
// localStorage, one for its sessionStorage), for partitions that keep caches
// per site (MoltNet's): what each site's areas hold together, its commit
// rate, its write rate, and the memory valve's growth signal past the
// floor. The rules are in molt_dom_storage_site_policy.h; LocalStorageImpl
// and SessionStorageImpl apply them through StorageAreaImpl::Delegate.
//
// None of a site's records stays once it no longer limits anything: what
// its areas hold goes with the last item, its write allowance once it is
// full again and its commit rate once it lets a commit go at once (a record
// made afresh then is no looser, so emptying its storage gains a site
// nothing), a few due records at each operation
// (kMoltDomStorageForgetPerCall); and all of a group's at once when the
// group itself goes (ForgetSite(): a page's nonce, whose storage goes with
// the page).

#ifndef COMPONENTS_SERVICES_STORAGE_DOM_STORAGE_MOLT_DOM_STORAGE_SITE_BUDGET_H_
#define COMPONENTS_SERVICES_STORAGE_DOM_STORAGE_MOLT_DOM_STORAGE_SITE_BUDGET_H_

#include <string>
#include <string_view>
#include <utility>

#include "base/time/time.h"
#include "components/services/storage/dom_storage/molt_dom_storage_site_policy.h"
#include "components/services/storage/public/mojom/local_storage_control.mojom.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/remote.h"

namespace storage {

class MoltDomStorageSiteBudget {
 public:
  MoltDomStorageSiteBudget();
  MoltDomStorageSiteBudget(const MoltDomStorageSiteBudget&) = delete;
  MoltDomStorageSiteBudget& operator=(const MoltDomStorageSiteBudget&) =
      delete;
  ~MoltDomStorageSiteBudget();

  // Whether `area` of `site` may grow to `new_bytes` (counted if so); past
  // the floor, the valve hears of the growth.
  bool MayGrowTo(std::string_view area,
                 std::string_view site,
                 size_t new_bytes) {
    ForgetDue();
    const size_t before = totals_.total();
    if (!totals_.MayGrowTo(area, site, new_bytes)) {
      return false;
    }
    if (growth_observer_ &&
        MoltDomStorageGrowthNotifies(before, totals_.total())) {
      growth_observer_->OnGrowthBeyondFloor();
    }
    return true;
  }
  // `area` of `site` holds `new_bytes`, no more than it is counted for, or
  // is counted for the first time (a copy); past the floor, the valve hears
  // of that.
  void ShrankTo(std::string_view area,
                std::string_view site,
                size_t new_bytes) {
    ForgetDue();
    const size_t before = totals_.total();
    totals_.ShrankTo(area, site, new_bytes);
    if (growth_observer_ &&
        MoltDomStorageGrowthNotifies(before, totals_.total())) {
      growth_observer_->OnGrowthBeyondFloor();
    }
  }
  void Forget(std::string_view area) { totals_.Forget(area); }

  // How long the next commit of an area of `site` waits for the site's rate.
  base::TimeDelta CommitDelay(std::string_view site) {
    const int64_t now = ForgetDue();
    MoltDomStorageSiteRate& rate =
        rates_.GetOrAdd(site, MoltDomStorageSiteRate{now});
    rates_.SetForgetAt(site, MoltDomStorageSiteRateFreeAtMs(rate));
    return base::Milliseconds(MoltDomStorageSiteCommitDelayMs(
        rate.commits, rate.bytes, now - rate.start_ms));
  }
  void Committed(std::string_view site, size_t bytes) {
    const int64_t now = ForgetDue();
    MoltDomStorageSiteRate& rate =
        rates_.GetOrAdd(site, MoltDomStorageSiteRate{now});
    ++rate.commits;
    rate.bytes += static_cast<int64_t>(bytes);
    rates_.SetForgetAt(site, MoltDomStorageSiteRateFreeAtMs(rate));
  }

  // Whether a write of `bytes` to an area of `site` fits the site's write
  // allowance (taken if so).
  bool MayWrite(std::string_view site, size_t bytes) {
    const int64_t now = ForgetDue();
    MoltDomStorageWriteAllowance& allowance =
        writes_.GetOrAdd(site, MoltDomStorageWriteAllowance{now});
    const bool may = MoltDomStorageMayWrite(allowance, now, bytes);
    writes_.SetForgetAt(site, MoltDomStorageAllowanceFullAtMs(allowance));
    return may;
  }

  // `site` (a group: a page's nonce) is gone with everything it held: its
  // records go now.
  void ForgetSite(std::string_view site) {
    rates_.Forget(site);
    writes_.Forget(site);
  }

  void SetGrowthObserver(
      mojo::PendingRemote<mojom::MoltDomStorageGrowthObserver> observer) {
    growth_observer_.reset();
    growth_observer_.Bind(std::move(observer));
  }

  const MoltDomStorageSiteTotals& totals() const { return totals_; }
  size_t rate_count() const { return rates_.size(); }
  size_t allowance_count() const { return writes_.size(); }

 private:
  static int64_t NowMs() {
    return (base::TimeTicks::Now() - base::TimeTicks()).InMilliseconds();
  }
  // Forgets a few records that are due; returns the time it read.
  int64_t ForgetDue() {
    const int64_t now = NowMs();
    rates_.ForgetDue(now, kMoltDomStorageForgetPerCall);
    writes_.ForgetDue(now, kMoltDomStorageForgetPerCall);
    return now;
  }

  MoltDomStorageSiteTotals totals_;
  MoltDomStorageExpiringRecords<MoltDomStorageSiteRate> rates_;
  MoltDomStorageExpiringRecords<MoltDomStorageWriteAllowance> writes_;
  mojo::Remote<mojom::MoltDomStorageGrowthObserver> growth_observer_;
};

// Out of the class body, as Chromium's style asks of a class with members
// that have constructors.
inline MoltDomStorageSiteBudget::MoltDomStorageSiteBudget() = default;
inline MoltDomStorageSiteBudget::~MoltDomStorageSiteBudget() = default;

}  // namespace storage

#endif  // COMPONENTS_SERVICES_STORAGE_DOM_STORAGE_MOLT_DOM_STORAGE_SITE_BUDGET_H_
