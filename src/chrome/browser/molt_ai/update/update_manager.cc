// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/update/update_manager.h"

#include <optional>
#include <string>
#include <utility>

#include "base/command_line.h"
#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/numerics/safe_conversions.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/process/process_handle.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/values.h"
#include "base/version.h"
#include "build/build_config.h"
#include "chrome/browser/molt_ai/common/molt_blocking_scope.h"
#include "chrome/browser/molt_ai/update/release_manifest.h"
#include "chrome/common/webui_url_constants.h"
#include "crypto/hash.h"
#include "net/base/load_flags.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "url/gurl.h"

namespace molt_ai {

namespace {

// The release feed we publish to. releases/latest returns the newest
// non-prerelease release with its tag, notes, and per-platform assets.
constexpr char kLatestReleaseApi[] =
    "https://api.github.com/repos/OneManNoCode/MoltBrowser/releases/latest";

// Fallback probe when the API fails (it is rate-limited to 60 req/hour per
// IP for unauthenticated callers): the releases/latest WEB url 302s to
// .../releases/tag/<tag>, so the latest tag can be read from the redirect
// target alone. Served by the site frontend — no meaningful rate limit.
constexpr char kLatestReleaseWeb[] =
    "https://github.com/OneManNoCode/MoltBrowser/releases/latest";
constexpr char kReleaseTagPathMarker[] = "/releases/tag/";
constexpr char kDownloadUrlBase[] =
    "https://github.com/OneManNoCode/MoltBrowser/releases/download/";

// When both the API and the fallback probe fail, retry sooner than the
// normal background cadence so a transient blip self-heals quickly.
constexpr base::TimeDelta kFailureRetryDelay = base::Minutes(10);

// Delay the first check so it never competes with startup work.
constexpr base::TimeDelta kInitialCheckDelay = base::Seconds(30);
// Background re-check cadence.
constexpr base::TimeDelta kCheckInterval = base::Hours(6);

// Cap the API response; releases/latest JSON is a few KB.
constexpr size_t kMaxApiResponseBytes = 1024 * 1024;

net::NetworkTrafficAnnotationTag UpdaterAnnotation() {
  return net::DefineNetworkTrafficAnnotation("molt_updater", R"(
      semantics {
        sender: "MoltBrowser In-App Updater"
        description:
          "Checks the MoltBrowser GitHub Releases API for a newer version "
          "and, if found, downloads its signed release manifest and the "
          "installer for this platform. The installer is only used if it "
          "matches the manifest, which must be signed by MoltBrowser's "
          "offline release key."
        trigger:
          "On launch (delayed), periodically in the background, and when the "
          "user clicks 'Check for updates'."
        data: "None. A simple GET to the public GitHub releases endpoint."
        destination: WEBSITE
      }
      policy {
        cookies_allowed: NO
        setting: "Toggle 'Automatic updates' off on the molt://update page."
        policy_exception_justification: "Not yet covered by enterprise policy."
      })");
}

// Strip a leading v/V from a release tag ("v0.2.1" -> "0.2.1").
std::string NormalizeTag(const std::string& tag) {
  if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) {
    return tag.substr(1);
  }
  return tag;
}

// macOS: the new app must carry a valid Apple-issued signature from
// GenEye AI Labs' Developer ID team.
[[maybe_unused]] constexpr char kMacCodeRequirement[] =
    "=anchor apple generic and certificate leaf[subject.OU] = \"83DA5U8N7Z\"";

// Hashes the whole file (blocking; runs on the ThreadPool). Returns nullopt
// if it can't be read.
std::optional<std::array<uint8_t, 32>> HashFileSha256(
    const base::FilePath& path) {
  base::File file(path, base::File::FLAG_OPEN | base::File::FLAG_READ);
  if (!file.IsValid()) {
    return std::nullopt;
  }
  std::array<uint8_t, 32> digest;
  if (!crypto::hash::HashFile(crypto::hash::kSha256, &file, digest)) {
    return std::nullopt;
  }
  return digest;
}

#if BUILDFLAG(IS_WIN)
// Quotes |s| as a PowerShell single-quoted string literal. PowerShell treats
// the typographic single quotes as quote characters too, so all of them are
// doubled.
std::wstring PowerShellQuote(const std::wstring& s) {
  std::wstring out = L"'";
  for (wchar_t c : s) {
    if (c == L'\'' || c == L'\u2018' || c == L'\u2019' || c == L'\u201A' ||
        c == L'\u201B') {
      out.push_back(c);
    }
    out.push_back(c);
  }
  out.push_back(L'\'');
  return out;
}
#endif

}  // namespace

