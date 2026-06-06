// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// ModelDownloader — Downloads GGUF model files from HuggingFace Hub
// using Chromium's network::SimpleURLLoader for proper progress tracking,
// retry logic, and redirect handling.

#ifndef CHROME_BROWSER_MOLT_AI_MODELS_MODEL_DOWNLOADER_H_
#define CHROME_BROWSER_MOLT_AI_MODELS_MODEL_DOWNLOADER_H_

#include <cstdint>  // for int64_t — required on Linux under -fmodules
#include <string>
#include <functional>
#include <memory>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"

namespace network {
class SimpleURLLoader;
namespace mojom {
class URLLoaderFactory;
}
}  // namespace network

namespace molt_ai {

struct DownloadProgress;

// ModelDownloader handles downloading GGUF model files from HuggingFace Hub.
//
// HuggingFace download URL pattern:
//   https://huggingface.co/{repo}/resolve/main/{filename}
//
// Usage:
//   auto downloader = std::make_unique<ModelDownloader>();
//   downloader->SetURLLoaderFactory(url_loader_factory);
//   downloader->StartDownload(repo, filename, dest_path,
//       progress_callback, completion_callback);
class ModelDownloader {
 public:
  using ProgressCallback =
      base::RepeatingCallback<void(const DownloadProgress& progress)>;
  using CompletionCallback =
      base::OnceCallback<void(bool success, const std::string& error)>;

  ModelDownloader();
  ~ModelDownloader();

  ModelDownloader(const ModelDownloader&) = delete;
  ModelDownloader& operator=(const ModelDownloader&) = delete;

  // Set the URL loader factory from the browser profile.
  // Must be called before StartDownload.
  // Get from: content::BrowserContext::GetDefaultStoragePartition(profile)
  //               ->GetURLLoaderFactoryForBrowserProcess()
  void SetURLLoaderFactory(network::mojom::URLLoaderFactory* factory);

  // Start downloading a model from HuggingFace
  void StartDownload(
      const std::string& repo,
      const std::string& filename,
      const base::FilePath& destination,
      ProgressCallback progress_callback,
      CompletionCallback completion_callback);

  // Cancel the current download
  void CancelDownload();

  // Check if a download is in progress
  bool IsDownloading() const;

  // Build the HuggingFace download URL
  static std::string BuildDownloadURL(const std::string& repo,
                                       const std::string& filename);

  // Map model_id to HuggingFace repo and filename
  static bool GetModelDownloadInfo(const std::string& model_id,
                                    std::string* repo,
                                    std::string* filename);

 private:
  // Called by SimpleURLLoader during download with bytes received
  void OnDownloadProgress(ProgressCallback progress_callback,
                          uint64_t current);

  // Called when download completes (success or failure)
  void OnDownloadComplete(base::FilePath final_path,
                          base::FilePath partial_path,
                          CompletionCallback completion_callback,
                          base::FilePath downloaded_path);

  bool is_downloading_ = false;
  std::string current_model_id_;

  // Network loader for the current download
  std::unique_ptr<network::SimpleURLLoader> url_loader_;
  raw_ptr<network::mojom::URLLoaderFactory> url_loader_factory_ = nullptr;

  // Speed/ETA tracking
  base::TimeTicks download_start_time_;
  uint64_t download_last_bytes_ = 0;
  base::TimeTicks download_last_time_;

  base::WeakPtrFactory<ModelDownloader> weak_factory_{this};
};

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_MODELS_MODEL_DOWNLOADER_H_
