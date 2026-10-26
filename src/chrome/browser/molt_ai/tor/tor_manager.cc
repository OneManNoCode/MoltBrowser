// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/tor_manager.h"

#include "build/build_config.h"

#if !BUILDFLAG(IS_WIN)
#include <signal.h>
#include <unistd.h>
#endif  // !BUILDFLAG(IS_WIN)

#include <utility>

#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/process/process_handle.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/common/helper_binary.h"
#include "chrome/browser/molt_ai/tor/molt_net_cpu.h"
#include "chrome/browser/molt_ai/tor/pluggable_transports.h"
#include "chrome/browser/molt_ai/tor/tor_control_protocol.h"
#include "chrome/browser/molt_ai/tor/torrc_builder.h"

namespace molt_ai {
namespace tor {

// Result of the off-thread spawn step.
struct TorSpawnResult {
  base::Process process;
  std::string error;
};

// Result of the off-thread bootstrap wait.
struct TorBootstrapResult {
  bool success = false;
  std::string error;
  uint16_t control_port = 0;
  uint16_t socks_port = 0;
  // The authenticated connection that sent TAKEOWNERSHIP.
  std::unique_ptr<TorControlConnection> owner;
};

namespace {

constexpr char kTorrcName[] = "torrc";
constexpr char kDefaultsTorrcName[] = "torrc-defaults";
constexpr char kControlPortFileName[] = "control_port";
constexpr char kCookieFileName[] = "control_auth_cookie";
constexpr char kLogFileName[] = "tor.log";
constexpr char kBridgesFileName[] = "bridges.json";
// Older builds persisted the exit-country pin here. It is session-only now.
constexpr char kLegacyExitCountryFileName[] = "exit_country";

constexpr char kPtDirName[] = "pluggable_transports";
constexpr char kPtConfigName[] = "pt_config.json";

// Cold start through bridges/snowflake can take a while.
constexpr int kColdBootstrapMs = 90000;
// Already-running Tor: just confirm it can still route.
constexpr int kWarmCheckMs = 20000;
constexpr int kPollIntervalMs = 500;

// Per-user MoltNet Tor state dir. Private to MoltBrowser so a system Tor's
// data dir / cookie is never touched.
base::FilePath ResolveAppSupportRoot() {
  base::FilePath home;
  if (!base::PathService::Get(base::DIR_HOME, &home)) {
    return base::FilePath();
  }
#if BUILDFLAG(IS_MAC)
  return home.AppendASCII("Library")
      .AppendASCII("Application Support")
      .AppendASCII("MoltBrowser")
      .AppendASCII("tor");
#else
  return home.AppendASCII(".moltbrowser").AppendASCII("tor");
#endif
}

base::FilePath BundledTorBinaryName() {
#if BUILDFLAG(IS_WIN)
  return base::FilePath(FILE_PATH_LITERAL("tor.exe"));
#else
  return base::FilePath(FILE_PATH_LITERAL("tor"));
#endif
}

// Linux distribution packages (MoltBrowser's .deb/.rpm say "Recommends:
// tor"). Used only when no tor is bundled, and only if root owns the file and
// every directory above it (helper_binary.h). Never PATH, /usr/local or
// Homebrew.
std::vector<std::string> SystemTorPaths() {
#if BUILDFLAG(IS_LINUX)
  return {"/usr/bin/tor", "/usr/sbin/tor"};
#else
  return {};
#endif
}

// Linux packages don't bundle Tor or its pluggable transports; elsewhere
// only the copies bundled with MoltBrowser are used.
TransportSource PlatformTransportSource() {
#if BUILDFLAG(IS_LINUX)
  return TransportSource::kSystemPackages;
#else
  return TransportSource::kBundled;
#endif
}

// The distribution's transport packages (Linux only), checked like the
// system tor: root must own the file and every directory above it.
std::vector<std::string> SystemTransportPaths(BridgeTransport transport) {
#if BUILDFLAG(IS_LINUX)
  return LinuxSystemTransportPaths(transport);
#else
  return {};
#endif
}

// <bundled tor dir>/pluggable_transports/<name>[.exe].
base::FilePath BundledTransportFile(BridgeTransport transport) {
  std::string name(BundledTransportFileName(transport));
#if BUILDFLAG(IS_WIN)
  name += ".exe";
#endif
  return base::FilePath::FromASCII(name);
}

// How Tor, whose working directory is the bundled tor dir, execs a bundled
// transport.
std::string BundledTransportExec(BridgeTransport transport) {
  const std::string name(BundledTransportFileName(transport));
#if BUILDFLAG(IS_WIN)
  return std::string(kPtDirName) + "\\" + name + ".exe";
#else
  return "./" + std::string(kPtDirName) + "/" + name;
#endif
}

// A pluggable transport Tor may run for |transport| bridges: the bundled
// copy in |pt_dir| (which may be empty or missing), else on Linux the
// distribution's root-owned package (pluggable_transports.h). Both go
// through FindHelperBinary's checks.
struct TransportLookup {
  // For the ClientTransportPlugin line; empty if there is no usable one.
  std::string exec;
  // When |exec| is empty: user-facing explanation and what to do.
  std::string error;
};

TransportLookup FindTransport(const base::FilePath& pt_dir,
                              BridgeTransport transport) {
  TransportLookup out;
  const molt_ai::HelperBinary found = molt_ai::FindHelperBinary(
      pt_dir, {BundledTransportFile(transport)},
      SystemTransportPaths(transport));
  if (!found.found()) {
    out.error = TransportUnavailableMessage(
        transport, PlatformTransportSource(), found.rejected_existing,
        found.error);
    return out;
  }
  // A system transport runs by its canonical absolute path (no working
  // directory involved); a bundled one relative to the bundled tor dir.
  std::string exec = found.bundled ? BundledTransportExec(transport)
                                   : found.path.AsUTF8Unsafe();
  if (!IsValidTransportExec(exec)) {
    // E.g. a symlink that resolves to a path with a space in it, which Tor
    // would split.
    out.error = TransportUnavailableMessage(
        transport, PlatformTransportSource(), /*rejected_existing=*/true,
        exec + " can't be used in Tor's configuration");
    return out;
  }
  out.exec = std::move(exec);
  return out;
}

BridgeSupport ComputeBridgeSupportBlocking(const base::FilePath& bundled_dir) {
  BridgeSupport support;
  const base::FilePath pt_dir = bundled_dir.empty()
                                    ? base::FilePath()
                                    : bundled_dir.AppendASCII(kPtDirName);
  support.obfs4_problem = FindTransport(pt_dir, BridgeTransport::kObfs4).error;
  support.snowflake_problem =
      FindTransport(pt_dir, BridgeTransport::kSnowflake).error;
  if (!support.obfs4_problem.empty() && !support.snowflake_problem.empty()) {
    support.no_transport_problem =
        NoTransportsMessage(PlatformTransportSource());
  }
  if (pt_dir.empty() || !base::PathExists(pt_dir.AppendASCII(kPtConfigName))) {
    support.built_in_problem =
        BuiltInBridgesUnavailableMessage(PlatformTransportSource());
  }
  return support;
}

// What the user should do when there is no tor MoltNet can run.
std::string TorUnavailableMessage(const molt_ai::HelperBinary& found,
                                  bool explicit_switch) {
  if (explicit_switch) {
    return "MoltNet can't use the tor binary given with --" +
           std::string(TorManager::kTorBinarySwitch) + ": " + found.error + ".";
  }
#if BUILDFLAG(IS_LINUX)
  if (found.rejected_existing) {
    return "MoltNet found a tor binary it won't run (" + found.error +
           "). Install or reinstall the tor package from your "
           "distribution, then try again.";
  }
  return "Tor isn't installed. Install the tor package (for example "
         "\"sudo apt install tor\" or \"sudo dnf install tor\"), then try "
         "again.";
#else
  return "This build of MoltBrowser has no usable bundled Tor (" + found.error +
         "). Reinstall MoltBrowser. (Advanced: start it with "
         "--moltnet-tor-binary=<absolute path to tor>.)";
#endif
}

BridgeSettings ReadBridgeSettingsBlocking(const base::FilePath& file) {
  BridgeSettings settings;
  std::string json;
  if (!base::ReadFileToStringWithMaxSize(file, &json, 64 * 1024)) {
    return settings;
  }
  std::optional<base::DictValue> dict =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!dict) {
    return settings;
  }
  const std::string* mode = dict->FindString("mode");
  std::optional<BridgeSettings::Mode> parsed =
      mode ? BridgeModeFromString(*mode) : std::nullopt;
  if (!parsed) {
    return settings;
  }
  settings.mode = *parsed;
  if (const std::string* lines = dict->FindString("custom_lines")) {
    settings.custom_lines = *lines;
  }
  return settings;
}

bool WriteBridgeSettingsBlocking(const base::FilePath& dir,
                                 const BridgeSettings& settings) {
  if (dir.empty() || !base::CreateDirectory(dir)) {
    return false;
  }
  base::DictValue dict;
  dict.Set("mode", BridgeModeToString(settings.mode));
  dict.Set("custom_lines", settings.custom_lines);
  std::optional<std::string> json = base::WriteJson(dict);
  return json && base::WriteFile(dir.AppendASCII(kBridgesFileName), *json);
}

// Turns the persisted bridge choice into validated torrc Bridge lines. Any
// problem is an error (a user-facing sentence): if the user asked for
// bridges (e.g. because direct Tor is blocked or dangerous where they are)
// we must not silently connect without them.
std::optional<std::vector<std::string>> ResolveBridgeLinesBlocking(
    const BridgeSettings& settings,
    const base::FilePath& pt_config,
    std::string* error) {
  switch (settings.mode) {
    case BridgeSettings::Mode::kNone:
      return std::vector<std::string>();
    case BridgeSettings::Mode::kCustom: {
      std::string why;
      std::optional<std::vector<std::string>> lines =
          ParseBridgeLines(settings.custom_lines, &why);
      if (!lines) {
        *error = "Bridges: " + why + ".";
        return std::nullopt;
      }
      if (lines->empty()) {
        *error = "Bridges: custom bridges are selected but none were entered.";
        return std::nullopt;
      }
      return lines;
    }
    case BridgeSettings::Mode::kSnowflake:
    case BridgeSettings::Mode::kObfs4: {
      const bool snowflake = settings.mode == BridgeSettings::Mode::kSnowflake;
      const BridgeTransport wanted =
          snowflake ? BridgeTransport::kSnowflake : BridgeTransport::kObfs4;
      std::string json;
      if (pt_config.empty() ||
          !base::ReadFileToStringWithMaxSize(pt_config, &json, 256 * 1024)) {
        *error = BuiltInBridgesUnavailableMessage(PlatformTransportSource());
        return std::nullopt;
      }
      std::optional<base::DictValue> dict =
          base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
      const base::DictValue* bridges =
          dict ? dict->FindDict("bridges") : nullptr;
      const base::ListValue* list =
          bridges ? bridges->FindList(snowflake ? "snowflake" : "obfs4")
                  : nullptr;
      std::vector<std::string> out;
      if (list) {
        for (const base::Value& value : *list) {
          if (!value.is_string()) {
            continue;
          }
          std::optional<std::string> line =
              NormalizeBridgeLine(value.GetString(), nullptr);
          if (line && GetBridgeTransport(*line) == wanted) {
            out.push_back(std::move(*line));
          } else {
            LOG(WARNING) << "[MoltTor] skipping invalid built-in bridge line";
          }
        }
      }
      if (out.empty()) {
        *error = snowflake ? "Bridges: no usable built-in snowflake bridges."
                           : "Bridges: no usable built-in obfs4 bridges.";
        return std::nullopt;
      }
      return out;
    }
  }
  return std::nullopt;
}

struct SpawnSpec {
  base::FilePath binary;
  base::FilePath data_dir;
  base::FilePath bundled_dir;
  std::string exit_country;
  int64_t owner_pid = 0;
};

std::unique_ptr<TorSpawnResult> SpawnTorBlocking(SpawnSpec spec) {
  auto result = std::make_unique<TorSpawnResult>();
  const base::FilePath& dir = spec.data_dir;
  if (dir.empty() || !base::CreateDirectory(dir)) {
    result->error = "Could not create the Tor data directory.";
    return result;
  }
#if BUILDFLAG(IS_POSIX)
  base::SetPosixFilePermissions(dir, base::FILE_PERMISSION_USER_MASK);
#endif
  // Never trust state left by a previous run: a stale port file or cookie
  // could point us at (or authenticate us to) something that isn't this Tor.
  base::DeleteFile(dir.AppendASCII(kControlPortFileName));
  base::DeleteFile(dir.AppendASCII(kCookieFileName));
  base::DeleteFile(dir.AppendASCII(kLegacyExitCountryFileName));

  TorrcOptions options;
  options.data_dir = dir.AsUTF8Unsafe();
  options.cookie_file = dir.AppendASCII(kCookieFileName).AsUTF8Unsafe();
  options.control_port_file =
      dir.AppendASCII(kControlPortFileName).AsUTF8Unsafe();
  options.log_file = dir.AppendASCII(kLogFileName).AsUTF8Unsafe();
  options.exit_country = spec.exit_country;

  base::FilePath pt_dir;
  if (!spec.bundled_dir.empty()) {
    base::FilePath geo = spec.bundled_dir.AppendASCII("geoip");
    base::FilePath geo6 = spec.bundled_dir.AppendASCII("geoip6");
    if (base::PathExists(geo)) {
      options.geoip_file = geo.AsUTF8Unsafe();
    }
    if (base::PathExists(geo6)) {
      options.geoip6_file = geo6.AsUTF8Unsafe();
    }
    pt_dir = spec.bundled_dir.AppendASCII(kPtDirName);
  }

  std::string error;
  BridgeSettings bridges =
      ReadBridgeSettingsBlocking(dir.AppendASCII(kBridgesFileName));
  std::optional<std::vector<std::string>> lines = ResolveBridgeLinesBlocking(
      bridges,
      pt_dir.empty() ? base::FilePath() : pt_dir.AppendASCII(kPtConfigName),
      &error);
  if (!lines) {
    result->error = error;
    return result;
  }
  // Only the transports these bridges need, so a missing one says exactly
  // what to install.
  for (BridgeTransport transport : RequiredTransports(*lines)) {
    TransportLookup pt = FindTransport(pt_dir, transport);
    if (pt.exec.empty()) {
      result->error = pt.error;
      return result;
    }
    if (transport == BridgeTransport::kObfs4) {
      options.obfs4_exec = std::move(pt.exec);
    } else {
      options.snowflake_exec = std::move(pt.exec);
    }
  }
  options.bridge_lines = std::move(*lines);

  std::optional<std::string> torrc = BuildTorrc(options, &error);
  if (!torrc) {
    result->error = "Could not write a safe Tor configuration: " + error + ".";
    return result;
  }
  const base::FilePath torrc_path = dir.AppendASCII(kTorrcName);
  const base::FilePath defaults_path = dir.AppendASCII(kDefaultsTorrcName);
  if (!base::WriteFile(torrc_path, *torrc) ||
      !base::WriteFile(defaults_path,
                       "# Intentionally empty: keeps any system-wide "
                       "torrc-defaults out of MoltNet's Tor.\n")) {
    result->error = "Could not write the Tor configuration.";
    return result;
  }

  base::CommandLine cmd(spec.binary);
  cmd.AppendArg("--defaults-torrc");
  cmd.AppendArgPath(defaults_path);
  cmd.AppendArg("-f");
  cmd.AppendArgPath(torrc_path);
  // Tor polls this pid and exits when the browser goes away (even on crash).
  cmd.AppendArg("__OwningControllerProcess");
  cmd.AppendArg(base::NumberToString(spec.owner_pid));

  base::LaunchOptions launch_options;
  // Bundled pluggable transports are referenced relative to the bundled tor
  // dir (a system one on Linux by its absolute path).
  if (!spec.bundled_dir.empty() && base::DirectoryExists(spec.bundled_dir)) {
    launch_options.current_directory = spec.bundled_dir;
  }
#if BUILDFLAG(IS_POSIX)
  launch_options.new_process_group = true;
#endif
#if BUILDFLAG(IS_WIN)
  launch_options.start_hidden = true;  // tor.exe is a console program.
#endif
  result->process = base::LaunchProcess(cmd, launch_options);
  if (!result->process.IsValid()) {
    result->error = "Failed to start tor.";
  }
  return result;
}

std::string SecondsText(int ms) {
  return base::NumberToString(ms / 1000) + "s";
}

// Waits for Tor to (1) publish its control port, (2) pass SAFECOOKIE,
// (3) accept TAKEOWNERSHIP, (4) report its SOCKS port and (5) have an
// established circuit. Success means MoltNet traffic can flow.
std::unique_ptr<TorBootstrapResult> BootstrapBlocking(
    base::FilePath control_port_file,
    base::FilePath cookie_path,
    int total_ms) {
  auto result = std::make_unique<TorBootstrapResult>();
  const base::TimeTicks deadline =
      base::TimeTicks::Now() + base::Milliseconds(total_ms);
  TorControlEndpoint endpoint;
  endpoint.cookie_path = cookie_path;
  std::string last_error = "Tor did not open its control port";
  while (base::TimeTicks::Now() < deadline) {
    if (!endpoint.port) {
      std::string contents;
      if (base::ReadFileToStringWithMaxSize(control_port_file, &contents,
                                            4096)) {
        if (std::optional<uint16_t> port =
                control::ParseControlPortFile(contents)) {
          endpoint.port = *port;
        }
      }
    }
    if (endpoint.port && !result->owner) {
      auto connection = std::make_unique<TorControlConnection>();
      std::string error;
      TorControlConnection::AuthResult auth =
          connection->ConnectAndAuthenticate(endpoint, &error);
      if (auth == TorControlConnection::AuthResult::kAuthenticationFailed) {
        result->error = "Tor control authentication failed: " + error + ".";
        return result;
      }
      if (auth == TorControlConnection::AuthResult::kOk) {
        std::optional<std::string> reply =
            connection->SendCommand("TAKEOWNERSHIP");
        if (!reply || !control::IsOkReply(*reply)) {
          result->error = "Tor refused TAKEOWNERSHIP.";
          return result;
        }
        reply = connection->SendCommand("GETINFO net/listeners/socks");
        std::optional<uint16_t> socks =
            reply ? control::ParseSocksListenerReply(*reply) : std::nullopt;
        if (!socks) {
          result->error = "Tor did not report a loopback SOCKS port.";
          return result;
        }
        result->control_port = endpoint.port;
        result->socks_port = *socks;
        result->owner = std::move(connection);
      } else {
        last_error = error;
      }
    }
    if (result->owner) {
      std::optional<std::string> reply =
          result->owner->SendCommand("GETINFO status/circuit-established");
      if (!reply) {
        result->owner.reset();
        result->error = "Lost the Tor control connection during bootstrap.";
        return result;
      }
      if (control::ParseGetInfoValue(*reply, "status/circuit-established") ==
          "1") {
        result->success = true;
        return result;
      }
      last_error =
          "Tor is running but couldn't build a circuit (the network may be "
          "blocking Tor — try bridges in MoltNet settings)";
    }
    base::PlatformThread::Sleep(base::Milliseconds(kPollIntervalMs));
  }
  result->error = last_error + " within " + SecondsText(total_ms) + ".";
  return result;
}

// Confirms an already-running Tor can still route.
TorLaunchResult CheckCircuitBlocking(TorControlEndpoint endpoint,
                                     int total_ms) {
  TorLaunchResult result;
  const base::TimeTicks deadline =
      base::TimeTicks::Now() + base::Milliseconds(total_ms);
  std::string error = "Tor is not reachable";
  while (base::TimeTicks::Now() < deadline) {
    TorControlConnection connection;
    TorControlConnection::AuthResult auth =
        connection.ConnectAndAuthenticate(endpoint, &error);
    if (auth == TorControlConnection::AuthResult::kAuthenticationFailed) {
      result.error = "Tor control authentication failed: " + error + ".";
      return result;
    }
    if (auth == TorControlConnection::AuthResult::kOk) {
      std::optional<std::string> reply =
          connection.SendCommand("GETINFO status/circuit-established");
      if (reply && control::ParseGetInfoValue(
                       *reply, "status/circuit-established") == "1") {
        result.success = true;
        return result;
      }
      error = "Tor has no established circuit";
    }
    base::PlatformThread::Sleep(base::Milliseconds(kPollIntervalMs));
  }
  result.error = error + " within " + SecondsText(total_ms) + ".";
  return result;
}

struct CommandResult {
  bool ok = false;
  std::string error;
};

// Authenticates and runs |commands| in order; every reply must be "250 OK".
CommandResult RunCommandsBlocking(TorControlEndpoint endpoint,
                                  std::vector<std::string> commands) {
  CommandResult result;
  TorControlConnection connection;
  if (connection.ConnectAndAuthenticate(endpoint, &result.error) !=
      TorControlConnection::AuthResult::kOk) {
    return result;
  }
  for (const std::string& command : commands) {
    std::optional<std::string> reply = connection.SendCommand(command);
    if (!reply || !control::IsOkReply(*reply)) {
      const std::string verb = command.substr(0, command.find(' '));
      result.error = "Tor rejected " + verb;
      return result;
    }
  }
  result.ok = true;
  return result;
}

// Tor rate-limits NEWNYM: within ten seconds of the previous one it answers
// "250 OK" at once but applies the signal only when the ten seconds are up,
// and until then new streams still get the circuits (and exits) the old
// identity used. So New Identity listens for Tor's SIGNAL event and only
// reports success once Tor says it has switched.
constexpr base::TimeDelta kNewnymAppliedTimeout = base::Seconds(15);

CommandResult SignalNewnymAndWaitBlocking(TorControlEndpoint endpoint) {
  CommandResult result;
  TorControlConnection connection;
  if (connection.ConnectAndAuthenticate(endpoint, &result.error) !=
      TorControlConnection::AuthResult::kOk) {
    return result;
  }
  for (const char* command : {"SETEVENTS SIGNAL", "SIGNAL NEWNYM"}) {
    // A NEWNYM applied before ours (its event can still arrive first) is
    // skipped here: SendCommand() returns the command's own reply.
    std::optional<std::string> reply = connection.SendCommand(command);
    if (!reply || !control::IsOkReply(*reply)) {
      result.error = std::string("Tor rejected ") + command;
      return result;
    }
  }
  const base::TimeTicks deadline =
      base::TimeTicks::Now() + kNewnymAppliedTimeout;
  while (std::optional<std::string> event = connection.ReadEvent(deadline)) {
    if (control::IsNewnymAppliedEvent(*event)) {
      result.ok = true;
      return result;
    }
  }
  result.error = "Tor didn't confirm that it switched to new circuits";
  return result;
}

void RunDone(TorManager::DoneCallback done, CommandResult result) {
  if (done) {
    std::move(done).Run(result.ok, result.error);
  }
}

}  // namespace

BridgeSettings::BridgeSettings() = default;
BridgeSettings::BridgeSettings(const BridgeSettings&) = default;
BridgeSettings& BridgeSettings::operator=(const BridgeSettings&) = default;
BridgeSettings::~BridgeSettings() = default;

BridgeSupport::BridgeSupport() = default;
BridgeSupport::BridgeSupport(const BridgeSupport&) = default;
BridgeSupport& BridgeSupport::operator=(const BridgeSupport&) = default;
BridgeSupport::~BridgeSupport() = default;

TorBinaryInfo::TorBinaryInfo() = default;
TorBinaryInfo::TorBinaryInfo(const TorBinaryInfo&) = default;
TorBinaryInfo& TorBinaryInfo::operator=(const TorBinaryInfo&) = default;
TorBinaryInfo::~TorBinaryInfo() = default;

std::string BridgeModeToString(BridgeSettings::Mode mode) {
  switch (mode) {
    case BridgeSettings::Mode::kNone:
      return "none";
    case BridgeSettings::Mode::kSnowflake:
      return "snowflake";
    case BridgeSettings::Mode::kObfs4:
      return "obfs4";
    case BridgeSettings::Mode::kCustom:
      return "custom";
  }
  return "none";
}

std::optional<BridgeSettings::Mode> BridgeModeFromString(std::string_view s) {
  if (s == "none") {
    return BridgeSettings::Mode::kNone;
  }
  if (s == "snowflake") {
    return BridgeSettings::Mode::kSnowflake;
  }
  if (s == "obfs4") {
    return BridgeSettings::Mode::kObfs4;
  }
  if (s == "custom") {
    return BridgeSettings::Mode::kCustom;
  }
  return std::nullopt;
}

// static
TorManager* TorManager::Get() {
  static base::NoDestructor<TorManager> instance;
  return instance.get();
}

TorManager::TorManager() = default;

TorManager::~TorManager() {
  // NoDestructor never runs this in production; useful for tests.
  Stop();
}

base::FilePath TorManager::GetDataDir() const {
  return ResolveAppSupportRoot();
}

base::FilePath TorManager::GetBundledTorDir() const {
#if BUILDFLAG(IS_MAC)
  // MoltBrowser.app/Contents/MacOS/MoltBrowser ->
  // MoltBrowser.app/Contents/Resources/tor/
  base::FilePath exe;
  if (!base::PathService::Get(base::FILE_EXE, &exe)) {
    return base::FilePath();
  }
  return exe.DirName().DirName().AppendASCII("Resources").AppendASCII("tor");
#else
  // <install dir>/tor/ (Windows packaging places tor.exe, geoip and the
  // pluggable transports there; Linux packages should do the same).
  base::FilePath exe_dir;
  if (!base::PathService::Get(base::DIR_EXE, &exe_dir)) {
    return base::FilePath();
  }
  return exe_dir.Append(FILE_PATH_LITERAL("tor"));
#endif
}

TorBinaryInfo TorManager::FindTorBinary() const {
  TorBinaryInfo info;
  // An explicit, absolute user override replaces the search entirely. No
  // Homebrew / PATH search: a binary found that way is whatever happens to
  // be installed (or planted), not what we tested.
  const base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();
  const bool explicit_switch = command_line->HasSwitch(kTorBinarySwitch);
  const molt_ai::HelperBinary found =
      explicit_switch
          ? molt_ai::CheckExplicitHelperBinary(
                command_line->GetSwitchValuePath(kTorBinarySwitch))
          : molt_ai::FindHelperBinary(
                GetBundledTorDir(), {BundledTorBinaryName()}, SystemTorPaths());
  if (!found.found()) {
    info.error = TorUnavailableMessage(found, explicit_switch);
    return info;
  }
  info.path = found.path;
  info.bundled = found.bundled;
  return info;
}

base::FilePath TorManager::ResolveTorBinary() const {
  return FindTorBinary().path;
}

bool TorManager::IsUsingBundledTor() const {
  return FindTorBinary().bundled;
}

bool TorManager::IsRunning() const {
  return child_.IsValid() && socks_port_ != 0 && control_port_ != 0;
}

TorControlEndpoint TorManager::GetControlEndpoint() const {
  TorControlEndpoint endpoint;
  if (IsRunning()) {
    endpoint.port = control_port_;
    endpoint.cookie_path = GetDataDir().AppendASCII(kCookieFileName);
  }
  return endpoint;
}

base::CallbackListSubscription TorManager::AddStateChangedCallback(
    base::RepeatingClosure callback) {
  return state_callbacks_.Add(std::move(callback));
}

void TorManager::NotifyStateChanged() {
  state_callbacks_.Notify();
}

void TorManager::Launch(base::OnceCallback<void(TorLaunchResult)> on_ready) {
  if (IsRunning()) {
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&CheckCircuitBlocking, GetControlEndpoint(),
                       kWarmCheckMs),
        std::move(on_ready));
    return;
  }
  pending_launches_.push_back(std::move(on_ready));
  if (launching_) {
    return;  // Coalesced into the in-flight launch.
  }
  launching_ = true;

  // MoltNet's Tor never starts on a processor MoltNet refuses
  // (molt_net_cpu_policy.h), whichever page or command asks.
  if (const std::string& cpu_error = MoltNetCpuError(); !cpu_error.empty()) {
    TorLaunchResult result;
    result.error = cpu_error;
    FinishLaunch(result);
    return;
  }

  const TorBinaryInfo tor = FindTorBinary();
  if (tor.path.empty()) {
    TorLaunchResult result;
    result.error = tor.error;
    FinishLaunch(result);
    return;
  }
  const base::FilePath& binary = tor.path;
  if (!tor.bundled) {
    LOG(INFO) << "[MoltTor] no bundled tor; using " << binary;
  }
  // A child that never finished bootstrapping (or an earlier failure) must
  // not linger.
  TearDownChild();

  SpawnSpec spec;
  spec.binary = binary;
  spec.data_dir = GetDataDir();
  spec.bundled_dir = GetBundledTorDir();
  spec.exit_country = exit_country_;
  spec.owner_pid = static_cast<int64_t>(base::GetCurrentProcId());
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&SpawnTorBlocking, std::move(spec)),
      base::BindOnce(&TorManager::OnSpawned, weak_factory_.GetWeakPtr(),
                     generation_, binary.AsUTF8Unsafe()));
}

