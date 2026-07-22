// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/tor_manager.h"

#include "build/build_config.h"

#if BUILDFLAG(IS_WIN)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif  // BUILDFLAG(IS_WIN)

#include <mutex>
#include <utility>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "chrome/browser/molt_ai/tor/tor_service.h"

namespace molt_ai {
namespace tor {

namespace {

// Common locations where tor lands across the popular installers.
// We probe in order and pick the first that's executable.
const char* const kCandidatePaths[] = {
    "/opt/homebrew/bin/tor",      // Apple Silicon Homebrew
    "/usr/local/bin/tor",          // Intel Homebrew, also Linux /usr/local
    "/usr/bin/tor",                // Debian/Ubuntu/Fedora package
    "/opt/local/bin/tor",          // MacPorts
};

// Where we keep our managed Tor data + torrc. Per user, under the
// MoltBrowser app-support directory. We pick a private dir so we
// don't clash with a system Tor's data dir (which holds the cookie
// file controlling auth — using a separate dir means each instance
// has its own).
base::FilePath ResolveAppSupportRoot() {
  base::FilePath home;
  if (!base::PathService::Get(base::DIR_HOME, &home))
    return base::FilePath();
#if BUILDFLAG(IS_MAC)
  return home.AppendASCII("Library")
      .AppendASCII("Application Support")
      .AppendASCII("MoltBrowser")
      .AppendASCII("tor");
#else
  return home.AppendASCII(".moltbrowser").AppendASCII("tor");
#endif
}

#if BUILDFLAG(IS_WIN)
// Initialize Winsock exactly once for the process (see tor_service.cc
// for the rationale on skipping WSACleanup()).
void EnsureWinsockStarted() {
  static std::once_flag once;
  std::call_once(once, [] {
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
  });
}
#endif  // BUILDFLAG(IS_WIN)

// Attempt a single control-port TCP connect to 127.0.0.1:9051 with a
// short timeout. Returns true if Tor accepted the connection. The
// socket layer differs per platform (winsock vs POSIX) but the logic
// is identical: open, set a 300ms timeout, connect, close.
bool TryConnectControlPort() {
  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(9051);
  addr.sin_addr.s_addr = htonl(0x7F000001);  // 127.0.0.1
#if BUILDFLAG(IS_WIN)
  EnsureWinsockStarted();
  SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == INVALID_SOCKET)
    return false;
  // SO_RCVTIMEO/SO_SNDTIMEO take a DWORD of milliseconds on winsock.
  DWORD ms = 300;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
             reinterpret_cast<const char*>(&ms), sizeof(ms));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
             reinterpret_cast<const char*>(&ms), sizeof(ms));
  int rc = connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                   sizeof(addr));
  closesocket(fd);
  return rc == 0;
#else
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return false;
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 300000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  int rc = connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                   sizeof(addr));
  close(fd);
  return rc == 0;
#endif  // BUILDFLAG(IS_WIN)
}

// Body run on a thread-pool task to wait for Tor to be reachable.
// Polls the control port until it answers (each attempt has a ~300ms
// connect timeout) or |total_ms| elapses. Cross-platform.
// Forward decl (defined after the control-port helpers below).
bool TorCircuitEstablishedBlocking(const std::string& cookie_path);

// Wait until Tor can actually route (has built a usable circuit), not merely
// until its control port answers. Two phases:
//   1. Poll the cheap TCP control-port check until the port is up.
//   2. Poll GETINFO status/circuit-established until it reports "1".
// Returns success only when a circuit exists, so callers that flip on routing
// in the success path never strand the browser behind a still-bootstrapping
// (or stuck) daemon. On timeout, success=false + a clear error, and the caller
// tears the daemon down. |cookie_path| is <datadir>/control_auth_cookie.
TorLaunchResult DoWaitForBootstrap(int total_ms, std::string cookie_path) {
  TorLaunchResult r;
  const int kPollIntervalMs = 500;
  int waited = 0;
  bool port_up = false;
  while (waited < total_ms) {
    if (!port_up) {
      port_up = TryConnectControlPort();
    }
    if (port_up && TorCircuitEstablishedBlocking(cookie_path)) {
      r.success = true;
      return r;
    }
    base::PlatformThread::Sleep(base::Milliseconds(kPollIntervalMs));
    waited += kPollIntervalMs;
  }
  r.success = false;
  r.error = port_up
                ? ("Tor connected but couldn't build a circuit within " +
                   base::NumberToString(total_ms / 1000) +
                   "s (network may be blocking Tor).")
                : ("Tor did not become reachable within " +
                   base::NumberToString(total_ms / 1000) + "s.");
  return r;
}

}  // namespace

