// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: what MoltNet's cookie store keeps under a page's nonce.
//
// A credentialless iframe's requests, and those of the frames and documents
// it holds, carry its page's nonce, and their cookies are kept in partitions
// of their own: one for each site such a frame's document is of, keyed by
// that site and the nonce (net::CookiePartitionKey::
// FromNetworkIsolationKey(): the frame's site, kCrossSite), and holding the
// cookies of every domain the frame's requests reach. At Chromium 156 a
// partition holds at most 180 cookies and 10 KiB per domain
// (CookieMonster::kPerPartitionDomainMaxCookies,
// kPerPartitionDomainMaxCookieBytes) and nothing limits a partition's total
// ("TODO(crbug.com/40188414): Enforce global limit on partitioned cookies"),
// so a page's frames could keep adding domains (an onion address costs
// nothing to make) and sites (each navigation of a frame to a new site opens
// a partition) for as long as the page was open. The store also kept an
// entry per partition ever used (bytes_per_cookie_partition_, for a
// histogram), after its last cookie went.
//
// In MoltNet's network context (CookieMonster::MoltKeepNonceCookieLimits(),
// set when the context has NetworkContextParams::molt_nonce_limits):
//  * each nonce partition holds at most kMoltNoncePartitionMaxCookies
//    cookies and kMoltNoncePartitionMaxCookieBytes bytes, and a nonce's
//    partitions together at most kMoltNonceMaxCookies and
//    kMoltNonceMaxCookieBytes (bytes counted as Chromium counts them: a
//    cookie's name and value). Past either, the partition being written
//    evicts its own least recently used cookies (expired ones first), never
//    another partition's and never the cookie just set; a cookie whose
//    nonce's other partitions alone leave it no room is refused instead
//    (MoltNonceCookieRefused()). The checks sit where Chromium enforces its
//    own limits: right after a cookie is stored, after
//    CookieMonster::GarbageCollectPartitionedCookies();
//  * a nonce can be refused (CookieMonster::MoltDropNonce()): its cookies are
//    deleted and every cookie for its partitions is refused from then on, as
//    a cookie blocked by settings is, until the browser forgets it. The
//    browser refuses a nonce whose page lives on past its site's bound, and
//    one whose page has gone (chrome/browser/molt_ai/tor/
//    molt_net_nonce_storage_policy.h);
//  * a partition's entry in bytes_per_cookie_partition_ goes with its last
//    cookie.
// The numbers are far above what an embedded frame uses: Chromium lets one
// domain hold 180 cookies in a partition, and a typical embed's partition
// holds a few dozen.
//
// What it costs: the store keeps a record per nonce with cookies (its
// totals, and those of each of its partitions), and one per refused nonce
// until the browser forgets it. A cookie stored at a limit sorts the
// cookies of its own partition, at most kMoltNoncePartitionMaxCookies + 1.
//
// Std-only templates (Nonce: base::UnguessableToken, PartitionKey:
// net::CookiePartitionKey in the store), so molt_ai_unittests and a
// standalone harness test them. Not thread-safe: the store uses them on its
// thread.

#ifndef NET_COOKIES_MOLT_NONCE_COOKIE_POLICY_H_
#define NET_COOKIES_MOLT_NONCE_COOKIE_POLICY_H_

#include <cstddef>
#include <map>
#include <vector>

namespace net {

// What one nonce partition may hold.
inline constexpr size_t kMoltNoncePartitionMaxCookies = 300;
inline constexpr size_t kMoltNoncePartitionMaxCookieBytes = 32 * 1024;
// What a nonce's partitions (a page's credentialless frames') may hold
// together.
inline constexpr size_t kMoltNonceMaxCookies = 600;
inline constexpr size_t kMoltNonceMaxCookieBytes = 64 * 1024;
static_assert(kMoltNoncePartitionMaxCookies <= kMoltNonceMaxCookies);
static_assert(kMoltNoncePartitionMaxCookieBytes <= kMoltNonceMaxCookieBytes);
// One cookie's name and value never take more than a partition may hold
// (net::CanonicalCookie refuses a name and value past 4096 bytes together),
// so making room for a cookie never means evicting that cookie.
inline constexpr size_t kMoltCookieMaxNameValueBytes = 4096;
static_assert(kMoltCookieMaxNameValueBytes <=
              kMoltNoncePartitionMaxCookieBytes);

// Cookies and their bytes (name and value).
struct MoltNonceCookieCounts {
  size_t cookies = 0;
  size_t bytes = 0;

  friend bool operator==(const MoltNonceCookieCounts&,
                         const MoltNonceCookieCounts&) = default;
};

// Whether a cookie of `new_bytes` for a nonce partition holding `partition`
// is refused: its nonce is refused (`nonce_refused`), or the nonce's other
// partitions (`nonce` holds them and `partition`) leave it no room, however
// many of its own partition's cookies it would evict.
constexpr bool MoltNonceCookieRefused(bool nonce_refused,
                                      MoltNonceCookieCounts nonce,
                                      MoltNonceCookieCounts partition,
                                      size_t new_bytes) {
  if (nonce_refused) {
    return true;
  }
  const size_t other_cookies =
      nonce.cookies > partition.cookies ? nonce.cookies - partition.cookies
                                        : 0;
  const size_t other_bytes =
      nonce.bytes > partition.bytes ? nonce.bytes - partition.bytes : 0;
  return other_cookies + 1 > kMoltNonceMaxCookies ||
         other_bytes + new_bytes > kMoltNonceMaxCookieBytes;
}

// Whether a nonce partition (`partition`) or its nonce's partitions together
// (`nonce`) hold more than they may: the partition then evicts its own least
// recently used cookies until neither does.
constexpr bool MoltNonceCookiesOver(MoltNonceCookieCounts nonce,
                                    MoltNonceCookieCounts partition) {
  return partition.cookies > kMoltNoncePartitionMaxCookies ||
         partition.bytes > kMoltNoncePartitionMaxCookieBytes ||
         nonce.cookies > kMoltNonceMaxCookies ||
         nonce.bytes > kMoltNonceMaxCookieBytes;
}

// The cookies a store holds under each nonce, per partition, and the nonces
// it refuses. A record goes once its nonce holds nothing and isn't refused.
template <typename Nonce, typename PartitionKey>
class MoltNonceCookieTotals {
 public:
  MoltNonceCookieTotals();
  MoltNonceCookieTotals(const MoltNonceCookieTotals&) = delete;
  MoltNonceCookieTotals& operator=(const MoltNonceCookieTotals&) = delete;
  ~MoltNonceCookieTotals();