void TorManager::OnSpawned(uint64_t generation,
                           std::string binary_path,
                           std::unique_ptr<TorSpawnResult> result) {
  if (generation != generation_) {
    // Stop() ran while we were spawning: make sure this child dies too.
    if (result->process.IsValid()) {
      base::ThreadPool::PostTask(
          FROM_HERE,
          {base::MayBlock(), base::TaskPriority::BEST_EFFORT,
           base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
          base::BindOnce(
              [](base::Process child) {
                child.Terminate(/*exit_code=*/0, /*wait=*/true);
              },
              std::move(result->process)));
    }
    return;
  }
  if (!result->process.IsValid()) {
    TorLaunchResult failed;
    failed.error = result->error;
    failed.binary_path = binary_path;
    FinishLaunch(failed);
    return;
  }
  child_ = std::move(result->process);
  LOG(INFO) << "[MoltTor] launched " << binary_path << " pid=" << child_.Pid();
  const base::FilePath dir = GetDataDir();
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&BootstrapBlocking, dir.AppendASCII(kControlPortFileName),
                     dir.AppendASCII(kCookieFileName), kColdBootstrapMs),
      base::BindOnce(&TorManager::OnBootstrapped, weak_factory_.GetWeakPtr(),
                     generation, std::move(binary_path)));
}