// static
TorManager* TorManager::Get() {
  static base::NoDestructor<TorManager> instance;
  return instance.get();
}

TorManager::TorManager() = default;

TorManager::~TorManager() {
  // Best-effort cleanup. NoDestructor never actually runs this in
  // production, but useful for tests.
  Stop();
}

base::FilePath TorManager::GetDataDir() const {
  return ResolveAppSupportRoot();
}

base::FilePath TorManager::GetTorrcPath() const {
  return GetDataDir().AppendASCII("torrc");
}

bool TorManager::WriteTorrc() {
  base::FilePath dir = GetDataDir();
  if (dir.value().empty()) return false;
  if (!base::CreateDirectory(dir)) return false;
  // Make sure the in-memory exit-country reflects any persisted value
  // before we serialize the torrc.
  EnsureExitCountryLoaded();
  // Write a minimal torrc that:
  //   - Binds SocksPort + ControlPort on loopback only
  //   - Uses cookie auth so anything besides MoltBrowser (running as
  //     the same user) can't connect
  //   - Keeps Tor's data dir under MoltBrowser/tor so we never touch
  //     a system Tor's state.
  //
  // Paths are emitted via AsUTF8Unsafe(): FilePath::value() is a
  // std::wstring on Windows and can't be concatenated into a
  // std::string. Tor on Windows accepts the resulting (back-slashed)
  // UTF-8 path. (Code-review: keep paths UTF-8 across platforms.)
  std::string contents;
  contents += "# Auto-generated by MoltBrowser. Do not edit.\n";
  contents += "SocksPort 127.0.0.1:9050\n";
  contents += "ControlPort 127.0.0.1:9051\n";
  contents += "CookieAuthentication 1\n";
  contents += "DataDirectory " + dir.AsUTF8Unsafe() + "\n";
  contents +=
      "Log notice file " + dir.AppendASCII("tor.log").AsUTF8Unsafe() + "\n";
  contents += "AvoidDiskWrites 1\n";
  // Point Tor at our bundled GeoIP DB if it's present. Without this,
  // a bundled tor wouldn't know where to find its country DB (system-
  // installed Tor expects /usr/local/share/tor/geoip but we don't
  // ship to that path). GETINFO ip-to-country and ExitNodes {cc}
  // country matching both depend on this.
  base::FilePath geo = GetBundledTorDir().AppendASCII("geoip");
  base::FilePath geo6 = GetBundledTorDir().AppendASCII("geoip6");
  if (base::PathExists(geo))
    contents += "GeoIPFile " + geo.AsUTF8Unsafe() + "\n";
  if (base::PathExists(geo6))
    contents += "GeoIPv6File " + geo6.AsUTF8Unsafe() + "\n";
  // Exit-country constraint. When set, pin the exit relay to the chosen
  // country and make the constraint strict (StrictNodes 1 means Tor
  // will refuse rather than silently fall back to another country).
  // When empty, omit both lines so Tor picks any exit as usual.
  if (!exit_country_.empty()) {
    contents += "ExitNodes {" + exit_country_ + "}\n";
    contents += "StrictNodes 1\n";
  }
  return base::WriteFile(GetTorrcPath(), contents);
}

namespace {

// The bundled tor executable's filename: "tor.exe" on Windows, "tor"
// elsewhere. Kept in one place so ResolveTorBinary() and
// IsUsingBundledTor() always agree on what we're looking for.
base::FilePath BundledTorBinaryName() {
#if BUILDFLAG(IS_WIN)
  return base::FilePath(FILE_PATH_LITERAL("tor.exe"));
#else
  return base::FilePath(FILE_PATH_LITERAL("tor"));
#endif
}

// Full path to the bundled tor binary inside |dir|, or empty if |dir|
// is empty.
base::FilePath BundledTorBinaryIn(const base::FilePath& dir) {
  if (dir.value().empty()) return base::FilePath();
  return dir.Append(BundledTorBinaryName());
}

}  // namespace