// static
UpdateManager* UpdateManager::Get() {
  static base::NoDestructor<UpdateManager> instance;
  return instance.get();
}

UpdateManager::UpdateManager() = default;
UpdateManager::~UpdateManager() = default;

void UpdateManager::Initialize(
    scoped_refptr<network::SharedURLLoaderFactory> factory) {
  if (initialized_) {
    return;
  }
  initialized_ = true;
  factory_ = std::move(factory);
  current_version_ = chrome::kMoltBrowserVersion;

  LOG(INFO) << "[MoltUpdate] Initialized. Current version " << current_version_
            << ", auto-update " << (auto_update_ ? "on" : "off");

  // First check shortly after launch, then on a steady cadence.
  initial_check_timer_.Start(
      FROM_HERE, kInitialCheckDelay,
      base::BindOnce(&UpdateManager::CheckForUpdates, base::Unretained(this),
                     /*user_initiated=*/false));
  periodic_timer_.Start(FROM_HERE, kCheckInterval,
                        base::BindRepeating(&UpdateManager::OnPeriodicTimer,
                                            base::Unretained(this)));
}

void UpdateManager::OnPeriodicTimer() {
  CheckForUpdates(/*user_initiated=*/false);
}

void UpdateManager::CheckForUpdates(bool user_initiated) {
  // Don't interrupt an in-flight network operation.
  if (state_ == UpdateState::kChecking ||
      state_ == UpdateState::kDownloading ||
      state_ == UpdateState::kVerifying ||
      state_ == UpdateState::kInstalling) {
    return;
  }
  if (!factory_) {
    SetCheckFailed(
        "Couldn't check yet — still starting up. Try again in a moment.");
    return;
  }

  SetState(UpdateState::kChecking);
  error_.clear();

  auto request = std::make_unique<network::ResourceRequest>();
  request->url = GURL(kLatestReleaseApi);
  request->method = "GET";
  request->load_flags = net::LOAD_DO_NOT_SAVE_COOKIES;
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  // GitHub rejects requests without a User-Agent.
  request->headers.SetHeader(net::HttpRequestHeaders::kUserAgent,
                             base::StrCat({"MoltBrowser/", current_version_}));
  request->headers.SetHeader("Accept", "application/vnd.github+json");

  loader_ = network::SimpleURLLoader::Create(std::move(request),
                                             UpdaterAnnotation());
  loader_->SetRetryOptions(
      2, network::SimpleURLLoader::RETRY_ON_NETWORK_CHANGE);
  // Deliver non-2xx bodies too, so a 403 rate-limit is distinguishable from
  // a dead network (and gets logged as such) instead of both collapsing into
  // a null body.
  loader_->SetAllowHttpErrorResults(true);
  loader_->DownloadToString(
      factory_.get(),
      base::BindOnce(&UpdateManager::OnCheckResponse, base::Unretained(this),
                     user_initiated),
      kMaxApiResponseBytes);
}

