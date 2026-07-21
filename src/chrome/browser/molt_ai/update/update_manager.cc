// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/update/update_manager.h"

#include <optional>
#include <string>
#include <utility>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/process/process_handle.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "base/values.h"
#include "base/version.h"
#include "build/build_config.h"
#include "chrome/browser/molt_ai/common/molt_blocking_scope.h"
#include "chrome/common/webui_url_constants.h"
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
          "and, if found, downloads the official signed installer for this "
          "platform so the browser can update itself."
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

  // Find this platform's installer asset.
  asset_url_.clear();
  asset_name_.clear();
  asset_size_ = 0;
  const std::string matcher = PlatformAssetMatcher();
  if (!matcher.empty()) {
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
        if (name->find(matcher) != std::string::npos) {
          asset_url_ = *dl;
          asset_name_ = *name;
          asset_size_ =
              static_cast<int64_t>(ad.FindDouble("size").value_or(0));
          break;
        }
      }
    }
  }

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
  if (!asset_name.empty()) {
    asset_name_ = asset_name;
    asset_url_ = base::StrCat({kDownloadUrlBase, tag, "/", asset_name});
    asset_size_ = 0;  // Unknown without the API; progress shows percent-less.
  } else {
    asset_url_.clear();
    asset_name_.clear();
    asset_size_ = 0;
  }
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

void UpdateManager::DownloadUpdate() {
  if (asset_url_.empty()) {
    SetError("No installer available for this platform");
    return;
  }
  if (state_ == UpdateState::kDownloading ||
      state_ == UpdateState::kInstalling) {
    return;
  }
  if (!factory_) {
    SetError("Network not ready");
    return;
  }

  base::FilePath dir = DownloadDir();
  base::FilePath dest = dir.AppendASCII(
      asset_name_.empty() ? std::string("MoltBrowserUpdate") : asset_name_);
  {
    ScopedAllowBlockingForMolt allow_blocking;
    base::CreateDirectory(dir);
    // Remove any stale previous download.
    base::DeleteFile(dest);
  }

  download_percent_ = 0;
  SetState(UpdateState::kDownloading);

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
  loader_->DownloadToFile(
      factory_.get(),
      base::BindOnce(&UpdateManager::OnDownloadComplete, base::Unretained(this),
                     dest),
      dest);
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
  downloaded_path_ = path;
  download_percent_ = 100;
  LOG(INFO) << "[MoltUpdate] Download complete: " << path.value();
  SetState(UpdateState::kDownloaded);
}

bool UpdateManager::InstallUpdate() {
  if (state_ != UpdateState::kDownloaded || downloaded_path_.empty()) {
    LOG(WARNING) << "[MoltUpdate] InstallUpdate with no downloaded installer";
    return false;
  }
  if (!LaunchInstaller(downloaded_path_)) {
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

base::FilePath UpdateManager::DownloadDir() const {
  base::FilePath dir;
  base::PathService::Get(base::DIR_TEMP, &dir);
  return dir.AppendASCII("MoltBrowserUpdate");
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
// All three launch a small DETACHED helper that: waits for this browser
// process to exit, runs the official signed installer, and relaunches the
// app. Detaching is essential — the helper must outlive the browser it is
// replacing. These paths require on-device validation per platform.
bool UpdateManager::LaunchInstaller(const base::FilePath& installer_path) {
  const base::ProcessId pid = base::GetCurrentProcId();

#if BUILDFLAG(IS_MAC)
  // The browser executable is at MoltBrowser.app/Contents/MacOS/<exe>; the
  // .app bundle is three directories up.
  base::FilePath exe;
  base::PathService::Get(base::FILE_EXE, &exe);
  const std::string app = exe.DirName().DirName().DirName().value();  // *.app

  std::string script = base::StrCat({
      "while /bin/kill -0 ", base::NumberToString(pid),
      " 2>/dev/null; do sleep 1; done; ",
      "MNT=$(/usr/bin/mktemp -d); ",
      "/usr/bin/hdiutil attach '", installer_path.value(),
      "' -nobrowse -noautoopen -mountpoint \"$MNT\" || exit 1; ",
      "SRC=$(/bin/ls -d \"$MNT\"/*.app | /usr/bin/head -1); ",
      "/usr/bin/ditto \"$SRC\" '", app, ".new' && ",
      "/bin/rm -rf '", app, "' && /bin/mv '", app, ".new' '", app, "'; ",
      "/usr/bin/hdiutil detach \"$MNT\" -quiet; ",
      "/usr/bin/xattr -dr com.apple.quarantine '", app, "' 2>/dev/null; ",
      "/usr/bin/open '", app, "'",
  });

  base::CommandLine cmd(base::FilePath("/bin/bash"));
  cmd.AppendArg("-c");
  cmd.AppendArg(script);
  base::LaunchOptions options;
  options.new_process_group = true;
  return base::LaunchProcess(cmd, options).IsValid();

#elif BUILDFLAG(IS_WIN)
  base::FilePath exe;
  base::PathService::Get(base::FILE_EXE, &exe);
  // PowerShell helper: wait for us to exit, run the NSIS installer silently,
  // then relaunch. -Wait makes the silent install complete before relaunch.
  std::wstring ps = base::StrCat({
      L"try { Wait-Process -Id ", base::UTF8ToWide(base::NumberToString(pid)),
      L" -ErrorAction SilentlyContinue } catch {}; ",
      L"Start-Process -FilePath '", installer_path.value(),
      L"' -ArgumentList '/S' -Wait; ",
      L"Start-Process -FilePath '", exe.value(), L"'",
  });
  base::CommandLine cmd(base::FilePath(FILE_PATH_LITERAL("powershell.exe")));
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
  const std::string f = installer_path.value();
  std::string install_cmd;
  if (base::EndsWith(f, ".deb", base::CompareCase::SENSITIVE)) {
    install_cmd = base::StrCat({"pkexec dpkg -i '", f, "'"});
  } else if (base::EndsWith(f, ".rpm", base::CompareCase::SENSITIVE)) {
    install_cmd = base::StrCat({"pkexec rpm -U --force '", f, "'"});
  } else {
    // Tarball / unknown: reveal it. A self-extracting swap needs the install
    // prefix, which we don't reliably know here.
    install_cmd = base::StrCat({"xdg-open \"$(dirname '", f, "')\""});
  }
  std::string script = base::StrCat({
      "while kill -0 ", base::NumberToString(pid),
      " 2>/dev/null; do sleep 1; done; ", install_cmd, "; '", exe.value(),
      "' &",
  });
  base::CommandLine cmd(base::FilePath("/bin/sh"));
  cmd.AppendArg("-c");
  cmd.AppendArg(script);
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