base::FilePath TorManager::GetBundledTorDir() const {
#if BUILDFLAG(IS_MAC)
  // On macOS the running MoltBrowser executable lives at
  //   MoltBrowser.app/Contents/MacOS/MoltBrowser
  // The bundled tor sits at
  //   MoltBrowser.app/Contents/Resources/tor/tor
  base::FilePath exe;
  if (!base::PathService::Get(base::FILE_EXE, &exe)) return base::FilePath();
  base::FilePath contents = exe.DirName().DirName();
  return contents.AppendASCII("Resources").AppendASCII("tor");
#elif BUILDFLAG(IS_WIN)
  // On Windows the bundled tor sits next to molt.exe in a "tor"
  // subdirectory of the install dir:  <DIR_EXE>\tor\tor.exe  (with the
  // GeoIP DBs alongside it). The packaging step copies tor.exe + the
  // geoip/geoip6 files into that folder.
  base::FilePath exe_dir;
  if (!base::PathService::Get(base::DIR_EXE, &exe_dir))
    return base::FilePath();
  return exe_dir.Append(FILE_PATH_LITERAL("tor"));
#else
  // Linux bundling lands in a follow-up — for now the system path
  // lookup in ResolveTorBinary() handles that platform.
  return base::FilePath();
#endif
}

base::FilePath TorManager::ResolveTorBinary() const {
  // 1) Bundled tor first — gives the seamless out-of-box experience.
  base::FilePath bundled = BundledTorBinaryIn(GetBundledTorDir());
  if (!bundled.value().empty() && base::PathExists(bundled)
#if !BUILDFLAG(IS_WIN)
      // access(X_OK) is POSIX-only; on Windows fall back to existence.
      && access(bundled.value().c_str(), X_OK) == 0
#endif
  ) {
    return bundled;
  }
  // 2) System-installed tor (power users / dev fallback).
#if BUILDFLAG(IS_WIN)
  // Windows has no canonical install path for tor and no execute-bit
  // concept. If the user dropped a tor.exe next to the install (e.g. an
  // unpacked Tor Expert Bundle), prefer that; otherwise fall back to a
  // bare "tor.exe" name and let base::LaunchProcess() resolve it via
  // the PATH environment variable at spawn time (CreateProcess searches
  // PATH). If neither the bundle nor PATH has tor, the spawn fails and
  // the bootstrap wait reports a clear timeout error.
  base::FilePath exe_dir;
  if (base::PathService::Get(base::DIR_EXE, &exe_dir)) {
    base::FilePath side = exe_dir.Append(FILE_PATH_LITERAL("tor.exe"));
    if (base::PathExists(side))
      return side;
  }
  return base::FilePath(FILE_PATH_LITERAL("tor.exe"));
#else
  for (const char* p : kCandidatePaths) {
    base::FilePath fp = base::FilePath::FromUTF8Unsafe(p);
    if (base::PathExists(fp) && access(fp.value().c_str(), X_OK) == 0) {
      return fp;
    }
  }
  return base::FilePath();
#endif  // BUILDFLAG(IS_WIN)
}

bool TorManager::IsUsingBundledTor() const {
  base::FilePath bundled = BundledTorBinaryIn(GetBundledTorDir());
  base::FilePath resolved = ResolveTorBinary();
  return !bundled.value().empty() && !resolved.value().empty() &&
         resolved.value() == bundled.value();
}

void TorManager::Launch(
    base::OnceCallback<void(TorLaunchResult)> on_ready) {
  const std::string cookie_path =
      GetDataDir().AppendASCII("control_auth_cookie").AsUTF8Unsafe();
  // If we already have a healthy child, just confirm it can route. If it's
  // still bootstrapping, give it up to 20s to establish a circuit.
  if (IsRunning()) {
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE,
        {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&DoWaitForBootstrap, /*total_ms=*/20000, cookie_path),
        std::move(on_ready));
    return;
  }
  base::FilePath bin = ResolveTorBinary();
  if (bin.empty()) {
    // On Windows ResolveTorBinary() always returns at least a bare
    // "tor.exe" (resolved via PATH at spawn time), so this branch is
    // reached only on macOS/Linux when no binary was found.
    TorLaunchResult r;
    r.error =
        "No tor binary found. Install with `brew install tor` (macOS) "
        "or `apt-get install tor` (Linux), then re-run /tor launch.";
    std::move(on_ready).Run(std::move(r));
    return;
  }
  if (!WriteTorrc()) {
    TorLaunchResult r;
    r.error =
        "Failed to write managed torrc to " + GetDataDir().AsUTF8Unsafe();
    std::move(on_ready).Run(std::move(r));
    return;
  }
  base::CommandLine cmd(bin);
  cmd.AppendArg("-f");
  cmd.AppendArgPath(GetTorrcPath());
  base::LaunchOptions opts;
