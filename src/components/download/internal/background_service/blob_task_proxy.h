// Copyright 2017 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_DOWNLOAD_INTERNAL_BACKGROUND_SERVICE_BLOB_TASK_PROXY_H_
#define COMPONENTS_DOWNLOAD_INTERNAL_BACKGROUND_SERVICE_BLOB_TASK_PROXY_H_

#include <memory>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/task/single_thread_task_runner.h"
#include "storage/browser/blob/blob_storage_constants.h"
#include "storage/browser/blob/molt_blob_budget_policy.h"  // MoltBrowser

namespace storage {
class BlobDataHandle;
class BlobStorageContext;
}  // namespace storage

namespace download {

// Proxy for blob related task on IO thread.
// Created on main thread and do work on IO thread, destroyed on IO thread.
class BlobTaskProxy {
 public:
  using BlobContextGetter =
      base::RepeatingCallback<base::WeakPtr<storage::BlobStorageContext>()>;
  using BlobDataHandleCallback =
      base::OnceCallback<void(std::unique_ptr<storage::BlobDataHandle>,
                              storage::BlobStatus status)>;

  static std::unique_ptr<BlobTaskProxy> Create(
      BlobContextGetter blob_context_getter,
      scoped_refptr<base::SingleThreadTaskRunner> io_task_runner);

  BlobTaskProxy(BlobContextGetter blob_context_getter,
                scoped_refptr<base::SingleThreadTaskRunner> io_task_runner);

  BlobTaskProxy(const BlobTaskProxy&) = delete;
  BlobTaskProxy& operator=(const BlobTaskProxy&) = delete;

  ~BlobTaskProxy();

  // Save blob data on UI thread. |callback| will be called on main thread after
  // blob construction completes.
  void SaveAsBlob(std::unique_ptr<std::string> data,
                  BlobDataHandleCallback callback,
                  // MoltBrowser: the budget key the blob counts to
                  // (molt_blob_budget_policy.h).
                  uint64_t molt_budget_key = storage::kMoltBlobBrowserKey);

  // MoltBrowser: on the main thread. Counts |bytes| more of the data that
  // will become the blob against |molt_budget_key|'s share while it is
  // still being downloaded (storage::BlobStorageContext::MoltHoldMemory());
  // |done| gets the result on the main thread. What this proxy holds is
  // given up when SaveAsBlob() makes the blob from it, by
  // MoltReleaseHeldMemory(), or with the proxy.
  using MoltHoldCallback = base::OnceCallback<void(storage::MoltBlobHold)>;
  void MoltHoldMemory(uint64_t molt_budget_key,
                      size_t bytes,
                      MoltHoldCallback done);
  void MoltReleaseHeldMemory();

 private:
  // MoltBrowser:
  void MoltHoldMemoryOnIO(uint64_t molt_budget_key,
                          size_t bytes,
                          MoltHoldCallback done);
  void MoltReleaseHeldMemoryOnIO();

  void InitializeOnIO(BlobContextGetter blob_context_getter);

  void SaveAsBlobOnIO(std::unique_ptr<std::string> data,
                      BlobDataHandleCallback callback,
                      uint64_t molt_budget_key);  // MoltBrowser

  void BlobSavedOnIO(BlobDataHandleCallback callback,
                     storage::BlobStatus status);

  scoped_refptr<base::SingleThreadTaskRunner> main_task_runner_;

  // Used to build blob data, must accessed on |io_task_runner_|.
  base::WeakPtr<storage::BlobStorageContext> blob_storage_context_;

  // Used to access blob storage context.
  scoped_refptr<base::SingleThreadTaskRunner> io_task_runner_;

  // BlobDataHandle that will be eventually passed to main thread.
  std::unique_ptr<storage::BlobDataHandle> blob_data_handle_;

  // MoltBrowser: what MoltHoldMemory() holds, and for which key; on
  // |io_task_runner_|.
  uint64_t molt_held_key_ = storage::kMoltBlobBrowserKey;
  size_t molt_held_bytes_ = 0;

  // Bounded to IO thread task runner.
  base::WeakPtrFactory<BlobTaskProxy> weak_ptr_factory_{this};
};

}  // namespace download

#endif  // COMPONENTS_DOWNLOAD_INTERNAL_BACKGROUND_SERVICE_BLOB_TASK_PROXY_H_