void UpdateManager::OnCheckResponse(bool user_initiated,
                                    std::optional<std::string> body) {
  // Capture the HTTP status for diagnosis before releasing the loader —
  // e.g. 403 here almost always means the unauthenticated GitHub API
  // rate limit (60 req/hour per IP), not a broken network.
  int response_code = 0;
  if (loader_ && loader_->ResponseInfo() && loader_->ResponseInfo()->headers) {
    response_code = loader_->ResponseInfo()->headers->response_code();
  }
  loader_.reset();

  if (!body || body->empty()) {
    LOG(WARNING) << "[MoltUpdate] API check got no body (HTTP "
                 << response_code << ") — trying redirect fallback";
    StartFallbackCheck(user_initiated);
    return;
  }

  std::optional<base::Value> parsed =
      base::JSONReader::Read(*body, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    LOG(WARNING) << "[MoltUpdate] API check unparseable (HTTP "
                 << response_code << ") — trying redirect fallback";
    StartFallbackCheck(user_initiated);
    return;
  }
  const base::DictValue& root = parsed->GetDict();

  const std::string* tag = root.FindString("tag_name");
  if (!tag || tag->empty()) {
    // Valid JSON but no tag — typically GitHub's rate-limit/error payload
    // ({"message": "API rate limit exceeded..."}).
    const std::string* msg = root.FindString("message");
    LOG(WARNING) << "[MoltUpdate] API check returned no tag (HTTP "
                 << response_code << ", message: " << (msg ? *msg : "none")
                 << ") — trying redirect fallback";
    StartFallbackCheck(user_initiated);
    return;
  }
  latest_tag_ = *tag;
  latest_version_ = NormalizeTag(*tag);
  if (const std::string* notes = root.FindString("body")) {
    release_notes_ = *notes;
  }
  if (const std::string* html = root.FindString("html_url")) {
    release_url_ = *html;
  }

  // Find this platform's installer plus the signed manifest + signature.
  // Sizes and hashes reported here are NOT trusted; the signed manifest is.
  std::string installer_url, manifest_url, signature_url;
  asset_name_.clear();
  asset_size_ = 0;
  const std::string matcher = PlatformAssetMatcher();
  if (const base::ListValue* assets = root.FindList("assets")) {
    for (const base::Value& a : *assets) {
      if (!a.is_dict()) {
        continue;
      }
      const base::DictValue& ad = a.GetDict();
      const std::string* name = ad.FindString("name");
      const std::string* dl = ad.FindString("browser_download_url");
      if (!name || !dl) {
        continue;
      }
      if (*name == kReleaseManifestAssetName) {
        manifest_url = *dl;
      } else if (*name == kReleaseSignatureAssetName) {
        signature_url = *dl;
      } else if (installer_url.empty() && !matcher.empty() &&
                 name->find(matcher) != std::string::npos) {
        installer_url = *dl;
        asset_name_ = *name;
        asset_size_ = base::saturated_cast<int64_t>(
            ad.FindDouble("size").value_or(0));
      }
    }
  }
  SetReleaseAssetUrls(installer_url, manifest_url, signature_url);

  // Compare versions.
  base::Version current(current_version_);
  base::Version latest(latest_version_);
  const bool newer = current.IsValid() && latest.IsValid() && current < latest;

  if (!newer) {
    LOG(INFO) << "[MoltUpdate] Up to date (" << current_version_
              << " >= " << latest_version_ << ")";
    SetState(UpdateState::kUpToDate);
    return;
  }

  LOG(INFO) << "[MoltUpdate] Update available: " << latest_version_
            << " (asset: " << (asset_name_.empty() ? "none" : asset_name_)
            << ")";
  SetState(UpdateState::kAvailable);

  // Fully-automatic mode: start the background download immediately. If we
  // couldn't resolve an installer asset for this platform, leave it at
  // kAvailable so the UI can offer the release page instead.
  if (auto_update_ && !asset_url_.empty()) {
    DownloadUpdate();
  }
}

void UpdateManager::StartFallbackCheck(bool user_initiated) {
  if (!factory_) {
    ResolveCheckFailure();
    return;
  }

  auto request = std::make_unique<network::ResourceRequest>();
  request->url = GURL(kLatestReleaseWeb);
  request->method = "GET";
  request->load_flags = net::LOAD_DO_NOT_SAVE_COOKIES;
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->headers.SetHeader(net::HttpRequestHeaders::kUserAgent,
                             base::StrCat({"MoltBrowser/", current_version_}));

  loader_ = network::SimpleURLLoader::Create(std::move(request),
                                             UpdaterAnnotation());
  loader_->SetRetryOptions(
      1, network::SimpleURLLoader::RETRY_ON_NETWORK_CHANGE);
  // Headers only: the redirect chain is followed and the final URL carries
  // the tag; the release page body itself is never downloaded.
  loader_->DownloadHeadersOnly(
      factory_.get(),
      base::BindOnce(&UpdateManager::OnFallbackHeaders,
                     base::Unretained(this), user_initiated));
}

