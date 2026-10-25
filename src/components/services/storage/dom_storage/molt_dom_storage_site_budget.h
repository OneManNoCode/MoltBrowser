// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: a partition's per-site DOM storage budget (one for its
// localStorage, one for its sessionStorage), for partitions that keep caches
// per site (MoltNet's): what each site's areas hold together, its commit
// rate, and the memory valve's growth signal past the floor. The rules are
// in molt_dom_storage_site_policy.h; LocalStorageImpl and SessionStorageImpl
// apply them through StorageAreaImpl::Delegate.

#ifndef COMPONENTS_SERVICES_STORAGE_DOM_STORAGE_MOLT_DOM_STORAGE_SITE_BUDGET_H_
#define COMPONENTS_SERVICES_STORAGE_DOM_STORAGE_MOLT_DOM_STORAGE_SITE_BUDGET_H_

#include <map>
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
  MoltDomStorageSiteBudget() = default;
  MoltDomStorageSiteBudget(const MoltDomStorageSiteBudget&) = delete;
  MoltDomStorageSiteBudget& operator=(const MoltDomStorageSiteBudget&) =
      delete;

  // Whether `area` of `site` may grow to `new_bytes` (counted if so); past
  // the floor, the valve hears of the growth.
  bool MayGrowTo(std::string_view area,
                 std::string_view site,
                 size_t new_bytes) {
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
  void ShrankTo(std::string_view area, size_t new_bytes) {
    totals_.ShrankTo(area, new_bytes);
  }
  void Forget(std::string_view area) { totals_.Forget(area); }

  // How long the next commit of an area of `site` waits for the site's rate.
  base::TimeDelta CommitDelay(std::string_view site) {
    const MoltDomStorageSiteRate& rate = RateOf(site);
    return base::Milliseconds(MoltDomStorageSiteCommitDelayMs(
        rate.commits, rate.bytes, NowMs() - rate.start_ms));
  }
  void Committed(std::string_view site, size_t bytes) {
    MoltDomStorageSiteRate& rate = RateOf(site);
    ++rate.commits;
    rate.bytes += static_cast<int64_t>(bytes);
  }

  void SetGrowthObserver(
      mojo::PendingRemote<mojom::MoltDomStorageGrowthObserver> observer) {
    growth_observer_.reset();
    growth_observer_.Bind(std::move(observer));
  }

  const MoltDomStorageSiteTotals& totals() const { return totals_; }

 private:
  static int64_t NowMs() {
    return (base::TimeTicks::Now() - base::TimeTicks()).InMilliseconds();
  }
  MoltDomStorageSiteRate& RateOf(std::string_view site) {
    auto it = rates_.find(site);
    if (it == rates_.end()) {
      it = rates_.emplace(std::string(site), MoltDomStorageSiteRate{NowMs()})
               .first;
    }
    return it->second;
  }

  MoltDomStorageSiteTotals totals_;
  std::map<std::string, MoltDomStorageSiteRate, std::less<>> rates_;
  mojo::Remote<mojom::MoltDomStorageGrowthObserver> growth_observer_;
};

}  // namespace storage

#endif  // COMPONENTS_SERVICES_STORAGE_DOM_STORAGE_MOLT_DOM_STORAGE_SITE_BUDGET_H_
