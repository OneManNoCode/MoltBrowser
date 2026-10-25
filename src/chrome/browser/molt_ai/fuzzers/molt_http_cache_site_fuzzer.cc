// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet's HTTP cache budget per site
// (net/disk_cache/memory/molt_mem_backend_budget_policy.h) on cache keys:
// the key carries the page's URL and its sites, so what a page requests
// shapes it.
//
// Checks: no memory or UB error; the site read from any key is a part of
// it, holds no space, and is empty unless the key has the double-key form;
// a key built the way net::HttpCache::GenerateCacheKey() builds one, with
// any URL, frame site and prefixes, gives back its top-level site; and the
// budget's decisions agree with each other for any sizes.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <fuzzer/FuzzedDataProvider.h>

#include "base/check.h"
#include "net/disk_cache/memory/molt_mem_backend_budget_policy.h"

namespace {

// What a serialized SchemefulSite looks like: a scheme, "://", a host; no
// space.
std::string Site(FuzzedDataProvider& p) {
  std::string site = p.ConsumeBool() ? "https://" : "http://";
  std::string host = p.ConsumeRandomLengthString(80);
  for (char& c : host) {
    if (c == ' ') {
      c = '-';
    }
  }
  return site + host;
}

std::string NoSlash(std::string s) {
  for (char& c : s) {
    if (c == '/') {
      c = '_';
    }
  }
  return s;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace disk_cache;
  FuzzedDataProvider p(data, size);

  // Any key.
  const std::string any = p.ConsumeRandomLengthString(1024);
  const std::string_view site = MoltHttpCacheSiteOfKey(any);
  CHECK(site.find(' ') == std::string_view::npos);
  if (!site.empty()) {
    CHECK(site.data() >= any.data() &&
          site.data() + site.size() <= any.data() + any.size());
    CHECK(any.find("/_dk_") != std::string::npos);
  }

  // A key as HttpCache makes one.
  const std::string top = Site(p);
  std::string key = NoSlash(p.ConsumeRandomLengthString(4)) + "/" +
                    NoSlash(p.ConsumeRandomLengthString(12)) + "/_dk_";
  if (p.ConsumeBool()) {
    key += "s_";
  }
  if (p.ConsumeBool()) {
    key += "cn_";
  }
  key += top + " " + Site(p) + " " + p.ConsumeRandomLengthString(512);
  CHECK(MoltHttpCacheSiteOfKey(key) == top);

  // The budget's decisions.
  const MoltCacheSiteBudget budget = MoltNetHttpCacheBudget();
  CHECK(MoltUsesCacheSiteBudget(budget));
  CHECK(!MoltUsesCacheSiteBudget(MoltCacheSiteBudget()));
  const int64_t site_size =
      p.ConsumeIntegralInRange<int64_t>(0, 4 * budget.total_bytes);
  const int64_t total_size =
      site_size + p.ConsumeIntegralInRange<int64_t>(0, 4 * budget.total_bytes);
  CHECK(MoltCacheSiteOverShare(site_size, budget) ==
        (site_size > budget.per_site_bytes));
  CHECK(MoltCacheSiteEvictionTarget(budget) < budget.per_site_bytes);
  CHECK(MoltCacheSiteEvictionTarget(budget) > 0);
  if (MoltCacheSiteOverShare(site_size, budget)) {
    CHECK(MoltCacheWriteRefused(site_size, total_size, budget));
  }
  if (!MoltCacheWriteRefused(site_size, total_size, budget)) {
    CHECK(site_size <= budget.per_site_bytes);
    CHECK(total_size <= budget.total_bytes);
  }
  const int64_t idle = p.ConsumeIntegral<int64_t>();
  CHECK(MoltCacheEntryExpired(idle, budget) ==
        (idle >= budget.idle_expiry_seconds));
  CHECK(!MoltCacheEntryExpired(idle, MoltCacheSiteBudget()));
  return 0;
}