#if BUILDFLAG(IS_POSIX)
  opts.new_process_group = true;  // POSIX-only LaunchOptions field
#endif
  base::Process p = base::LaunchProcess(cmd, opts);
  if (!p.IsValid()) {
    TorLaunchResult r;
    r.error = "Failed to spawn tor (LaunchProcess returned invalid).";
    r.binary_path = bin.AsUTF8Unsafe();
    std::move(on_ready).Run(std::move(r));
    return;
  }
  child_ = std::move(p);
  LOG(INFO) << "[MoltTor] launched " << bin.AsUTF8Unsafe()
            << " pid=" << child_.Pid();

  // Wait off-thread for the control port to come up.
  int pid_copy = child_.Pid();
  std::string bin_path_copy = bin.AsUTF8Unsafe();
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&DoWaitForBootstrap, /*total_ms=*/60000, cookie_path),
      base::BindOnce(
          [](base::OnceCallback<void(TorLaunchResult)> cb,
             std::string bin_path, int pid,
             TorLaunchResult inner) {
            inner.binary_path = bin_path;
            inner.pid = pid;
            std::move(cb).Run(std::move(inner));
          },
          std::move(on_ready), bin_path_copy, pid_copy));
}

void TorManager::Stop() {
  if (!child_.IsValid()) return;
#if BUILDFLAG(IS_WIN)
  // Windows has no POSIX signals, so we terminate via the cross-platform
  // base::Process API. Tor on Windows installs a console control handler
  // and shuts down on TerminateProcess; we wait for it to exit. Move the
  // wait+terminate to a worker thread so we don't block the UI thread or
  // leak the child.
  base::Process child = std::move(child_);
  child_ = base::Process();  // mark not-running on the UI side
  LOG(INFO) << "[MoltTor] terminating pid=" << child.Pid();
  base::ThreadPool::PostTask(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::BEST_EFFORT,
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(
          [](base::Process child) {
            child.Terminate(/*exit_code=*/0, /*wait=*/true);
          },
          std::move(child)));
#else
  // SIGTERM first — Tor handles it as a clean shutdown signal and
  // flushes its hidden service descriptors. Move the wait+SIGKILL to
  // a worker thread so we don't block the UI thread but also don't
  // leak the child (the previous version's "drop the handle and let
  // the OS reap" left orphans on a wedged Tor). Code-review LOW #15.
  base::Process child = std::move(child_);
  child_ = base::Process();  // mark not-running on the UI side
  const pid_t pid = child.Pid();
  LOG(INFO) << "[MoltTor] sending SIGTERM to pid=" << pid;
  base::ThreadPool::PostTask(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::BEST_EFFORT,
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(
          [](base::Process child) {
            const pid_t pid = child.Pid();
            ::kill(pid, SIGTERM);
            int exit_code = 0;
            // Give Tor 3s to shut down cleanly, then escalate.
            if (!child.WaitForExitWithTimeout(base::Seconds(3),
                                               &exit_code)) {
              LOG(WARNING) << "[MoltTor] pid=" << pid
                           << " ignored SIGTERM; sending SIGKILL";
              ::kill(pid, SIGKILL);
              child.WaitForExitWithTimeout(base::Seconds(1), &exit_code);
            }
            LOG(INFO) << "[MoltTor] pid=" << pid
                      << " exited with code=" << exit_code;
          },
          std::move(child)));
#endif  // BUILDFLAG(IS_WIN)
}

// ---------------------------------------------------------------------
// Exit-country selection
// ---------------------------------------------------------------------

