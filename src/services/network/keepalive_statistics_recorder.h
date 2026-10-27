// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_NETWORK_KEEPALIVE_STATISTICS_RECORDER_H_
#define SERVICES_NETWORK_KEEPALIVE_STATISTICS_RECORDER_H_

#include <map>
#include <string>

#include "base/component_export.h"
#include "base/functional/callback.h"  // MoltBrowser
#include "base/memory/weak_ptr.h"
#include "base/unguessable_token.h"
#include "services/network/molt_keepalive_partitions.h"  // MoltBrowser

namespace network {

// KeepaliveStatisticsRecorder keeps tracks of the number of inflight requests
// with "keepalive" set.
class COMPONENT_EXPORT(NETWORK_SERVICE) KeepaliveStatisticsRecorder final {
 public:
  struct PerTopLevelFrameStats {
    int num_registrations = 1;
    int num_inflight_requests = 0;
    int peak_inflight_requests = 0;
    int total_request_size = 0;
  };

  KeepaliveStatisticsRecorder();

  KeepaliveStatisticsRecorder(const KeepaliveStatisticsRecorder&) = delete;
  KeepaliveStatisticsRecorder& operator=(const KeepaliveStatisticsRecorder&) =
      delete;

  ~KeepaliveStatisticsRecorder();

  // Registers / Unregisters |top_level_frame| to this object.
  // There can be multiple Register / Unregister calls with the same
  // |top_level_frame|, and this object thinks a an entry for |top_level_frame|
  // is gone when the number of Register calls with |top_level_frame| equals to
  // the number of Unregister calls with |token|.
  void Register(const base::UnguessableToken& top_level_frame_id);
  void Unregister(const base::UnguessableToken& top_level_frame_id);

  // Called when a request with keepalive set starts.
  void OnLoadStarted(const base::UnguessableToken& top_level_frame_id,
                     int request_size);
  // Called when a request with keepalive set finishes.
  void OnLoadFinished(const base::UnguessableToken& top_level_frame_id,
                      int request_size);

  const std::map<base::UnguessableToken, PerTopLevelFrameStats>&
  per_top_level_frame_records() const {
    return per_top_level_frame_records_;
  }
  int NumInflightRequestsPerTopLevelFrame(
      const base::UnguessableToken& top_level_frame_id) const;
  int GetTotalRequestSizePerTopLevelFrame(
      const base::UnguessableToken& top_level_frame_id) const;
  int num_inflight_requests() const { return num_inflight_requests_; }
  int peak_inflight_requests() const { return peak_inflight_requests_; }

  base::WeakPtr<KeepaliveStatisticsRecorder> AsWeakPtr() {
    return weak_ptr_factory_.GetWeakPtr();
  }

  // MoltBrowser: keepalive limits per site for MoltNet's network contexts
  // (services/network/molt_keepalive_partitions.h). Marks `context` (the
  // network service's recorder only), whose URLLoaderFactories then count
  // their requests in a recorder of their site's instead of this one.
  void MoltAddPartitionedContext(const void* context);
  // Unmarks it; its sites' recorders, and what they count, go.
  void MoltRemovePartitionedContext(const void* context);
  // The recorder for requests of `context`'s factories of site `site`: this
  // one, or the site's own in a marked context.
  KeepaliveStatisticsRecorder* MoltRecorderFor(const void* context,
                                               const std::string& site);
  // MoltBrowser: whether nothing uses this recorder (no top-level frame
  // registered, nothing in flight): a site's can then go
  // (molt_keepalive_partitions.h).
  bool MoltIsIdle() const {
    return per_top_level_frame_records_.empty() && num_inflight_requests_ == 0;
  }

 private:
  // MoltBrowser: a site's recorder tells the network service's (`root`) when
  // it becomes idle, in a task of its own (it may be inside a call of its
  // own), and is dropped there if it still is.
  void MoltNotifyIfIdle();
  static void MoltForgetIfIdle(base::WeakPtr<KeepaliveStatisticsRecorder> root,
                               uintptr_t context,
                               std::string site);

  std::map<base::UnguessableToken, PerTopLevelFrameStats>
      per_top_level_frame_records_;
  int num_inflight_requests_ = 0;
  int peak_inflight_requests_ = 0;
  // MoltBrowser: see MoltAddPartitionedContext().
  MoltKeepalivePartitions<KeepaliveStatisticsRecorder> molt_partitions_;
  // MoltBrowser: for a site's recorder, what MoltNotifyIfIdle() runs.
  base::RepeatingClosure molt_on_idle_;
  base::WeakPtrFactory<KeepaliveStatisticsRecorder> weak_ptr_factory_{this};
};

}  // namespace network

#endif  // SERVICES_NETWORK_KEEPALIVE_STATISTICS_RECORDER_H_