void TorManager::OnBootstrapped(uint64_t generation,
                                std::string binary_path,
                                std::unique_ptr<TorBootstrapResult> result) {
  if (generation != generation_ || !result->success) {
    // Close any owner connection off the UI thread (Tor exits with it).
    if (result->owner) {
      base::ThreadPool::PostTask(
          FROM_HERE, {base::MayBlock(), base::TaskPriority::BEST_EFFORT},
          base::BindOnce([](std::unique_ptr<TorControlConnection>) {},
                         std::move(result->owner)));
    }
    if (generation != generation_) {
      return;
    }
    // Bootstrap failed: tear the daemon down. Nothing is routed through it,
    // and nothing falls back to a direct connection either.
    TearDownChild();
    TorLaunchResult failed;
    failed.error = result->error;
    failed.binary_path = binary_path;
    FinishLaunch(failed);
    return;
  }
  socks_port_ = result->socks_port;
  control_port_ = result->control_port;
  owner_connection_ = std::move(result->owner);
  TorLaunchResult ok;
  ok.success = true;
  ok.binary_path = binary_path;
  ok.pid = static_cast<int>(child_.Pid());
  LOG(INFO) << "[MoltTor] ready: socks 127.0.0.1:" << socks_port_;
  FinishLaunch(ok);
  NotifyStateChanged();
}