namespace {

// Hex-encode a byte string for AUTHENTICATE <hex>.
std::string HexEncodeBytes(const std::string& bytes) {
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (unsigned char c : bytes) {
    out += kHex[(c >> 4) & 0xF];
    out += kHex[c & 0xF];
  }
  return out;
}

// Send |line| (CRLF appended) on |fd| and drain whatever comes back
// within the socket timeout. Best-effort: we don't parse the reply.
void ControlSendDrain(
#if BUILDFLAG(IS_WIN)
    SOCKET fd,
#else
    int fd,
#endif
    const std::string& line) {
  std::string cmd = line + "\r\n";
  send(fd, cmd.data(), static_cast<int>(cmd.size()), 0);
  char buf[1024];
  recv(fd, buf, sizeof(buf), 0);
}

// Like ControlSendDrain but returns the control-port reply so callers can
// parse a GETINFO result. One recv() is enough for the small single-line
// replies we query (bootstrap/circuit status).
std::string ControlSendRecv(
#if BUILDFLAG(IS_WIN)
    SOCKET fd,
#else
    int fd,
#endif
    const std::string& line) {
  std::string cmd = line + "\r\n";
  send(fd, cmd.data(), static_cast<int>(cmd.size()), 0);
  char buf[1024];
  int n = recv(fd, buf, sizeof(buf) - 1, 0);
  if (n <= 0) {
    return std::string();
  }
  return std::string(buf, static_cast<size_t>(n));
}

// Blocking: is Tor actually able to route yet? Connects to the control port,
// cookie-authenticates, and asks GETINFO status/circuit-established — which is
// "1" only once Tor has built at least one usable circuit (i.e. bootstrap is
// effectively done and traffic can flow). This is a stronger, more honest
// readiness signal than "the control port answered": we only flip MoltNet to
// Connected + apply routing once traffic will actually go through. Returns
// false on any connect/auth failure. Runs on a MayBlock() worker thread.
bool TorCircuitEstablishedBlocking(const std::string& cookie_path) {
  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(9051);
  addr.sin_addr.s_addr = htonl(0x7F000001);  // 127.0.0.1

  std::string cookie;
  base::ReadFileToString(base::FilePath::FromUTF8Unsafe(cookie_path), &cookie);

#if BUILDFLAG(IS_WIN)
  EnsureWinsockStarted();
  SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == INVALID_SOCKET)
    return false;
  DWORD ms = 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
             reinterpret_cast<const char*>(&ms), sizeof(ms));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
             reinterpret_cast<const char*>(&ms), sizeof(ms));
  if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) !=
      0) {
    closesocket(fd);
    return false;
  }
#else
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return false;
  struct timeval tv;
  tv.tv_sec = 1;
  tv.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) !=
      0) {
    close(fd);
    return false;
  }
#endif  // BUILDFLAG(IS_WIN)

  if (!cookie.empty())
    ControlSendDrain(fd, "AUTHENTICATE " + HexEncodeBytes(cookie));
  else
    ControlSendDrain(fd, "AUTHENTICATE");

  std::string resp = ControlSendRecv(fd, "GETINFO status/circuit-established");
  ControlSendDrain(fd, "QUIT");

#if BUILDFLAG(IS_WIN)
  closesocket(fd);
#else
  close(fd);
#endif
  // Reply looks like: 250-status/circuit-established=1\r\n250 OK
  return resp.find("circuit-established=1") != std::string::npos;
}

// Blocking: connect to the control port, authenticate with the cookie
// from our managed data dir (our torrc sets CookieAuthentication 1),
// and issue SIGNAL RELOAD followed by SIGNAL NEWNYM so Tor re-reads the
// torrc (picking up the new ExitNodes) and rebuilds circuits through
// the new exit. Returns true if the commands were dispatched. Runs on a
// MayBlock() worker thread. |cookie_path| is the absolute control auth
// cookie path (<datadir>/control_auth_cookie).
bool ReloadTorConfigBlocking(std::string cookie_path) {
  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(9051);
  addr.sin_addr.s_addr = htonl(0x7F000001);  // 127.0.0.1

  // Read the cookie (binary, ~32 bytes). If it's missing we still try
  // NULL auth in case the user reconfigured Tor without cookie auth.
  std::string cookie;
  base::ReadFileToString(base::FilePath::FromUTF8Unsafe(cookie_path),
                         &cookie);

#if BUILDFLAG(IS_WIN)
  EnsureWinsockStarted();
  SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == INVALID_SOCKET)
    return false;
  DWORD ms = 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
             reinterpret_cast<const char*>(&ms), sizeof(ms));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
             reinterpret_cast<const char*>(&ms), sizeof(ms));
  if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
              sizeof(addr)) != 0) {
    closesocket(fd);
    return false;
  }
#else
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return false;
  struct timeval tv;
  tv.tv_sec = 1;
  tv.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
              sizeof(addr)) != 0) {
    close(fd);
    return false;
  }