void UpdateManager::OnFallbackHeaders(
    bool user_initiated,
    scoped_refptr<net::HttpResponseHeaders> headers) {
  std::string tag;
  if (loader_) {
    const std::string final_url = loader_->GetFinalURL().spec();
    const size_t marker = final_url.find(kReleaseTagPathMarker);
    if (marker != std::string::npos) {
      tag = final_url.substr(marker + sizeof(kReleaseTagPathMarker) - 1);
      // Strip any trailing path/query (defensive; not expected).
      const size_t cut = tag.find_first_of("/?#");
      if (cut != std::string::npos) {
        tag = tag.substr(0, cut);
      }
    }
  }
  loader_.reset();

  if (!headers || tag.empty()) {
    ResolveCheckFailure();
    return;
  }

  latest_tag_ = tag;
  latest_version_ = NormalizeTag(tag);
  release_url_ = base::StrCat(
      {"https://github.com/OneManNoCode/MoltBrowser/releases/tag/", tag});

  base::Version current(current_version_);
  base::Version latest(latest_version_);
  const bool newer = current.IsValid() && latest.IsValid() && current < latest;

  if (!newer) {
    LOG(INFO) << "[MoltUpdate] Up to date via redirect fallback ("
              << current_version_ << " >= " << latest_version_ << ")";
    SetState(UpdateState::kUpToDate);
    return;
  }

  // Newer release found. The API (which carries notes + asset metadata)
  // was unreachable, but our release pipeline uploads assets under fixed,
  // versionless names — so the download URL is constructible directly.
  const std::string asset_name = PlatformAssetName();
  const std::string release_base = base::StrCat({kDownloadUrlBase, tag, "/"});
  asset_name_ = asset_name;
  asset_size_ = 0;  // Unknown until the signed manifest is fetched.
  SetReleaseAssetUrls(
      asset_name.empty() ? std::string()
                         : base::StrCat({release_base, asset_name}),
      base::StrCat({release_base, kReleaseManifestAssetName}),
      base::StrCat({release_base, kReleaseSignatureAssetName}));
  release_notes_.clear();  // Notes need the API; the release page link stands in.

  LOG(INFO) << "[MoltUpdate] Update available via redirect fallback: "
            << latest_version_
            << " (asset: " << (asset_name_.empty() ? "none" : asset_name_)
            << ")";
  SetState(UpdateState::kAvailable);

  if (auto_update_ && !asset_url_.empty()) {
    DownloadUpdate();
  }
}

void UpdateManager::ResolveCheckFailure() {
  // Retry sooner than the 6-hour cadence so a blip self-heals.
  initial_check_timer_.Start(
      FROM_HERE, kFailureRetryDelay,
      base::BindOnce(&UpdateManager::CheckForUpdates, base::Unretained(this),
                     /*user_initiated=*/false));

  // A previous successful check already answered the only question that
  // matters ("am I up to date?"); keep that answer instead of alarming the
  // user over a transient network problem.
  if (!latest_version_.empty()) {
    base::Version current(current_version_);
    base::Version latest(latest_version_);
    const bool newer =
        current.IsValid() && latest.IsValid() && current < latest;
    LOG(WARNING) << "[MoltUpdate] Check failed; keeping last known result ("
                 << (newer ? "update available" : "up to date") << ")";
    SetState(newer ? UpdateState::kAvailable : UpdateState::kUpToDate);
    return;
  }

  SetCheckFailed(base::StrCat(
      {"Couldn't check for updates — you're on ", current_version_,
       ". We'll retry automatically."}));
}

void UpdateManager::SetReleaseAssetUrls(const std::string& installer_url,
                                        const std::string& manifest_url,
                                        const std::string& signature_url) {
  // Downloads only ever come from this release's official asset path, so a
  // tampered API response can't point the updater at another host.
  const std::string release_base =
      base::StrCat({kDownloadUrlBase, latest_tag_, "/"});
  auto official = [&release_base](const std::string& url) {
    return base::StartsWith(url, release_base, base::CompareCase::SENSITIVE)
               ? url
               : std::string();
  };
  asset_url_ = official(installer_url);
  manifest_url_ = official(manifest_url);
  signature_url_ = official(signature_url);
  if (asset_url_.empty()) {
    asset_name_.clear();
  }
}

bool UpdateManager::can_install_automatically() const {
  return HasUsableUpdateSigningKey(TrustedUpdateSigningKeys());
}

void UpdateManager::ResetVerification() {
  manifest_body_.clear();
  expected_size_ = 0;
  expected_sha256_ = {};
  verified_ = false;
  downloaded_path_.clear();
}

void UpdateManager::RejectUpdate(const std::string& reason) {
  LOG(ERROR) << "[MoltUpdate] Update rejected: " << reason;
  ResetVerification();
  SetError(base::StrCat(
      {"Update not installed: ", reason,
       ". Only download MoltBrowser from the official releases page."}));
}

