// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// EmbeddingModelManager — whether the on-device embedding model behind
// "Search by meaning" (docs/design/SEMANTIC_SEARCH.md) is offered,
// downloaded or being downloaded, and its download and removal. One per
// browser (the model file is shared by every profile, like the chat
// models), on the UI thread.
//
// Fail closed: the model is offered only when its registry entry is pinned
// (revision + SHA-256 + exact size; embedding_model_registry.h), on-device
// AI can run on this machine, and it has the memory for it. The download
// comes from the pinned revision and is rejected unless its size and
// SHA-256 match (models/model_download_job.h); a file of another size on
// disk counts as not installed and is never loaded.

#ifndef CHROME_BROWSER_MOLT_AI_MEMORY_EMBEDDING_MODEL_MANAGER_H_
#define CHROME_BROWSER_MOLT_AI_MEMORY_EMBEDDING_MODEL_MANAGER_H_

#include <cstdint>
#include <memory>
#include <string>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/sequence_checker.h"
#include "chrome/browser/molt_ai/memory/embedding_model_registry.h"

namespace network {
class SharedURLLoaderFactory;
}  // namespace network

namespace molt_ai {

namespace models {
class ModelDownloadJob;
}  // namespace models

namespace memory {

class EmbeddingModelManager {
 public:
  enum class State {
    // Not looked at disk yet.
    kUnknown,
    // Not available in this build or on this machine (see |reason|).
    kNotOffered,
    kNotInstalled,
    kDownloading,
    kInstalled,
    kRemoving,
  };

  struct Status {
    State state = State::kUnknown;
    // Why the model isn't offered, user-facing; "" otherwise.
    std::string reason;
    // The last download or removal error, user-facing; "" if none.
    std::string error;
    int64_t received_bytes = 0;
    int64_t total_bytes = 0;
  };

  class Observer : public base::CheckedObserver {
   public:
    // The state changed (not called for download progress).
    virtual void OnEmbeddingModelChanged() = 0;
  };

  static EmbeddingModelManager* GetInstance();

  EmbeddingModelManager();
  ~EmbeddingModelManager();
  EmbeddingModelManager(const EmbeddingModelManager&) = delete;
  EmbeddingModelManager& operator=(const EmbeddingModelManager&) = delete;

  const EmbeddingModelSpec& spec() const { return spec_; }
  // ~/.moltbrowser/models/<file>, with the chat models.
  base::FilePath ModelPath() const;
  const Status& status() const { return status_; }
  bool IsKnown() const { return status_.state != State::kUnknown; }
  bool IsInstalled() const { return status_.state == State::kInstalled; }

  // Looks at disk (on a worker) and updates the state. Called once by the
  // first MemoryService; later calls are cheap.
  void Refresh();

  // Downloads the pinned file through |factory| (a regular profile's, never
  // a MoltNet one). |done| gets "" or a user-facing error.
  void StartDownload(scoped_refptr<network::SharedURLLoaderFactory> factory,
                     base::OnceCallback<void(std::string error)> done);
  // Stops the download and deletes its partial file.
  void CancelDownload();
  // Ends the model's process, then deletes the file.
  void Remove(base::OnceCallback<void(bool ok)> done);

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  void SetSpecForTesting(const EmbeddingModelSpec& spec);
  void SetModelDirForTesting(const base::FilePath& dir);

 private:
  struct DiskState {
    base::FilePath dir;
    bool installed = false;
  };

  // Whether on-device AI can run the model here; "" when it can.
  std::string NotOfferedReason() const;
  void OnRefreshed(uint64_t refresh, DiskState disk);
  void OnDownloadPrecheck(scoped_refptr<network::SharedURLLoaderFactory>
                              factory,
                          base::OnceCallback<void(std::string)> done,
                          std::string error);
  void OnDownloadProgress(int64_t received, int64_t total);
  void SetState(State state);

  SEQUENCE_CHECKER(sequence_checker_);

  EmbeddingModelSpec spec_;
  base::FilePath model_dir_;
  Status status_;
  // Bumped by every Refresh(); older replies are ignored.
  uint64_t refresh_ = 0;
  std::unique_ptr<models::ModelDownloadJob> download_;
  base::ObserverList<Observer> observers_;

  base::WeakPtrFactory<EmbeddingModelManager> weak_factory_{this};
};

}  // namespace memory
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_MEMORY_EMBEDDING_MODEL_MANAGER_H_