#endif  // BUILDFLAG(IS_WIN)

  // Authenticate. Prefer cookie auth (matches our managed torrc); fall
  // back to NULL auth if we couldn't read a cookie.
  if (!cookie.empty())
    ControlSendDrain(fd, "AUTHENTICATE " + HexEncodeBytes(cookie));
  else
    ControlSendDrain(fd, "AUTHENTICATE");

  // RELOAD makes Tor re-read torrc (new ExitNodes/StrictNodes); NEWNYM
  // drops existing circuits so new requests use a fresh exit.
  ControlSendDrain(fd, "SIGNAL RELOAD");
  ControlSendDrain(fd, "SIGNAL NEWNYM");
  ControlSendDrain(fd, "QUIT");

#if BUILDFLAG(IS_WIN)
  closesocket(fd);
#else
  close(fd);
#endif
  return true;
}

}  // namespace

base::FilePath TorManager::GetExitCountryPath() const {
  return GetDataDir().AppendASCII("exit_country");
}

void TorManager::EnsureExitCountryLoaded() const {
  if (exit_country_loaded_)
    return;
  exit_country_loaded_ = true;
  std::string contents;
  if (base::ReadFileToString(GetExitCountryPath(), &contents)) {
    std::string trimmed;
    base::TrimWhitespaceASCII(contents, base::TRIM_ALL, &trimmed);
    std::string normalized = base::ToLowerASCII(trimmed);
    // Validate as a strict ISO alpha-2 code (same rule as
    // SetExitCountry): never trust an on-disk value enough to splice it
    // into the torrc unchecked. Invalid/tampered content -> "any".
    if (normalized.size() == 2 && base::IsAsciiAlpha(normalized[0]) &&
        base::IsAsciiAlpha(normalized[1])) {
      exit_country_ = normalized;
    }
  }
}

void TorManager::SetExitCountry(const std::string& country_code) {
  EnsureExitCountryLoaded();
  // Normalize: lowercase, trimmed. "" clears the constraint.
  std::string trimmed;
  base::TrimWhitespaceASCII(country_code, base::TRIM_ALL, &trimmed);
  std::string normalized = base::ToLowerASCII(trimmed);
  // Validate strictly: an ISO 3166-1 alpha-2 code is exactly two ASCII
  // letters. Anything else (empty, junk, or — importantly — strings
  // containing whitespace/newlines/braces) is treated as "clear". This
  // also prevents torrc injection: the value is written verbatim into
  // `ExitNodes {<cc>}`, so we must never let arbitrary text through.
  bool valid = normalized.size() == 2 &&
               base::IsAsciiAlpha(normalized[0]) &&
               base::IsAsciiAlpha(normalized[1]);
  exit_country_ = valid ? normalized : std::string();

  // Best-effort persist to the data dir so the choice survives restarts.
  // Create the dir first (it may not exist if Tor never launched).
  base::FilePath dir = GetDataDir();
  if (!dir.value().empty()) {
    base::CreateDirectory(dir);
    base::WriteFile(GetExitCountryPath(), exit_country_);
  }

  // Rewrite the torrc with (or without) the ExitNodes lines.
  WriteTorrc();

  // If Tor is up, reload it in place so the change takes effect now.
  // The cookie lives at <datadir>/control_auth_cookie for our managed
  // instance. Do the blocking socket work on a worker thread.
  if (IsRunning()) {
    std::string cookie_path =
        dir.AppendASCII("control_auth_cookie").AsUTF8Unsafe();
    base::ThreadPool::PostTask(
        FROM_HERE,
        {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(base::IgnoreResult(&ReloadTorConfigBlocking),
                       std::move(cookie_path)));
  }
}

std::string TorManager::GetExitCountry() const {
  EnsureExitCountryLoaded();
  return exit_country_;
}

std::vector<std::string> TorManager::GetAvailableExitCountries() const {
  // Curated list of commonly-used, generally well-connected Tor exit
  // countries (lowercase ISO 3166-1 alpha-2). This is a static list for
  // the picker UI; a future enhancement could query the live consensus
  // (e.g. GETINFO ns/all parsed for exit flags, or md/all) to build the
  // set of countries that actually have exit capacity right now.
  return {
      "us",  // United States
      "gb",  // United Kingdom
      "de",  // Germany
      "fr",  // France
      "nl",  // Netherlands
      "ch",  // Switzerland
      "se",  // Sweden
      "no",  // Norway
      "fi",  // Finland
      "ca",  // Canada
      "jp",  // Japan
      "sg",  // Singapore
      "au",  // Australia
      "es",  // Spain
      "it",  // Italy
      "at",  // Austria
      "pl",  // Poland
      "cz",  // Czechia
      "ro",  // Romania
      "is",  // Iceland
  };
}

}  // namespace tor
}  // namespace molt_ai
