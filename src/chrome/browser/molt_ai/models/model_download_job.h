// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// ModelDownloadJob — downloads one GGUF into "<final>.partial", hashing it
// (SHA-256) while it streams, and only renames it to the final path once
// its size and hash check out (S13).
//
// Why not SimpleURLLoader::DownloadToFile: it opens the target with
// FLAG_CREATE_ALWAYS, so "resuming" with a Range header truncated the
// .partial file and kept only the tail — a corrupt model that was then
// renamed into place. Here the body is streamed (DownloadAsStream) and
// appended on a blocking-IO sequence: a 206 that continues exactly at the
// end of the .partial file is appended, a 200 (Range ignored) restarts from
// byte 0, anything else fails. Bytes already on disk are re-hashed before a
// resume so the final hash covers the whole file. Only one chunk is in
// flight at a time (the loader is resumed after each write), so memory use
// stays bounded.
//
// UI-thread object. Destroying it cancels the request; the .partial file
// is kept so the next attempt resumes (HandleCancelDownload deletes it).

#ifndef CHROME_BROWSER_MOLT_AI_MODELS_MODEL_DOWNLOAD_JOB_H_
#define CHROME_BROWSER_MOLT_AI_MODELS_MODEL_DOWNLOAD_JOB_H_

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "services/network/public/cpp/simple_url_loader_stream_consumer.h"
#include "url/gurl.h"

namespace network {
class SharedURLLoaderFactory;
class SimpleURLLoader;
namespace mojom {
class URLResponseHead;
}  // namespace mojom
}  // namespace network

namespace molt_ai {
namespace models {

class ModelDownloadJob : public network::SimpleURLLoaderStreamConsumer {
 public:
  struct Params {
    GURL url;
    base::FilePath final_path;
    // Lowercase hex. Empty = entry not pinned: allowed, reported as
    // unverified.
    std::string expected_sha256;
    // Exact byte size; <= 0 = unknown.
    int64_t expected_size = 0;
  };

  struct Result {
    bool success = false;
    // True when the file matched a pinned SHA-256.
    bool verified = false;
    // SHA-256 of what was downloaded (lowercase hex), for logs.
    std::string sha256;
    int net_error = 0;
    // User-facing reason when !success.
    std::string error;
  };

  // (bytes on disk so far, expected total or 0)
  using ProgressCallback =
      base::RepeatingCallback<void(int64_t received, int64_t total)>;
  using DoneCallback = base::OnceCallback<void(Result)>;

  // Starts immediately. |done| runs exactly once unless the job is
  // destroyed first.
  static std::unique_ptr<ModelDownloadJob> Start(
      Params params,
      scoped_refptr<network::SharedURLLoaderFactory> factory,
      ProgressCallback progress,
      DoneCallback done);

  ModelDownloadJob(const ModelDownloadJob&) = delete;
  ModelDownloadJob& operator=(const ModelDownloadJob&) = delete;
  ~ModelDownloadJob() override;

  static base::FilePath PartialPathFor(const base::FilePath& final_path);

  // Stops the transfer and deletes the .partial file (user pressed Cancel,
  // as opposed to destroying the job, which keeps it for a later resume).
  // |done| is not run. The job can be destroyed right after.
  void CancelAndDiscard();

 private:
  class Writer;

  ModelDownloadJob(Params params,
                   scoped_refptr<network::SharedURLLoaderFactory> factory,
                   ProgressCallback progress,
                   DoneCallback done);

  void OnPartialOpened(int64_t existing_bytes);
  void OnResponseStarted(const GURL& final_url,
                         const network::mojom::URLResponseHead& head);
  void OnChunkWritten(base::OnceClosure resume, bool ok);
  void OnFinished(Result result);
  void Fail(std::string error, int net_error);

  // network::SimpleURLLoaderStreamConsumer:
  void OnDataReceived(std::string_view data,
                      base::OnceClosure resume) override;
  void OnComplete(bool success) override;
  void OnRetry(base::OnceClosure start_retry) override;

  Params params_;
  scoped_refptr<network::SharedURLLoaderFactory> factory_;
  ProgressCallback progress_;
  DoneCallback done_;

  scoped_refptr<base::SequencedTaskRunner> file_runner_;
  // Lives on |file_runner_|; deleted there.
  std::unique_ptr<Writer> writer_;
  std::unique_ptr<network::SimpleURLLoader> loader_;

  int64_t requested_offset_ = 0;
  int64_t written_ = 0;   // bytes on disk (UI-side mirror for progress)
  bool response_ok_ = false;

  base::WeakPtrFactory<ModelDownloadJob> weak_factory_{this};
};

}  // namespace models
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_MODELS_MODEL_DOWNLOAD_JOB_H_