void UpdateManager::DownloadUpdate() {
  if (asset_url_.empty() || asset_name_.empty()) {
    SetError("No installer available for this platform");
    return;
  }
  if (state_ == UpdateState::kDownloading ||
      state_ == UpdateState::kVerifying ||
      state_ == UpdateState::kInstalling) {
    return;
  }
  if (!factory_) {
    SetError("Network not ready");
    return;
  }
  if (!can_install_automatically()) {
    // Fail closed: without a compiled-in key nothing can be verified, so the
    // UI offers the release page instead of an in-app install.
    error_ =
        "This build can't verify updates, so it won't install them "
        "automatically. Download the new version from the release page.";
    SetState(UpdateState::kAvailable);
    return;
  }
  if (manifest_url_.empty() || signature_url_.empty()) {
    error_ =
        "This release isn't signed, so it can't be installed automatically. "
        "Download it from the release page.";
    SetState(UpdateState::kAvailable);
    return;
  }

  ResetVerification();
  error_.clear();
  download_percent_ = 0;
  SetState(UpdateState::kDownloading);
  FetchToString(manifest_url_, kMaxReleaseManifestBytes,
                base::BindOnce(&UpdateManager::OnManifestFetched,
                               base::Unretained(this)));
}

void UpdateManager::FetchToString(
    const std::string& url,
    size_t max_bytes,
    base::OnceCallback<void(std::optional<std::string>)> done) {
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = GURL(url);
  request->method = "GET";
  request->load_flags = net::LOAD_DO_NOT_SAVE_COOKIES;
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->redirect_mode = network::mojom::RedirectMode::kFollow;
  request->headers.SetHeader(net::HttpRequestHeaders::kUserAgent,
                             base::StrCat({"MoltBrowser/", current_version_}));

  loader_ = network::SimpleURLLoader::Create(std::move(request),
                                             UpdaterAnnotation());
  loader_->SetRetryOptions(
      2, network::SimpleURLLoader::RETRY_ON_NETWORK_CHANGE);
  loader_->DownloadToString(factory_.get(), std::move(done), max_bytes);
}

void UpdateManager::OnManifestFetched(std::optional<std::string> body) {
  loader_.reset();
  if (!body || body->empty()) {
    SetError("Couldn't download the release manifest");
    return;
  }
  manifest_body_ = std::move(*body);
  FetchToString(signature_url_, kMaxReleaseSignatureBytes,
                base::BindOnce(&UpdateManager::OnSignatureFetched,
                               base::Unretained(this)));
}

void UpdateManager::OnSignatureFetched(std::optional<std::string> body) {
  loader_.reset();
  if (!body || body->empty()) {
    SetError("Couldn't download the release signature");
    return;
  }
  std::string error;
  if (!VerifyReleaseManifestSignature(manifest_body_, *body,
                                      TrustedUpdateSigningKeys(), &error)) {
    RejectUpdate(error);
    return;
  }
  std::optional<ReleaseManifest> manifest =
      ParseReleaseManifest(manifest_body_, &error);
  if (!manifest || !CheckReleaseManifestPolicy(*manifest, latest_tag_,
                                               current_version_, &error)) {
    RejectUpdate(error);
    return;
  }
  const ReleaseManifest::Asset* asset = manifest->FindAsset(asset_name_);
  if (!asset) {
    RejectUpdate(base::StrCat(
        {"the signed manifest doesn't list ", asset_name_}));
    return;
  }
  expected_size_ = asset->size;
  expected_sha256_ = asset->sha256;
  asset_size_ = asset->size;  // Trusted now; drives the progress bar.
  LOG(INFO) << "[MoltUpdate] Signed manifest verified for "
            << manifest->version;
  StartInstallerDownload();
}

