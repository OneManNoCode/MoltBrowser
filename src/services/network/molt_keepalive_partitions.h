// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: MoltNet's keepalive request limits, per site and its own.
//
// The network service limits requests that may outlive their page (fetch()
// with keepalive, navigator.sendBeacon(), pings): a new one fails with
// ERR_INSUFFICIENT_RESOURCES once 2048 are in flight, once 256 are in
// flight for its top-level frame, or once that frame's in-flight ones hold
// 512 KiB of URL and headers (URLLoaderFactory::CreateLoaderAndStart()).
// The 2048 are counted by one KeepaliveStatisticsRecorder for the whole
// network service: every profile and every site together. So one site
// could use them up and another, in MoltNet or in a regular window, could
// tell from its own requests failing, and a regular page could see
// MoltNet's.
//
// A network context with NetworkContextParams::molt_per_site_keepalive_limits
// (MoltNet's) is marked in the network service's recorder; each of its
// URLLoaderFactories then counts its keepalive requests in a recorder of
// its own site (molt_keepalive_site.h), with Chromium's three limits:
// 2048 for the site, 256 and 512 KiB for each of its top-level frames. Its
// requests count toward no other recorder, and no other context's or
// site's toward its. A context's recorders go with it.
//
// Std-only (a template over the recorder type), so molt_ai_unittests and a
// standalone harness test it.

#ifndef SERVICES_NETWORK_MOLT_KEEPALIVE_PARTITIONS_H_
#define SERVICES_NETWORK_MOLT_KEEPALIVE_PARTITIONS_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>

namespace network {

template <typename Recorder>
class MoltKeepalivePartitions {
 public:
  // Marks `context` (a network context, by address) as one whose keepalive
  // requests are counted per site.
  void AddContext(uintptr_t context) { contexts_.insert(context); }

  // Unmarks it and drops its sites' recorders.
  void RemoveContext(uintptr_t context) {
    contexts_.erase(context);
    std::erase_if(recorders_, [context](const auto& item) {
      return item.first.first == context;
    });
  }

  bool IsPartitioned(uintptr_t context) const {
    return contexts_.contains(context);
  }

  // The recorder for `context`'s requests of site `site` ("" for none):
  // `shared` unless `context` is marked, else the site's own, made on first
  // use.
  Recorder* RecorderFor(uintptr_t context,
                        const std::string& site,
                        Recorder* shared) {
    if (!IsPartitioned(context)) {
      return shared;
    }
    std::unique_ptr<Recorder>& recorder = recorders_[{context, site}];
    if (!recorder) {
      recorder = std::make_unique<Recorder>();
    }
    return recorder.get();
  }

  size_t RecorderCount() const { return recorders_.size(); }

 private:
  std::set<uintptr_t> contexts_;
  std::map<std::pair<uintptr_t, std::string>, std::unique_ptr<Recorder>>
      recorders_;
};

}  // namespace network

#endif  // SERVICES_NETWORK_MOLT_KEEPALIVE_PARTITIONS_H_
