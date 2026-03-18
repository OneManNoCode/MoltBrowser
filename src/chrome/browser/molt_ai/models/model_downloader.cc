// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/models/model_downloader.h"

#include <unordered_map>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "chrome/browser/molt_ai/models/model_manager.h"
#include "net/base/load_flags.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"

namespace molt_ai {

// Model download registry: maps model_id -> {repo, filename}
struct ModelDownloadEntry {
  std::string repo;
  std::string filename;
};

static const std::unordered_map<std::string, ModelDownloadEntry>&
GetDownloadRegistry() {
  static const auto* registry =
      new std::unordered_map<std::string, ModelDownloadEntry>{
          {"tinyllama-1.1b",
           {"TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF",
            "tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf"}},
          {"phi-3.5-mini",
           {"bartowski/Phi-3.5-mini-instruct-GGUF",
            "Phi-3.5-mini-instruct-Q4_K_M.gguf"}},
          {"mistral-7b",
           {"MistralAI/Mistral-7B-Instruct-v0.3-GGUF",
            "Mistral-7B-Instruct-v0.3-Q4_K_M.gguf"}},
          {"llama3.1-8b",
           {"bartowski/Meta-Llama-3.1-8B-Instruct-GGUF",
            "Meta-Llama-3.1-8B-Instruct-Q4_K_M.gguf"}},
          {"qwen2.5-7b",
           {"Qwen/Qwen2.5-7B-Instruct-GGUF",
            "qwen2.5-7b-instruct-q4_k_m.gguf"}},
          {"gemma2-9b",
           {"bartowski/gemma-2-9b-it-GGUF",
            "gemma-2-9b-it-Q4_K_M.gguf"}},
      };
  return *registry;
}

// Network traffic annotation — required by Chromium for all network requests.
constexpr net::NetworkTrafficAnnotationTag kModelDownloadAnnotation =
    net::DefineNetworkTrafficAnnotation("molt_ai_model_download", R"(
    semantics {
      sender: "MoltBrowser AI Model Downloader"
      description:
        "Downloads GGUF model files from HuggingFace Hub for on-device "
        "AI inference. Models are stored locally and all inference runs "
        "on-device. No user data is sent to the server."
      trigger:
        "User explicitly requests a model download from the AI settings "
        "or first-run experience."
      data: "HTTP GET request with no user-identifying information."
      destination: WEBSITE
    }
    policy {
      cookies_allowed: NO
      setting:
        "Users control which models to download through the settings UI."
    })");

ModelDownloader::ModelDownloader() = default;
ModelDownloader::~ModelDownloader() = default;

std::string ModelDownloader::BuildDownloadURL(const std::string& repo,
                                               const std::string& filename) {
  return "https://huggingface.co/" + repo + "/resolve/main/" + filename;
}

bool ModelDownloader::GetModelDownloadInfo(const std::string& model_id,
                                            std::string* repo,
                                            std::string* filename) {
  const auto& registry = GetDownloadRegistry();
  auto it = registry.find(model_id);
  if (it == registry.end()) {
    return false;
  }
  *repo = it->second.repo;
  *filename = it->second.filename;
  return true;
}

void ModelDownloader::StartDownload(
    const std::string& repo,
    const std::string& filename,
    const base::FilePath& destination,
    ProgressCallback progress_callback,
    CompletionCallback completion_callback) {
  if (is_downloading_) {
    std::move(completion_callback)
        .Run(false, "Another download is already in progress");
    return;
  }

  is_downloading_ = true;
  current_model_id_ = filename;

  std::string url = BuildDownloadURL(repo, filename);
  LOG(INFO) << "[MoltAI] Starting model download: " << url;

  // Report initial progress
  DownloadProgress initial_progress;
  initial_progress.model_id = current_model_id_;
  initial_progress.bytes_downloaded = 0;
  initial_progress.total_bytes = 0;
  initial_progress.percent_complete = 0.0f;
  initial_progress.speed_bytes_per_sec = 0.0f;
  initial_progress.estimated_seconds_remaining = -1;
  initial_progress.is_complete = false;
  initial_progress.has_error = false;
  progress_callback.Run(initial_progress);

  // Create the network request
  auto resource_request = std::make_unique<network::ResourceRequest>();
  resource_request->url = GURL(url);
  resource_request->method = "GET";
  resource_request->load_flags =
      net::LOAD_DO_NOT_SAVE_COOKIES | net::LOAD_DO_NOT_SEND_COOKIES;
  // HuggingFace CDN uses 302 redirects — follow them
  resource_request->redirect_mode = network::mojom::RedirectMode::kFollow;

  // Create SimpleURLLoader — no size limit for model files (multi-GB)
  url_loader_ = network::SimpleURLLoader::Create(
      std::move(resource_request), kModelDownloadAnnotation);
  url_loader_->SetRetryOptions(
      2, network::SimpleURLLoader::RETRY_ON_NETWORK_CHANGE);
  // Allow unlimited download size (models can be 1-10 GB)
  url_loader_->SetAllowHttpErrorResults(false);

  // Track download speed/ETA
  download_start_time_ = base::TimeTicks::Now();
  download_last_bytes_ = 0;
  download_last_time_ = download_start_time_;

  // Register progress callback for real-time speed/ETA updates
  url_loader_->SetOnDownloadProgressCallback(base::BindRepeating(
      &ModelDownloader::OnDownloadProgress, weak_factory_.GetWeakPtr(),
      progress_callback));

  // Download to a .partial file first to avoid incomplete model files
  base::FilePath partial_path =
      destination.AddExtension(FILE_PATH_LITERAL(".partial"));

  // Ensure parent directory exists
  base::FilePath parent_dir = destination.DirName();
  base::CreateDirectory(parent_dir);

  // The caller must set the URLLoaderFactory before calling StartDownload.
  // Typically from the browser profile:
  //   content::BrowserContext::GetDefaultStoragePartition(profile)
  //       ->GetURLLoaderFactoryForBrowserProcess()
  if (!url_loader_factory_) {
    LOG(ERROR) << "[MoltAI] URL loader factory not set";
    is_downloading_ = false;
    std::move(completion_callback)
        .Run(false, "Network not initialized. Please restart the browser.");
    return;
  }

  // Begin downloading to file
  url_loader_->DownloadToFile(
      url_loader_factory_,
      base::BindOnce(&ModelDownloader::OnDownloadComplete,
                     weak_factory_.GetWeakPtr(), destination, partial_path,
                     std::move(completion_callback)),
      partial_path);

  LOG(INFO) << "[MoltAI] Download in progress — partial: "
            << partial_path.value();
}

void ModelDownloader::OnDownloadProgress(
    ProgressCallback progress_callback,
    uint64_t current) {
  base::TimeTicks now = base::TimeTicks::Now();
  double elapsed_sec = (now - download_last_time_).InSecondsF();

  // Throttle progress updates to avoid flooding the UI (max 2/sec)
  if (elapsed_sec < 0.5)
    return;

  uint64_t bytes_delta = current - download_last_bytes_;
  double speed_bps = static_cast<double>(bytes_delta) / elapsed_sec;
  download_last_bytes_ = current;
  download_last_time_ = now;

  // Get total file size from Content-Length header
  int64_t total = -1;
  if (url_loader_ && url_loader_->ResponseInfo() &&
      url_loader_->ResponseInfo()->headers) {
    total = url_loader_->ResponseInfo()->headers->GetContentLength();
  }

  int eta_sec = -1;
  if (speed_bps > 0 && total > 0 && static_cast<int64_t>(current) < total) {
    eta_sec = static_cast<int>(
        static_cast<double>(total - current) / speed_bps);
  }

  DownloadProgress progress;
  progress.model_id = current_model_id_;
  progress.bytes_downloaded = current;
  progress.total_bytes = total > 0 ? static_cast<size_t>(total) : 0;
  progress.percent_complete =
      total > 0
          ? static_cast<float>(current) / static_cast<float>(total) * 100.0f
          : 0.0f;
  progress.speed_bytes_per_sec = static_cast<float>(speed_bps);
  progress.estimated_seconds_remaining = eta_sec;
  progress.is_complete = false;
  progress.has_error = false;

  progress_callback.Run(progress);
}

void ModelDownloader::OnDownloadComplete(
    base::FilePath final_path,
    base::FilePath partial_path,
    CompletionCallback completion_callback,
    base::FilePath downloaded_path) {
  is_downloading_ = false;

  int net_error = url_loader_ ? url_loader_->NetError() : net::OK;
  url_loader_.reset();

  if (downloaded_path.empty() || net_error != net::OK) {
    // Download failed — clean up partial file
    base::DeleteFile(partial_path);
    LOG(ERROR) << "[MoltAI] Model download failed, net_error=" << net_error;
    std::move(completion_callback)
        .Run(false, "Download failed (error " +
                        base::NumberToString(net_error) +
                        "). Check your network connection.");
    return;
  }

  // Rename .partial to final destination
  base::File::Error rename_error;
  if (!base::ReplaceFile(partial_path, final_path, &rename_error)) {
    base::DeleteFile(partial_path);
    LOG(ERROR) << "[MoltAI] Failed to rename downloaded file: "
               << rename_error;
    std::move(completion_callback)
        .Run(false, "Failed to save model file.");
    return;
  }

  // Verify file exists and has reasonable size (>1 MB)
  auto file_size = base::GetFileSize(final_path);
  if (!file_size.has_value() || file_size.value() < 1024 * 1024) {
    base::DeleteFile(final_path);
    LOG(ERROR) << "[MoltAI] Downloaded file too small or missing";
    std::move(completion_callback)
        .Run(false, "Downloaded file appears corrupted. Please try again.");
    return;
  }

  LOG(INFO) << "[MoltAI] Model download complete: " << final_path.value()
            << " (" << file_size.value() / (1024 * 1024) << " MB)";
  std::move(completion_callback).Run(true, "");
}

void ModelDownloader::CancelDownload() {
  if (url_loader_) {
    url_loader_.reset();
  }
  is_downloading_ = false;
  LOG(INFO) << "[MoltAI] Download cancelled";
}

bool ModelDownloader::IsDownloading() const {
  return is_downloading_;
}

void ModelDownloader::SetURLLoaderFactory(
    network::mojom::URLLoaderFactory* factory) {
  url_loader_factory_ = factory;
}

}  // namespace molt_ai