void TorManager::FinishLaunch(const TorLaunchResult& result) {
  launching_ = false;
  std::vector<base::OnceCallback<void(TorLaunchResult)>> callbacks =
      std::move(pending_launches_);
  pending_launches_.clear();
  for (auto& callback : callbacks) {
    std::move(callback).Run(result);
  }
}

void TorManager::Stop() {
  ++generation_;
  const bool was_active = IsRunning() || launching_ || child_.IsValid();
  TearDownChild();
  if (launching_) {
    TorLaunchResult stopped;
    stopped.error = "MoltNet was turned off.";
    FinishLaunch(stopped);
  }
  if (was_active) {
    NotifyStateChanged();
  }
}

void TorManager::TearDownChild() {
  socks_port_ = 0;
  control_port_ = 0;
  if (owner_connection_) {
    // Closing the TAKEOWNERSHIP connection makes Tor exit on its own.
    base::ThreadPool::PostTask(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::BEST_EFFORT},
        base::BindOnce([](std::unique_ptr<TorControlConnection>) {},
                       std::move(owner_connection_)));
  }
  if (!child_.IsValid()) {
    return;
  }
  base::Process child = std::move(child_);
  child_ = base::Process();
#if BUILDFLAG(IS_WIN)
  LOG(INFO) << "[MoltTor] terminating pid=" << child.Pid();
  base::ThreadPool::PostTask(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::BEST_EFFORT,
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(
          [](base::Process process) {
            process.Terminate(/*exit_code=*/0, /*wait=*/true);
          },
          std::move(child)));