void UpdateManager::StartInstallerDownload() {
  base::FilePath dir;
  {
    ScopedAllowBlockingForMolt allow_blocking;
    if (!download_dir_.empty()) {
      base::DeletePathRecursively(download_dir_);
      download_dir_.clear();
    }
    // A fresh, private directory per download: mkdtemp (0700) on POSIX, the
    // per-user temp dir on Windows. Nobody else can swap the installer
    // between verification and install.
    if (!base::CreateNewTempDirectory(FILE_PATH_LITERAL("MoltBrowserUpdate"),
                                      &dir)) {
      SetError("Couldn't create a private folder for the download");
      return;
    }
  }
  download_dir_ = dir;
  // asset_name_ matched a name in the signed manifest, which only allows
  // plain file names.
  const base::FilePath dest = dir.AppendASCII(asset_name_);

  auto request = std::make_unique<network::ResourceRequest>();
  request->url = GURL(asset_url_);
  request->method = "GET";
  request->load_flags = net::LOAD_DO_NOT_SAVE_COOKIES;
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->redirect_mode = network::mojom::RedirectMode::kFollow;
  request->headers.SetHeader(net::HttpRequestHeaders::kUserAgent,
                             base::StrCat({"MoltBrowser/", current_version_}));

  loader_ = network::SimpleURLLoader::Create(std::move(request),
                                             UpdaterAnnotation());
  loader_->SetRetryOptions(
      2, network::SimpleURLLoader::RETRY_ON_NETWORK_CHANGE);
  loader_->SetOnDownloadProgressCallback(base::BindRepeating(
      &UpdateManager::OnDownloadProgress, base::Unretained(this)));
  // Never accept more bytes than the signed manifest promises.
  loader_->DownloadToFile(
      factory_.get(),
      base::BindOnce(&UpdateManager::OnDownloadComplete, base::Unretained(this),
                     dest),
      dest, expected_size_);
  LOG(INFO) << "[MoltUpdate] Downloading " << asset_name_ << " -> "
            << dest.value();
}

void UpdateManager::OnDownloadProgress(uint64_t current) {
  if (asset_size_ > 0) {
    int pct = static_cast<int>((current * 100) / asset_size_);
    download_percent_ = pct > 100 ? 100 : pct;
  } else {
    download_percent_ = -1;
  }
  NotifyChanged();
}

void UpdateManager::OnDownloadComplete(base::FilePath expected_path,
                                       base::FilePath path) {
  loader_.reset();
  if (path.empty()) {
    SetError("Download failed");
    return;
  }
  download_percent_ = 100;
  SetState(UpdateState::kVerifying);
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(&HashFileSha256, path),
      base::BindOnce(&UpdateManager::OnInstallerHashed, base::Unretained(this),
                     path));
}

void UpdateManager::OnInstallerHashed(
    base::FilePath path,
    std::optional<std::array<uint8_t, 32>> sha256) {
  if (state_ != UpdateState::kVerifying) {
    return;  // Superseded.
  }
  int64_t size = -1;
  {
    ScopedAllowBlockingForMolt allow_blocking;
    size = base::GetFileSize(path).value_or(-1);
  }
  if (!sha256 || size != expected_size_ || *sha256 != expected_sha256_) {
    {
      ScopedAllowBlockingForMolt allow_blocking;
      base::DeleteFile(path);
    }
    RejectUpdate("the downloaded installer doesn't match the signed manifest");
    return;
  }
  downloaded_path_ = path;
  verified_ = true;
  LOG(INFO) << "[MoltUpdate] Installer verified: " << path.value();
  SetState(UpdateState::kDownloaded);
}

bool UpdateManager::InstallUpdate() {
  if (state_ != UpdateState::kDownloaded || downloaded_path_.empty() ||
      !verified_) {
    LOG(WARNING) << "[MoltUpdate] InstallUpdate with no verified installer";
    return false;
  }
  if (!LaunchInstaller(downloaded_path_,
                       base::HexEncodeLower(expected_sha256_))) {
    SetError("Could not launch the installer");
    return false;
  }
  SetState(UpdateState::kInstalling);
  LOG(INFO) << "[MoltUpdate] Installer launched; caller should exit to apply.";
  // The detached installer waits for this process to exit, replaces the app,
  // and relaunches it. The CALLER (browser-layer) performs the exit.
  return true;
}

void UpdateManager::SetAutoUpdate(bool enabled) {
  auto_update_ = enabled;
  LOG(INFO) << "[MoltUpdate] Auto-update " << (enabled ? "enabled" : "disabled");
  // If turning auto-update on while an update is already waiting, kick the
  // download off now.
  if (enabled && state_ == UpdateState::kAvailable && !asset_url_.empty()) {
    DownloadUpdate();
  } else {
    NotifyChanged();
  }
}

std::string UpdateManager::PlatformAssetMatcher() const {
#if BUILDFLAG(IS_MAC)
  return ".dmg";
#elif BUILDFLAG(IS_WIN)
  return "Setup.exe";
#elif BUILDFLAG(IS_LINUX)
  ScopedAllowBlockingForMolt allow_blocking;
  if (base::PathExists(base::FilePath("/usr/bin/dpkg")) ||
      base::PathExists(base::FilePath("/var/lib/dpkg"))) {
    return ".deb";
  }
  if (base::PathExists(base::FilePath("/usr/bin/rpm"))) {
    return ".rpm";
  }
  return ".tar.gz";
#else
  return "";
#endif
}