  // A cookie of `bytes` was stored in `partition`, of `nonce`.
  void Added(const Nonce& nonce, const PartitionKey& partition, size_t bytes) {
    Record& record = records_[nonce];
    record.counts.cookies += 1;
    record.counts.bytes += bytes;
    MoltNonceCookieCounts& in_partition = record.partitions[partition];
    in_partition.cookies += 1;
    in_partition.bytes += bytes;
  }

  // A cookie of `bytes` left `partition`, of `nonce`.
  void Removed(const Nonce& nonce,
               const PartitionKey& partition,
               size_t bytes) {
    auto it = records_.find(nonce);
    if (it == records_.end()) {
      return;
    }
    Record& record = it->second;
    Subtract(record.counts, bytes);
    if (auto in_partition = record.partitions.find(partition);
        in_partition != record.partitions.end()) {
      Subtract(in_partition->second, bytes);
      if (in_partition->second.cookies == 0) {
        record.partitions.erase(in_partition);
      }
    }
    MaybeForget(it);
  }

  // From now on `nonce` is refused (`refused`), or no longer is.
  void SetRefused(const Nonce& nonce, bool refused) {
    if (refused) {
      records_[nonce].refused = true;
      return;
    }
    auto it = records_.find(nonce);
    if (it == records_.end()) {
      return;
    }
    it->second.refused = false;
    MaybeForget(it);
  }

  // Whether a cookie of `new_bytes` for `partition`, of `nonce`, is refused
  // (MoltNonceCookieRefused()).
  bool Refuses(const Nonce& nonce,
               const PartitionKey& partition,
               size_t new_bytes) const {
    auto it = records_.find(nonce);
    if (it == records_.end()) {
      return MoltNonceCookieRefused(false, {}, {}, new_bytes);
    }
    return MoltNonceCookieRefused(it->second.refused, it->second.counts,
                                  PartitionCounts(it->second, partition),
                                  new_bytes);
  }

  // Whether `partition`, or its nonce's partitions together, hold more than
  // they may (MoltNonceCookiesOver()).
  bool Over(const Nonce& nonce, const PartitionKey& partition) const {
    auto it = records_.find(nonce);
    if (it == records_.end()) {
      return false;
    }
    return MoltNonceCookiesOver(it->second.counts,
                                PartitionCounts(it->second, partition));
  }

  bool IsRefused(const Nonce& nonce) const {
    auto it = records_.find(nonce);
    return it != records_.end() && it->second.refused;
  }

  MoltNonceCookieCounts Of(const Nonce& nonce) const {
    auto it = records_.find(nonce);
    return it == records_.end() ? MoltNonceCookieCounts() : it->second.counts;
  }

  MoltNonceCookieCounts OfPartition(const Nonce& nonce,
                                    const PartitionKey& partition) const {
    auto it = records_.find(nonce);
    return it == records_.end() ? MoltNonceCookieCounts()
                                : PartitionCounts(it->second, partition);
  }

  // The partitions of `nonce` that hold cookies.
  std::vector<PartitionKey> PartitionsOf(const Nonce& nonce) const {
    std::vector<PartitionKey> partitions;
    auto it = records_.find(nonce);
    if (it != records_.end()) {
      for (const auto& [partition, counts] : it->second.partitions) {
        partitions.push_back(partition);
      }
    }
    return partitions;
  }

  // How many nonces have a record (for tests).
  size_t size() const { return records_.size(); }

 private:
  struct Record {
    MoltNonceCookieCounts counts;
    std::map<PartitionKey, MoltNonceCookieCounts> partitions;
    bool refused = false;
  };
  using Records = std::map<Nonce, Record>;

  static void Subtract(MoltNonceCookieCounts& counts, size_t bytes) {
    counts.cookies = counts.cookies > 0 ? counts.cookies - 1 : 0;
    counts.bytes = counts.bytes > bytes ? counts.bytes - bytes : 0;
  }

  static MoltNonceCookieCounts PartitionCounts(const Record& record,
                                               const PartitionKey& partition) {
    auto it = record.partitions.find(partition);
    return it == record.partitions.end() ? MoltNonceCookieCounts()
                                         : it->second;
  }

  void MaybeForget(typename Records::iterator it) {
    if (it->second.counts.cookies == 0 && !it->second.refused) {
      records_.erase(it);
    }
  }

  Records records_;
};

// Out of the class body, as Chromium's style asks of a class with members
// that have constructors.
template <typename Nonce, typename PartitionKey>
MoltNonceCookieTotals<Nonce, PartitionKey>::MoltNonceCookieTotals() = default;
template <typename Nonce, typename PartitionKey>
MoltNonceCookieTotals<Nonce, PartitionKey>::~MoltNonceCookieTotals() = default;

}  // namespace net

#endif  // NET_COOKIES_MOLT_NONCE_COOKIE_POLICY_H_