#else
  // SIGTERM first (clean shutdown), SIGKILL if it hangs. Off the UI thread so
  // we neither block nor leak the child.
  base::ThreadPool::PostTask(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::BEST_EFFORT,
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(
          [](base::Process process) {
            const pid_t pid = process.Pid();
            ::kill(pid, SIGTERM);
            int exit_code = 0;
            if (!process.WaitForExitWithTimeout(base::Seconds(3),
                                                &exit_code)) {
              LOG(WARNING) << "[MoltTor] pid=" << pid
                           << " ignored SIGTERM; sending SIGKILL";
              ::kill(pid, SIGKILL);
              process.WaitForExitWithTimeout(base::Seconds(1), &exit_code);
            }
          },
          std::move(child)));
#endif  // BUILDFLAG(IS_WIN)
}

void TorManager::SendNewIdentitySignal(DoneCallback done) {
  if (!IsRunning()) {
    std::move(done).Run(false, "MoltNet's Tor isn't running.");
    return;
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&SignalNewnymAndWaitBlocking, GetControlEndpoint()),
      base::BindOnce(&RunDone, std::move(done)));
}

void TorManager::SetExitCountry(const std::string& country_code,
                                DoneCallback done) {
  const std::string normalized = base::ToLowerASCII(
      base::TrimWhitespaceASCII(country_code, base::TRIM_ALL));
  if (!normalized.empty() && !IsValidExitCountry(normalized)) {
    if (done) {
      std::move(done).Run(false, "Invalid country code.");
    }
    return;
  }
  exit_country_ = normalized;
  if (!IsRunning()) {
    // Applied via the torrc on the next launch.
    if (done) {
      std::move(done).Run(true, std::string());
    }
    return;
  }
  // SETCONF/RESETCONF change only the running config (we never SAVECONF),
  // then NEWNYM moves new streams onto circuits through the new exit.
  std::vector<std::string> commands;
  if (normalized.empty()) {
    commands.push_back("RESETCONF ExitNodes StrictNodes");
  } else {
    commands.push_back("SETCONF ExitNodes=\"{" + normalized +
                       "}\" StrictNodes=1");
  }
  commands.push_back("SIGNAL NEWNYM");
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&RunCommandsBlocking, GetControlEndpoint(),
                     std::move(commands)),
      base::BindOnce(&RunDone, std::move(done)));
}

