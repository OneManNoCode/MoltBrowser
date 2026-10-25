// Copyright 2017 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/download/internal/background_service/blob_task_proxy.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/task/single_thread_task_runner.h"
#include "base/uuid.h"
#include "storage/browser/blob/blob_data_builder.h"
#include "storage/browser/blob/blob_data_handle.h"
#include "storage/browser/blob/blob_storage_context.h"

namespace download {

// static
std::unique_ptr<BlobTaskProxy> BlobTaskProxy::Create(
    BlobContextGetter blob_context_getter,
    scoped_refptr<base::SingleThreadTaskRunner> io_task_runner) {
  return std::make_unique<BlobTaskProxy>(blob_context_getter, io_task_runner);
}

BlobTaskProxy::BlobTaskProxy(
    BlobContextGetter blob_context_getter,
    scoped_refptr<base::SingleThreadTaskRunner> io_task_runner)
    : main_task_runner_(base::SingleThreadTaskRunner::GetCurrentDefault()),
      io_task_runner_(io_task_runner) {
  // Unretained the raw pointer because owner on UI thread should destroy this
  // object on IO thread.
  io_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(&BlobTaskProxy::InitializeOnIO,
                                base::Unretained(this), blob_context_getter));
}

BlobTaskProxy::~BlobTaskProxy() {
  CHECK(io_task_runner_->BelongsToCurrentThread());
  // MoltBrowser: nothing stays held for a download that went away.
  MoltReleaseHeldMemoryOnIO();
}

void BlobTaskProxy::InitializeOnIO(BlobContextGetter blob_context_getter) {
  DCHECK(io_task_runner_->BelongsToCurrentThread());

  blob_storage_context_ = blob_context_getter.Run();
}

void BlobTaskProxy::SaveAsBlob(std::unique_ptr<std::string> data,
                               BlobDataHandleCallback callback,
                               uint64_t molt_budget_key) {
  // Unretained the raw pointer because owner on UI thread should destroy this
  // object on IO thread.
  io_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&BlobTaskProxy::SaveAsBlobOnIO, base::Unretained(this),
                     std::move(data), std::move(callback), molt_budget_key));
}

void BlobTaskProxy::SaveAsBlobOnIO(std::unique_ptr<std::string> data,
                                   BlobDataHandleCallback callback,
                                   uint64_t molt_budget_key) {
  DCHECK(io_task_runner_->BelongsToCurrentThread());

  // MoltBrowser: what was held for the download becomes the blob, counted
  // as the blob from here on.
  MoltReleaseHeldMemoryOnIO();

  // Build blob data. This has to do a copy into blob's internal storage.
  std::string blob_uuid = base::Uuid::GenerateRandomV4().AsLowercaseString();
  auto builder = std::make_unique<storage::BlobDataBuilder>(blob_uuid);
  // MoltBrowser: held under the key of the site it is for
  // (molt_blob_budget_policy.h).
  builder->MoltSetBudgetKey(molt_budget_key);
  builder->AppendData(*data);
  blob_data_handle_ =
      blob_storage_context_->AddFinishedBlob(std::move(builder));

  // Wait for blob data construction complete.
  auto cb = base::BindOnce(&BlobTaskProxy::BlobSavedOnIO,
                           weak_ptr_factory_.GetWeakPtr(), std::move(callback));
  blob_data_handle_->RunOnConstructionComplete(std::move(cb));
}

// MoltBrowser: see the header.
void BlobTaskProxy::MoltHoldMemory(uint64_t molt_budget_key,
                                   size_t bytes,
                                   MoltHoldCallback done) {
  // Unretained the raw pointer because owner on UI thread should destroy this
  // object on IO thread.
  io_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&BlobTaskProxy::MoltHoldMemoryOnIO,
                     base::Unretained(this), molt_budget_key, bytes,
                     std::move(done)));
}

void BlobTaskProxy::MoltReleaseHeldMemory() {
  io_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(&BlobTaskProxy::MoltReleaseHeldMemoryOnIO,
                                base::Unretained(this)));
}

void BlobTaskProxy::MoltHoldMemoryOnIO(uint64_t molt_budget_key,
                                       size_t bytes,
                                       MoltHoldCallback done) {
  DCHECK(io_task_runner_->BelongsToCurrentThread());
  storage::MoltBlobHold hold = storage::MoltBlobHold::kNotCounted;
  if (blob_storage_context_) {
    if (molt_held_bytes_ > 0 && molt_held_key_ != molt_budget_key) {
      MoltReleaseHeldMemoryOnIO();
    }
    hold = blob_storage_context_->MoltHoldMemory(molt_budget_key, bytes);
    if (hold == storage::MoltBlobHold::kCounted) {
      molt_held_key_ = molt_budget_key;
      molt_held_bytes_ += bytes;
    }
  }
  main_task_runner_->PostTask(FROM_HERE,
                              base::BindOnce(std::move(done), hold));
}

void BlobTaskProxy::MoltReleaseHeldMemoryOnIO() {
  DCHECK(io_task_runner_->BelongsToCurrentThread());
  if (blob_storage_context_ && molt_held_bytes_ > 0) {
    blob_storage_context_->MoltReleaseHeldMemory(molt_held_key_,
                                                 molt_held_bytes_);
  }
  molt_held_bytes_ = 0;
}

void BlobTaskProxy::BlobSavedOnIO(BlobDataHandleCallback callback,
                                  storage::BlobStatus status) {
  DCHECK(io_task_runner_->BelongsToCurrentThread());

  // Relay BlobDataHandle and |status| back to main thread.
  auto cb =
      base::BindOnce(std::move(callback), std::move(blob_data_handle_), status);
  main_task_runner_->PostTask(FROM_HERE, std::move(cb));
}

}  // namespace download