// ---- Platform install ----
//
// Each platform launches a small DETACHED helper that waits for this browser
// process to exit, re-checks the installer's SHA-256 against the signed
// manifest (so nothing can swap the file after verification), installs it,
// and relaunches the app. Values are passed as separate arguments (never
// spliced into shell text), so paths can't inject commands. Detaching is
// essential: the helper must outlive the browser it is replacing. These
// paths require on-device validation per platform.
bool UpdateManager::LaunchInstaller(const base::FilePath& installer_path,
                                    const std::string& expected_sha256_hex) {
  const base::ProcessId pid = base::GetCurrentProcId();

#if BUILDFLAG(IS_MAC)
  // The browser executable is at MoltBrowser.app/Contents/MacOS/<exe>; the
  // .app bundle is three directories up.
  base::FilePath exe;
  base::PathService::Get(base::FILE_EXE, &exe);
  const std::string app = exe.DirName().DirName().DirName().value();  // *.app

  // $1 pid, $2 dmg, $3 installed .app, $4 expected sha256, $5 code
  // requirement. The new app must pass codesign with GenEye's Team ID both on
  // the mounted image and after copying; the old app is only removed once
  // the new one is in place.
  static constexpr char kScript[] = R"SH(
pid="$1"; dmg="$2"; app="$3"; want="$4"; req="$5"
while /bin/kill -0 "$pid" 2>/dev/null; do /bin/sleep 1; done
relaunch() { /usr/bin/open "$app"; exit "$1"; }
got=$(/usr/bin/shasum -a 256 "$dmg" | /usr/bin/awk '{print $1}')
[ "$got" = "$want" ] || relaunch 1
mnt=$(/usr/bin/mktemp -d) || relaunch 1
/usr/bin/hdiutil attach "$dmg" -readonly -nobrowse -noautoopen -mountpoint "$mnt" || relaunch 1
src=$(/bin/ls -d "$mnt"/*.app 2>/dev/null | /usr/bin/head -1)
if [ -z "$src" ] || ! /usr/bin/codesign --verify --deep --strict -R "$req" "$src"; then
  /usr/bin/hdiutil detach "$mnt" -quiet; relaunch 1
fi
/bin/rm -rf "$app.new" "$app.old"
if ! /usr/bin/ditto "$src" "$app.new"; then
  /bin/rm -rf "$app.new"; /usr/bin/hdiutil detach "$mnt" -quiet; relaunch 1
fi
/usr/bin/hdiutil detach "$mnt" -quiet
if ! /usr/bin/codesign --verify --deep --strict -R "$req" "$app.new"; then
  /bin/rm -rf "$app.new"; relaunch 1
fi
if /bin/mv "$app" "$app.old" && /bin/mv "$app.new" "$app"; then
  /bin/rm -rf "$app.old"
elif [ -d "$app.old" ] && [ ! -d "$app" ]; then
  /bin/mv "$app.old" "$app"
fi
/usr/bin/open "$app"
)SH";

  base::CommandLine cmd(base::FilePath("/bin/bash"));
  cmd.AppendArg("-c");
  cmd.AppendArg(kScript);
  cmd.AppendArg("molt-update");  // $0
  cmd.AppendArg(base::NumberToString(pid));
  cmd.AppendArg(installer_path.value());
  cmd.AppendArg(app);
  cmd.AppendArg(expected_sha256_hex);
  cmd.AppendArg(kMacCodeRequirement);
  base::LaunchOptions options;
  options.new_process_group = true;
  return base::LaunchProcess(cmd, options).IsValid();

#elif BUILDFLAG(IS_WIN)
  base::FilePath exe;
  base::PathService::Get(base::FILE_EXE, &exe);
  // PowerShell helper: wait for us to exit, re-check the installer hash, run
  // the NSIS installer silently, then relaunch. Every value is a quoted
  // literal, so a path can't break out of the script.
  const std::wstring installer = PowerShellQuote(installer_path.value());
  std::wstring ps = base::StrCat({
      L"try { Wait-Process -Id ", base::UTF8ToWide(base::NumberToString(pid)),
      L" -ErrorAction SilentlyContinue } catch {}; ",
      L"$h = ''; try { $h = (Get-FileHash -Algorithm SHA256 -LiteralPath ",
      installer, L").Hash.ToLowerInvariant() } catch {}; ",
      L"if ($h -eq ", PowerShellQuote(base::UTF8ToWide(expected_sha256_hex)),
      L") { Start-Process -FilePath ", installer,
      L" -ArgumentList '/S' -Wait }; ",
      L"Start-Process -FilePath ", PowerShellQuote(exe.value()),
  });
  // Launch PowerShell by its full path. A bare "powershell.exe" is searched
  // for in the browser's own directory and the current directory before
  // System32, so a planted copy there would run the update.
  base::FilePath system_dir;
  if (!base::PathService::Get(base::DIR_SYSTEM, &system_dir)) {
    return false;
  }
  const base::FilePath powershell =
      system_dir.Append(FILE_PATH_LITERAL("WindowsPowerShell"))
          .Append(FILE_PATH_LITERAL("v1.0"))
          .Append(FILE_PATH_LITERAL("powershell.exe"));
  base::CommandLine cmd(powershell);
  cmd.AppendArg("-NoProfile");
  cmd.AppendArg("-WindowStyle");
  cmd.AppendArg("Hidden");
  cmd.AppendArg("-Command");
  cmd.AppendArgNative(ps);
  base::LaunchOptions options;
  options.start_hidden = true;
  return base::LaunchProcess(cmd, options).IsValid();

#elif BUILDFLAG(IS_LINUX)
  base::FilePath exe;
  base::PathService::Get(base::FILE_EXE, &exe);
  // $1 pid, $2 package, $3 browser exe, $4 expected sha256. No --force:
  // package managers keep their downgrade protection.
  // PATH is reset first, so pkexec, dpkg and the rest always come from the
  // system directories, never from a user-writable PATH entry such as
  // ~/.local/bin.
  static constexpr char kScript[] = R"SH(
PATH=/usr/sbin:/usr/bin:/sbin:/bin; export PATH
pid="$1"; f="$2"; exe="$3"; want="$4"
while kill -0 "$pid" 2>/dev/null; do sleep 1; done
got=$(sha256sum "$f" | cut -d' ' -f1)
if [ "$got" = "$want" ]; then
  case "$f" in
    *.deb) pkexec dpkg -i "$f" ;;
    *.rpm) pkexec rpm -U "$f" ;;
    *) xdg-open "$(dirname "$f")" ;;
  esac
fi
"$exe" &
)SH";
  base::CommandLine cmd(base::FilePath("/bin/sh"));
  cmd.AppendArg("-c");
  cmd.AppendArg(kScript);
  cmd.AppendArg("molt-update");  // $0
  cmd.AppendArg(base::NumberToString(pid));
  cmd.AppendArg(installer_path.value());
  cmd.AppendArg(exe.value());
  cmd.AppendArg(expected_sha256_hex);
  base::LaunchOptions options;
  options.new_process_group = true;
  return base::LaunchProcess(cmd, options).IsValid();
#else
  return false;
#endif
}

// ---- Observers / state ----
void UpdateManager::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void UpdateManager::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

void UpdateManager::SetState(UpdateState state) {
  state_ = state;
  NotifyChanged();
}

void UpdateManager::SetError(const std::string& message) {
  error_ = message;
  LOG(WARNING) << "[MoltUpdate] " << message;
  state_ = UpdateState::kError;
  NotifyChanged();
}

void UpdateManager::SetCheckFailed(const std::string& message) {
  error_ = message;
  LOG(WARNING) << "[MoltUpdate] " << message;
  state_ = UpdateState::kCheckFailed;
  NotifyChanged();
}

std::string UpdateManager::PlatformAssetName() const {
#if BUILDFLAG(IS_MAC)
  return "MoltBrowser-macOS-arm64.dmg";
#elif BUILDFLAG(IS_LINUX)
  ScopedAllowBlockingForMolt allow_blocking;
  if (base::PathExists(base::FilePath("/usr/bin/dpkg")) ||
      base::PathExists(base::FilePath("/var/lib/dpkg"))) {
    return "MoltBrowser-Linux-x64.deb";
  }
  if (base::PathExists(base::FilePath("/usr/bin/rpm"))) {
    return "MoltBrowser-Linux-x64.rpm";
  }
  return "MoltBrowser-Linux-x64.tar.gz";
#else
  // Windows installer names aren't stable across releases; the API path
  // (which lists real asset names) is required there.
  return std::string();
#endif
}

void UpdateManager::NotifyChanged() {
  for (Observer& observer : observers_) {
    observer.OnUpdateChanged();
  }
}

}  // namespace molt_ai