std::vector<std::string> TorManager::GetAvailableExitCountries() const {
  // Curated list of commonly-used, generally well-connected Tor exit
  // countries (lowercase ISO 3166-1 alpha-2) for the picker UI.
  return {
      "us", "gb", "de", "fr", "nl", "ch", "se", "no", "fi", "ca",
      "jp", "sg", "au", "es", "it", "at", "pl", "cz", "ro", "is",
  };
}

void TorManager::GetBridgeSettings(
    base::OnceCallback<void(BridgeSettings)> callback) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&ReadBridgeSettingsBlocking,
                     GetDataDir().AppendASCII(kBridgesFileName)),
      std::move(callback));
}

void TorManager::GetBridgeSupport(
    base::OnceCallback<void(BridgeSupport)> callback) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&ComputeBridgeSupportBlocking, GetBundledTorDir()),
      std::move(callback));
}

void TorManager::SetBridgeSettings(const BridgeSettings& settings,
                                   DoneCallback done) {
  // Validate up front so the user gets an immediate, line-specific error and
  // nothing invalid is ever persisted.
  if (settings.mode == BridgeSettings::Mode::kCustom) {
    std::string error;
    std::optional<std::vector<std::string>> lines =
        ParseBridgeLines(settings.custom_lines, &error);
    if (!lines) {
      std::move(done).Run(false, error);
      return;
    }
    if (lines->empty()) {
      std::move(done).Run(false, "Paste at least one bridge line.");
      return;
    }
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&WriteBridgeSettingsBlocking, GetDataDir(), settings),
      base::BindOnce(
          [](DoneCallback done, bool ok) {
            std::move(done).Run(
                ok, ok ? std::string() : "Could not save bridge settings.");
          },
          std::move(done)));
}

}  // namespace tor
}  // namespace molt_ai
