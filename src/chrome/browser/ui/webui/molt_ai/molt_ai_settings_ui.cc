// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/webui/molt_ai/molt_ai_settings_ui.h"

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/memory/ref_counted_memory.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_message_handler.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "chrome/browser/bookmarks/bookmark_model_factory.h"
#include "chrome/browser/molt_ai/cloud/cloud_provider.h"
#include "chrome/browser/molt_ai/common/molt_blocking_scope.h"
#include "chrome/browser/molt_ai/import/browser_importer.h"
#include "chrome/browser/molt_ai/import/chrome_importer.h"
#include "chrome/browser/molt_ai/keys/molt_keys_store.h"
#include "chrome/browser/molt_ai/tor/tor_manager.h"
#include "chrome/browser/molt_ai/tor/tor_service.h"
#include "chrome/browser/password_manager/profile_password_store_factory.h"
#include "content/public/browser/storage_partition.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "components/bookmarks/browser/bookmark_model.h"
#include "components/bookmarks/browser/bookmark_node.h"
#include "components/password_manager/core/browser/password_form.h"
#include "components/password_manager/core/browser/password_store/password_store_interface.h"
#include "components/keyed_service/core/service_access_type.h"
#include "url/gurl.h"
#include "build/build_config.h"

namespace {

const char kSettingsFileName[] = "settings.json";

base::FilePath GetSettingsFilePath() {
  base::FilePath home_dir;
  base::PathService::Get(base::DIR_HOME, &home_dir);
  return home_dir.Append(base::FilePath::FromUTF8Unsafe(".moltbrowser"))
      .Append(base::FilePath::FromUTF8Unsafe(kSettingsFileName));
}

// Display names for the curated exit-country codes returned by
// TorManager::GetAvailableExitCountries() (lowercase ISO alpha-2). Kept
// in sync with the identical map in molt_ai_chat_handler_tor.cc so the
// settings picker and the chat side-panel picker show the same labels.
// Any code not found here renders as its uppercased ISO code.
std::string ExitCountryDisplayName(const std::string& cc) {
  static const auto* const kNames = new std::map<std::string, std::string>{
      {"us", "United States"}, {"gb", "United Kingdom"},
      {"de", "Germany"},       {"fr", "France"},
      {"nl", "Netherlands"},   {"ch", "Switzerland"},
      {"se", "Sweden"},        {"no", "Norway"},
      {"fi", "Finland"},       {"ca", "Canada"},
      {"jp", "Japan"},         {"sg", "Singapore"},
      {"au", "Australia"},     {"es", "Spain"},
      {"it", "Italy"},         {"at", "Austria"},
      {"pl", "Poland"},        {"cz", "Czechia"},
      {"ro", "Romania"},       {"is", "Iceland"},
  };
  auto it = kNames->find(cc);
  if (it != kNames->end())
    return it->second;
  return base::ToUpperASCII(cc);
}

// ---- Settings Handler ----
class MoltAISettingsHandler : public content::WebUIMessageHandler {
 public:
  MoltAISettingsHandler() = default;
  ~MoltAISettingsHandler() override = default;

  void RegisterMessages() override {
    web_ui()->RegisterMessageCallback(
        "getSettings",
        base::BindRepeating(&MoltAISettingsHandler::HandleGetSettings,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "saveSettings",
        base::BindRepeating(&MoltAISettingsHandler::HandleSaveSettings,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "resetSettings",
        base::BindRepeating(&MoltAISettingsHandler::HandleResetSettings,
                            base::Unretained(this)));
    // MoltNet message handlers — these run the Tor daemon detection
    // and circuit management. Without these registered, the settings
    // page would crash on click (NOTREACHED in WebUIImpl::Send).
    web_ui()->RegisterMessageCallback(
        "moltnetConnect",
        base::BindRepeating(&MoltAISettingsHandler::HandleMoltnetConnect,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "moltnetDisconnect",
        base::BindRepeating(&MoltAISettingsHandler::HandleMoltnetDisconnect,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "moltnetNewCircuit",
        base::BindRepeating(&MoltAISettingsHandler::HandleMoltnetNewCircuit,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "moltnetSetExitCountry",
        base::BindRepeating(&MoltAISettingsHandler::HandleMoltnetSetExitCountry,
                            base::Unretained(this)));
    // Populate the exit-country picker from the real backend list
    // (TorManager::GetAvailableExitCountries) — mirrors the chat panel's
    // getTorExitCountries handler.
    web_ui()->RegisterMessageCallback(
        "getMoltnetExitCountries",
        base::BindRepeating(
            &MoltAISettingsHandler::HandleGetMoltnetExitCountries,
            base::Unretained(this)));
    // Import & Migration — drives molt_ai::BrowserImporter to detect
    // installed browsers, read a chosen profile, and write its
    // bookmarks/passwords into this profile.
    //
    // getImportableBrowsers: stat-only detection of installed browsers.
    web_ui()->RegisterMessageCallback(
        "getImportableBrowsers",
        base::BindRepeating(
            &MoltAISettingsHandler::HandleGetImportableBrowsers,
            base::Unretained(this)));
    // importFromBrowser: read+write one detected browser's profile.
    web_ui()->RegisterMessageCallback(
        "importFromBrowser",
        base::BindRepeating(&MoltAISettingsHandler::HandleImportFromBrowser,
                            base::Unretained(this)));
    // importFromChrome: legacy message kept working during the migration —
    // it delegates to importFromBrowser("chrome", include_passwords).
    web_ui()->RegisterMessageCallback(
        "importFromChrome",
        base::BindRepeating(&MoltAISettingsHandler::HandleImportFromChrome,
                            base::Unretained(this)));
    // Connect AI Providers — cloud/frontier models via the user's own API
    // key. Keys live encrypted in molt_ai::MoltKeysStore (never returned to
    // JS); the model list is fetched/validated live via
    // molt_ai::FetchCloudModels.
    //
    // getProviders: list every known provider + which are connected +
    // enabled models.
    web_ui()->RegisterMessageCallback(
        "getProviders",
        base::BindRepeating(&MoltAISettingsHandler::HandleGetProviders,
                            base::Unretained(this)));
    // saveProviderKey(cb, provider, key, base_url): validate the key by
    // listing models, then store it and return the model list.
    web_ui()->RegisterMessageCallback(
        "saveProviderKey",
        base::BindRepeating(&MoltAISettingsHandler::HandleSaveProviderKey,
                            base::Unretained(this)));
    // setModelEnabled(cb, provider, model, enabled): toggle one model in a
    // connected provider's enabled set.
    web_ui()->RegisterMessageCallback(
        "setModelEnabled",
        base::BindRepeating(&MoltAISettingsHandler::HandleSetModelEnabled,
                            base::Unretained(this)));
    // removeProvider(cb, provider): forget a provider's key + config.
    web_ui()->RegisterMessageCallback(
        "removeProvider",
        base::BindRepeating(&MoltAISettingsHandler::HandleRemoveProvider,
                            base::Unretained(this)));
  }

 private:
  base::DictValue GetDefaultSettings() {
    base::DictValue defaults;
    defaults.Set("max_tokens", 512);
    defaults.Set("temperature", 0.7);
    defaults.Set("top_p", 0.9);
    defaults.Set("top_k", 40);
    defaults.Set("max_history_messages", 16);
    defaults.Set("max_page_content_chars", 4000);
    defaults.Set("auto_load_model", true);
    defaults.Set("default_model", "tinyllama-1.1b");
    defaults.Set("system_prompt",
        "You are MoltBrowser AI, a helpful local AI assistant built into "
        "the MoltBrowser web browser. You run entirely on the user's "
        "device for privacy. Be concise, accurate, and helpful. Format "
        "your responses with markdown when appropriate.");
    return defaults;
  }

  base::DictValue LoadSettings() {
    ScopedAllowBlockingForMolt allow_blocking;
    base::FilePath path = GetSettingsFilePath();
    std::string contents;
    if (base::ReadFileToString(path, &contents)) {
      auto parsed = base::JSONReader::Read(
          contents, base::JSON_ALLOW_TRAILING_COMMAS);
      if (parsed && parsed->is_dict()) {
        // Return loaded settings as DictValue
        base::DictValue loaded;
        for (const auto [key, value] : parsed->GetDict()) {
          loaded.Set(key, value.Clone());
        }
        return loaded;
      }
    }
    return GetDefaultSettings();
  }

  bool SaveSettings(const base::DictValue& settings) {
    ScopedAllowBlockingForMolt allow_blocking;
    base::FilePath path = GetSettingsFilePath();
    base::FilePath dir = path.DirName();
    if (!base::DirectoryExists(dir)) {
      base::CreateDirectory(dir);
    }
    std::string json;
    base::JSONWriter::WriteWithOptions(
        settings, base::JSONWriter::OPTIONS_PRETTY_PRINT, &json);
    return base::WriteFile(path, json);
  }

  void HandleGetSettings(const base::ListValue& args) {
    AllowJavascript();
    CHECK_GE(args.size(), 1u);
    const std::string callback_id = args[0].GetString();

    base::DictValue settings = LoadSettings();
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(settings)));
  }

  void HandleSaveSettings(const base::ListValue& args) {
    AllowJavascript();
    CHECK_GE(args.size(), 2u);
    const std::string callback_id = args[0].GetString();

    if (!args[1].is_dict()) {
      base::DictValue error_result;
      error_result.Set("success", false);
      error_result.Set("error", "Invalid settings format");
      ResolveJavascriptCallback(base::Value(callback_id),
                                base::Value(std::move(error_result)));
      return;
    }

    // Copy provided settings into a DictValue
    base::DictValue current;
    for (const auto [key, value] : args[1].GetDict()) {
      current.Set(key, value.Clone());
    }

    bool success = SaveSettings(current);
    LOG(INFO) << "[MoltAI] Settings saved: " << success;

    base::DictValue result;
    result.Set("success", success);
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
  }

  void HandleResetSettings(const base::ListValue& args) {
    AllowJavascript();
    CHECK_GE(args.size(), 1u);
    const std::string callback_id = args[0].GetString();

    base::DictValue defaults = GetDefaultSettings();
    SaveSettings(defaults);

    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(defaults)));
  }

  // ---- MoltNet (Tor privacy routing) ----
  //
  // These handlers drive the same real backend as the chat side-panel's
  // Tor controls (see molt_ai_chat_handler_tor.cc): molt_ai::tor::
  // TorManager for launch/stop/exit-country and molt_ai::tor::TorService
  // for live circuit/relay info over the control port. No placeholder
  // data — every value reported to the UI comes from the running Tor.
  //
  // The settings page is event-driven (it listens for "moltnet-status"
  // via cr.addWebUIListener and calls chrome.send fire-and-forget), so
  // each action ends by re-querying the real state and broadcasting it
  // with FireWebUIListener, rather than resolving a per-call promise.

  // True iff a tor binary is actually resolvable (bundled inside the
  // .app on macOS/Windows, or a system install). This is exactly how the
  // backend decides what it can launch — no per-platform hardcoding, no
  // "unsupported on Windows" special case.
  bool IsTorAvailable() const {
    return !molt_ai::tor::TorManager::Get()
                ->ResolveTorBinary()
                .value()
                .empty();
  }

  // Probe the live Tor and, if it's up, fetch the enriched circuit list,
  // then broadcast a "moltnet-status" event built entirely from real
  // data. |forced_status| lets callers show a transient state (e.g.
  // "connecting") immediately; pass "" to report whatever Probe finds.
  void RefreshMoltnetStatus(const std::string& forced_status = "") {
    AllowJavascript();
    if (!IsTorAvailable()) {
      EmitMoltnetStatus(
          "disconnected",
          "No tor binary available (bundled tor missing from this build).");
      return;
    }
    auto weak_this = weak_ptr_factory_.GetWeakPtr();
    std::string forced = forced_status;
    molt_ai::tor::TorService::Get()->Probe(base::BindOnce(
        [](base::WeakPtr<MoltAISettingsHandler> self, std::string forced,
           molt_ai::tor::TorStatus s) {
          if (!self)
            return;
          if (!s.running) {
            self->EmitMoltnetStatus("disconnected", s.error);
            return;
          }
          // Tor is up — pull the real circuits (with IP + country per
          // hop) and report them. The apparent_ip shown to the user is
          // the exit relay's IP from the first built general circuit.
          std::string status =
              forced.empty() ? std::string("connected") : forced;
          molt_ai::tor::TorService::Get()->GetCircuitsEnriched(
              base::BindOnce(
                  [](base::WeakPtr<MoltAISettingsHandler> self,
                     std::string status,
                     std::vector<molt_ai::tor::TorCircuit> circuits) {
                    if (!self)
                      return;
                    self->EmitMoltnetStatusFromCircuits(status, circuits);
                  },
                  self, status));
        },
        weak_this, forced));
  }

  // Build and fire the "moltnet-status" event from a real circuit list.
  // Picks the first BUILT general-purpose circuit as the active one; its
  // ordered hops (guard -> middle -> exit) become the relay chain and
  // its exit hop's IP becomes the apparent IP.
  void EmitMoltnetStatusFromCircuits(
      const std::string& status,
      const std::vector<molt_ai::tor::TorCircuit>& circuits) {
    const molt_ai::tor::TorCircuit* active = nullptr;
    for (const auto& c : circuits) {
      if (c.state == "BUILT" && !c.hops.empty()) {
        active = &c;
        if (c.purpose == "GENERAL")
          break;  // prefer a general circuit; otherwise take any built one
      }
    }

    base::DictValue result;
    result.Set("status", status);
    base::ListValue relay_list;
    std::string apparent_ip;
    if (active) {
      for (const auto& h : active->hops) {
        base::DictValue relay;
        // The JS flag map keys on uppercase ISO codes; Tor's GeoIP can
        // return either case, so normalize here.
        relay.Set("country", base::ToUpperASCII(h.country));
        // Real relay identity (operator nickname, falling back to
        // fingerprint) instead of a synthesized "relayN" id.
        relay.Set("relay_id",
                  h.nickname.empty() ? h.fingerprint : h.nickname);
        relay.Set("fingerprint", h.fingerprint);
        relay.Set("ip", h.ip);
        relay_list.Append(std::move(relay));
      }
      apparent_ip = active->hops.back().ip;
    }
    result.Set("apparent_ip", apparent_ip);
    result.Set("relays", std::move(relay_list));
    FireWebUIListener("moltnet-status", base::Value(std::move(result)));
  }

  // Simple status broadcast with no circuit data (disconnected /
  // connecting / error states). |detail| is shown next to the badge.
  void EmitMoltnetStatus(const std::string& status,
                         const std::string& detail = "") {
    base::DictValue result;
    result.Set("status", status);
    result.Set("apparent_ip", detail);
    result.Set("relays", base::ListValue());
    FireWebUIListener("moltnet-status", base::Value(std::move(result)));
  }

  void HandleMoltnetConnect(const base::ListValue& args) {
    AllowJavascript();
    std::string mode = "multi_hop";
    if (args.size() > 0 && args[0].is_string()) {
      mode = args[0].GetString();
    }

    // "direct" means no privacy routing — stop any managed Tor instead
    // of launching one.
    if (mode == "direct") {
      molt_ai::tor::TorManager::Get()->Stop();
      EmitMoltnetStatus("disconnected");
      return;
    }

    if (!IsTorAvailable()) {
      EmitMoltnetStatus(
          "disconnected",
          "No tor binary available (bundled tor missing from this build).");
      return;
    }

    // Show "connecting" immediately, then actually launch Tor. Launch()
    // resolves only after the control port answers (bootstrap can take
    // ~15-30s), at which point we report the real running state +
    // circuits.
    EmitMoltnetStatus("connecting");
    auto weak_this = weak_ptr_factory_.GetWeakPtr();
    molt_ai::tor::TorManager::Get()->Launch(base::BindOnce(
        [](base::WeakPtr<MoltAISettingsHandler> self,
           molt_ai::tor::TorLaunchResult r) {
          if (!self)
            return;
          if (!r.success) {
            self->EmitMoltnetStatus(
                "disconnected",
                r.error.empty() ? std::string("Failed to launch Tor")
                                : r.error);
            return;
          }
          self->RefreshMoltnetStatus();
        },
        weak_this));
  }

  void HandleMoltnetDisconnect(const base::ListValue& args) {
    AllowJavascript();
    molt_ai::tor::TorManager::Get()->Stop();
    EmitMoltnetStatus("disconnected");
  }

  void HandleMoltnetNewCircuit(const base::ListValue& args) {
    AllowJavascript();
    molt_ai::tor::TorManager* mgr = molt_ai::tor::TorManager::Get();
    if (!mgr->IsRunning()) {
      EmitMoltnetStatus("disconnected");
      return;
    }
    // There is no standalone NEWNYM entry point in the public API; the
    // backend rebuilds circuits (SIGNAL RELOAD + SIGNAL NEWNYM) as part
    // of SetExitCountry(). Re-applying the *current* exit country is the
    // supported way to force a fresh circuit without changing the exit.
    mgr->SetExitCountry(mgr->GetExitCountry());
    RefreshMoltnetStatus();
  }

  void HandleMoltnetSetExitCountry(const base::ListValue& args) {
    AllowJavascript();
    std::string country;
    if (args.size() > 0 && args[0].is_string()) {
      country = args[0].GetString();
    }
    // TorManager validates/normalizes, but lowercase here too so we match
    // the chat handler's contract exactly.
    molt_ai::tor::TorManager::Get()->SetExitCountry(
        base::ToLowerASCII(country));
    RefreshMoltnetStatus();
  }

  // Mirrors the chat panel's getTorExitCountries: returns the real
  // curated list (lowercase ISO codes) + the currently-selected exit,
  // each with a display name, so the picker is data-driven.
  void HandleGetMoltnetExitCountries(const base::ListValue& args) {
    AllowJavascript();
    CHECK_GE(args.size(), 1u);
    const std::string callback_id = args[0].GetString();

    molt_ai::tor::TorManager* mgr = molt_ai::tor::TorManager::Get();
    base::DictValue out;
    out.Set("selected", mgr->GetExitCountry());
    base::ListValue available;
    for (const std::string& cc : mgr->GetAvailableExitCountries()) {
      base::DictValue entry;
      entry.Set("code", cc);
      entry.Set("name", ExitCountryDisplayName(cc));
      available.Append(std::move(entry));
    }
    out.Set("available", std::move(available));
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
  }

  // ---- Import & Migration (multi-browser) ----
  //
  // Detects installed browsers and, on request, reads a chosen browser's
  // profile off the main thread via molt_ai::BrowserImporter (Agent B/A own
  // the actual detection / decryption / platform gating; passwords are
  // reported per-source via passwords_supported), then writes the results
  // into this profile's BookmarkModel and PasswordStore on the UI thread.
  // Progress between the bookmark and password phases is streamed via the
  // "import-progress" WebUI listener.

  // Map a stable lowercase string id (used by the WebUI) to a BrowserId, and
  // back. Keeping the mapping local means the settings page and the importer
  // agree on exactly one wire format for the id.
  static std::string BrowserIdToString(molt_ai::BrowserId id) {
    switch (id) {
      case molt_ai::BrowserId::kChrome:
        return "chrome";
      case molt_ai::BrowserId::kChromium:
        return "chromium";
      case molt_ai::BrowserId::kEdge:
        return "edge";
      case molt_ai::BrowserId::kBrave:
        return "brave";
      case molt_ai::BrowserId::kOpera:
        return "opera";
      case molt_ai::BrowserId::kVivaldi:
        return "vivaldi";
      case molt_ai::BrowserId::kFirefox:
        return "firefox";
      case molt_ai::BrowserId::kSafari:
        return "safari";
    }
    return "chrome";
  }

  // Returns false (and leaves |out| untouched) for an unknown id string.
  static bool StringToBrowserId(const std::string& s, molt_ai::BrowserId* out) {
    if (s == "chrome") {
      *out = molt_ai::BrowserId::kChrome;
    } else if (s == "chromium") {
      *out = molt_ai::BrowserId::kChromium;
    } else if (s == "edge") {
      *out = molt_ai::BrowserId::kEdge;
    } else if (s == "brave") {
      *out = molt_ai::BrowserId::kBrave;
    } else if (s == "opera") {
      *out = molt_ai::BrowserId::kOpera;
    } else if (s == "vivaldi") {
      *out = molt_ai::BrowserId::kVivaldi;
    } else if (s == "firefox") {
      *out = molt_ai::BrowserId::kFirefox;
    } else if (s == "safari") {
      *out = molt_ai::BrowserId::kSafari;
    } else {
      return false;
    }
    return true;
  }

  // getImportableBrowsers -> {browsers:[{id,display_name,installed,
  // has_bookmarks,has_passwords_store}]}. Detection is stat-only (no keychain,
  // no decryption) so it is cheap enough to run inline on the UI thread. Every
  // supported browser is returned; `installed` is false for ones not present
  // here so the UI can render them greyed/disabled.
  void HandleGetImportableBrowsers(const base::ListValue& args) {
    AllowJavascript();
    CHECK_GE(args.size(), 1u);
    const std::string callback_id = args[0].GetString();

    base::DictValue out;
    base::ListValue browsers;
    for (const molt_ai::DetectedBrowser& b :
         molt_ai::BrowserImporter::DetectInstalled()) {
      base::DictValue entry;
      entry.Set("id", BrowserIdToString(b.id));
      entry.Set("display_name", b.display_name);
      entry.Set("installed", b.installed);
      entry.Set("has_bookmarks", b.has_bookmarks);
      entry.Set("has_passwords_store", b.has_passwords_store);
      browsers.Append(std::move(entry));
    }
    out.Set("browsers", std::move(browsers));
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
  }

  // importFromBrowser(callback_id, browser_id_string, include_passwords):
  // reads the chosen browser's profile off-thread, then writes it in.
  void HandleImportFromBrowser(const base::ListValue& args) {
    AllowJavascript();
    CHECK_GE(args.size(), 3u);
    const std::string callback_id = args[0].GetString();
    const std::string browser_id_string =
        args[1].is_string() ? args[1].GetString() : std::string();
    const bool include_passwords = args[2].is_bool() && args[2].GetBool();

    molt_ai::BrowserId browser_id;
    if (!StringToBrowserId(browser_id_string, &browser_id)) {
      base::DictValue result;
      result.Set("success", false);
      result.Set("browser_id", browser_id_string);
      result.Set("error", "Unknown browser");
      ResolveJavascriptCallback(base::Value(callback_id),
                                base::Value(std::move(result)));
      return;
    }

    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE,
        {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
        base::BindOnce(&molt_ai::BrowserImporter::ReadProfile, browser_id,
                       include_passwords),
        base::BindOnce(&MoltAISettingsHandler::OnBrowserProfileRead,
                       weak_ptr_factory_.GetWeakPtr(), callback_id,
                       include_passwords));
  }

  // Legacy importFromChrome(callback_id, include_passwords) — delegate to the
  // generalized path so nothing breaks mid-migration.
  void HandleImportFromChrome(const base::ListValue& args) {
    AllowJavascript();
    CHECK_GE(args.size(), 2u);
    const std::string callback_id = args[0].GetString();
    const bool include_passwords = args[1].is_bool() && args[1].GetBool();

    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE,
        {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
        base::BindOnce(&molt_ai::BrowserImporter::ReadProfile,
                       molt_ai::BrowserId::kChrome, include_passwords),
        base::BindOnce(&MoltAISettingsHandler::OnBrowserProfileRead,
                       weak_ptr_factory_.GetWeakPtr(), callback_id,
                       include_passwords));
  }

  void OnBrowserProfileRead(const std::string& callback_id,
                            bool include_passwords,
                            molt_ai::BrowserImportData data) {
    // The handler could have outlived its WebContents; AllowJavascript()
    // was already called in the Handle* entrypoint, but re-guard against a
    // torn-down page.
    if (!IsJavascriptAllowed())
      return;

    const std::string browser_id = BrowserIdToString(data.source_id);

    if (!data.profile_found) {
      base::DictValue result;
      result.Set("success", false);
      result.Set("browser_id", browser_id);
      result.Set("needs_full_disk_access", data.needs_full_disk_access);
      result.Set(
          "error",
          data.error.empty()
              ? ("No " + data.display_name + " profile found")
              : data.error);
      ResolveJavascriptCallback(base::Value(callback_id),
                                base::Value(std::move(result)));
      return;
    }

    Profile* profile = Profile::FromWebUI(web_ui());

    // ---- Phase 1: bookmarks ----
    FireImportProgress("bookmarks", 0,
                       static_cast<int>(data.bookmarks.size()));
    int bookmarks_imported =
        ImportBookmarks(profile, data.display_name, data.bookmarks);
    FireImportProgress("bookmarks", bookmarks_imported,
                       static_cast<int>(data.bookmarks.size()));

    // ---- Phase 2: passwords ----
    int passwords_imported = 0;
    int password_failures = 0;
    if (include_passwords && data.passwords_supported &&
        !data.keychain_denied) {
      FireImportProgress("passwords", 0,
                         static_cast<int>(data.credentials.size()));
      ImportPasswords(profile, data.credentials, &passwords_imported,
                      &password_failures);
      FireImportProgress("passwords", passwords_imported,
                         static_cast<int>(data.credentials.size()));
    }

    base::DictValue result;
    result.Set("success", true);
    result.Set("browser_id", browser_id);
    result.Set("bookmarks_imported", bookmarks_imported);
    result.Set("passwords_imported", passwords_imported);
    result.Set("password_failures", password_failures);
    result.Set("keychain_denied", data.keychain_denied);
    result.Set("passwords_supported", data.passwords_supported);
    result.Set("needs_full_disk_access", data.needs_full_disk_access);
    if (!data.error.empty())
      result.Set("error", data.error);
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
  }

  // Find-or-create the nested folder chain described by |folder_path| (e.g.
  // "Bookmarks Bar/Tech/News") under |root|, creating any missing folders, and
  // return the leaf folder. Empty/whitespace-only segments are skipped; an
  // empty path returns |root| itself.
  const bookmarks::BookmarkNode* GetOrCreateFolderPath(
      bookmarks::BookmarkModel* model,
      const bookmarks::BookmarkNode* root,
      const std::string& folder_path) {
    const bookmarks::BookmarkNode* current = root;
    for (const std::string& segment :
         base::SplitString(folder_path, "/", base::TRIM_WHITESPACE,
                           base::SPLIT_WANT_NONEMPTY)) {
      const std::u16string seg16 = base::UTF8ToUTF16(segment);
      const bookmarks::BookmarkNode* next = nullptr;
      for (const auto& child : current->children()) {
        if (child->is_folder() && child->GetTitle() == seg16) {
          next = child.get();
          break;
        }
      }
      if (!next) {
        next = model->AddFolder(current, current->children().size(), seg16);
      }
      current = next;
    }
    return current;
  }

  // Creates (or reuses) a top-level "Imported from <Browser>" folder under the
  // bookmark bar and RE-CREATES the source browser's nested folder hierarchy
  // inside it, placing each bookmark in the same folder it had in the source
  // browser (from bm.folder_path, e.g. "Bookmarks Bar/Tech"). Folders are
  // found-or-created so bookmarks sharing a path group together, and repeat
  // imports reuse existing folders instead of duplicating them.
  int ImportBookmarks(
      Profile* profile,
      const std::string& source_display_name,
      const std::vector<molt_ai::BrowserBookmark>& bookmarks) {
    bookmarks::BookmarkModel* model =
        BookmarkModelFactory::GetForBrowserContext(profile);
    if (!model || !model->loaded())
      return 0;

    const bookmarks::BookmarkNode* bar = model->bookmark_bar_node();
    const std::u16string folder_title =
        u"Imported from " + base::UTF8ToUTF16(source_display_name);

    // Reuse an existing "Imported from <Browser>" folder if the user has
    // imported from this source before, so repeated imports don't stack up
    // duplicate top-level folders.
    const bookmarks::BookmarkNode* root_folder = nullptr;
    for (const auto& child : bar->children()) {
      if (child->is_folder() && child->GetTitle() == folder_title) {
        root_folder = child.get();
        break;
      }
    }
    if (!root_folder)
      root_folder = model->AddFolder(bar, bar->children().size(), folder_title);

    int imported = 0;
    for (const auto& bm : bookmarks) {
      GURL url(bm.url);
      if (!url.is_valid())
        continue;
      const bookmarks::BookmarkNode* target =
          GetOrCreateFolderPath(model, root_folder, bm.folder_path);
      model->AddURL(target, target->children().size(), bm.title, url);
      ++imported;
    }
    return imported;
  }

  void ImportPasswords(
      Profile* profile,
      const std::vector<molt_ai::BrowserCredential>& credentials,
      int* imported,
      int* failures) {
    scoped_refptr<password_manager::PasswordStoreInterface> store =
        ProfilePasswordStoreFactory::GetForProfile(
            profile, ServiceAccessType::EXPLICIT_ACCESS);
    if (!store) {
      *failures = static_cast<int>(credentials.size());
      return;
    }

    for (const auto& cred : credentials) {
      GURL url(cred.url);
      if (!url.is_valid() || cred.signon_realm.empty()) {
        ++(*failures);
        continue;
      }
      password_manager::PasswordForm form;
      form.signon_realm = cred.signon_realm;
      form.url = url;
      form.username_value = cred.username;
      form.password_value = cred.password;
      // AddLogin is asynchronous (writes on the store's own sequence); we
      // count it as accepted for import here. The store dedupes on its
      // primary key, so re-importing existing logins is a no-op there.
      store->AddLogin(form);
      ++(*imported);
    }
  }

  void FireImportProgress(const std::string& phase, int done, int total) {
    base::DictValue progress;
    progress.Set("phase", phase);
    progress.Set("done", done);
    progress.Set("total", total);
    FireWebUIListener("import-progress", base::Value(std::move(progress)));
  }

  // ---- Connect AI Providers (cloud / frontier API models) -----------
  //
  // API keys live encrypted in molt_ai::MoltKeysStore (keys.enc, OSCrypt).
  // They are NEVER returned to JS and NEVER logged — getProviders reports
  // only whether a key is stored, plus the non-secret base_url + enabled
  // model list. keys.enc reads/writes are tiny OSCrypt operations wrapped
  // in ScopedAllowBlockingForMolt, exactly like the password vault.

  // getProviders(callback_id) → { providers: [ {id, display_name,
  //   default_base_url, custom_base_url, connected, base_url,
  //   enabled_models:[...] } ] }
  void HandleGetProviders(const base::ListValue& args) {
    AllowJavascript();
    CHECK_GE(args.size(), 1u);
    const std::string callback_id = args[0].GetString();

    std::map<std::string, molt_ai::ProviderConfig> stored;
    {
      ScopedAllowBlockingForMolt allow;
      stored = molt_ai::MoltKeysStore::LoadAll();
    }

    base::ListValue providers;
    for (const molt_ai::CloudProviderInfo& info :
         molt_ai::GetCloudProviders()) {
      base::DictValue p;
      p.Set("id", info.id);
      p.Set("display_name", info.display_name);
      p.Set("default_base_url", info.default_base_url);
      p.Set("custom_base_url", info.custom_base_url);
      auto it = stored.find(info.id);
      const bool connected =
          it != stored.end() && !it->second.api_key.empty();
      p.Set("connected", connected);
      // Non-secret fields only — the api_key itself never leaves C++.
      p.Set("base_url", connected ? it->second.base_url : std::string());
      base::ListValue enabled;
      if (connected) {
        for (const std::string& m : it->second.enabled_models) {
          enabled.Append(m);
        }
      }
      p.Set("enabled_models", std::move(enabled));
      providers.Append(std::move(p));
    }

    base::DictValue result;
    result.Set("providers", std::move(providers));
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
  }

  // saveProviderKey(callback_id, provider, api_key, base_url?) — validate
  // the key by listing models over the network, then store it and return
  // the model list. On any validation failure nothing is persisted.
  void HandleSaveProviderKey(const base::ListValue& args) {
    AllowJavascript();
    CHECK_GE(args.size(), 3u);
    const std::string callback_id = args[0].GetString();
    const std::string provider_id =
        args[1].is_string() ? args[1].GetString() : std::string();
    const std::string api_key =
        args[2].is_string() ? args[2].GetString() : std::string();
    const std::string base_url =
        (args.size() >= 4u && args[3].is_string()) ? args[3].GetString()
                                                   : std::string();

    if (provider_id.empty() || api_key.empty()) {
      base::DictValue result;
      result.Set("success", false);
      result.Set("error", "Provider and API key are required");
      ResolveJavascriptCallback(base::Value(callback_id),
                                base::Value(std::move(result)));
      return;
    }

    Profile* profile = Profile::FromWebUI(web_ui());
    scoped_refptr<network::SharedURLLoaderFactory> factory =
        profile ? profile->GetDefaultStoragePartition()
                      ->GetURLLoaderFactoryForBrowserProcess()
                : nullptr;
    if (!factory) {
      base::DictValue result;
      result.Set("success", false);
      result.Set("error", "No network context available");
      ResolveJavascriptCallback(base::Value(callback_id),
                                base::Value(std::move(result)));
      return;
    }

    molt_ai::FetchCloudModels(
        std::move(factory), provider_id, api_key, base_url,
        base::BindOnce(&MoltAISettingsHandler::OnProviderKeyValidated,
                       weak_ptr_factory_.GetWeakPtr(), callback_id,
                       provider_id, api_key, base_url));
  }

  void OnProviderKeyValidated(const std::string& callback_id,
                              const std::string& provider_id,
                              const std::string& api_key,
                              const std::string& base_url,
                              bool ok,
                              std::vector<std::string> models,
                              std::string error) {
    if (!IsJavascriptAllowed())
      return;

    base::DictValue result;
    if (!ok) {
      result.Set("success", false);
      result.Set("error",
                 error.empty() ? "Could not validate API key" : error);
      ResolveJavascriptCallback(base::Value(callback_id),
                                base::Value(std::move(result)));
      return;
    }

    // Persist the (encrypted) key. Preserve any previously-enabled models
    // so re-entering a key to refresh the list keeps the user's picks.
    molt_ai::ProviderConfig cfg;
    cfg.api_key = api_key;
    cfg.base_url = base_url;
    {
      ScopedAllowBlockingForMolt allow;
      std::optional<molt_ai::ProviderConfig> existing =
          molt_ai::MoltKeysStore::Get(provider_id);
      if (existing) {
        cfg.enabled_models = existing->enabled_models;
      }
      molt_ai::MoltKeysStore::Save(provider_id, cfg);
    }

    result.Set("success", true);
    base::ListValue model_list;
    for (const std::string& m : models) {
      model_list.Append(m);
    }
    result.Set("models", std::move(model_list));
    base::ListValue enabled;
    for (const std::string& m : cfg.enabled_models) {
      enabled.Append(m);
    }
    result.Set("enabled_models", std::move(enabled));
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
  }

  // setModelEnabled(callback_id, provider, model, enabled) — toggle one
  // model in a connected provider's enabled set.
  void HandleSetModelEnabled(const base::ListValue& args) {
    AllowJavascript();
    CHECK_GE(args.size(), 4u);
    const std::string callback_id = args[0].GetString();
    const std::string provider_id =
        args[1].is_string() ? args[1].GetString() : std::string();
    const std::string model =
        args[2].is_string() ? args[2].GetString() : std::string();
    const bool enabled = args[3].is_bool() && args[3].GetBool();

    base::DictValue result;
    {
      ScopedAllowBlockingForMolt allow;
      std::optional<molt_ai::ProviderConfig> cfg =
          molt_ai::MoltKeysStore::Get(provider_id);
      if (!cfg || cfg->api_key.empty()) {
        result.Set("success", false);
        result.Set("error", "Provider not connected");
        ResolveJavascriptCallback(base::Value(callback_id),
                                  base::Value(std::move(result)));
        return;
      }
      auto& list = cfg->enabled_models;
      auto it = std::find(list.begin(), list.end(), model);
      if (enabled && it == list.end()) {
        list.push_back(model);
      } else if (!enabled && it != list.end()) {
        list.erase(it);
      }
      molt_ai::MoltKeysStore::Save(provider_id, *cfg);
      result.Set("success", true);
      base::ListValue enabled_list;
      for (const std::string& m : list) {
        enabled_list.Append(m);
      }
      result.Set("enabled_models", std::move(enabled_list));
    }
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
  }

  // removeProvider(callback_id, provider) — forget a provider's key +
  // config entirely.
  void HandleRemoveProvider(const base::ListValue& args) {
    AllowJavascript();
    CHECK_GE(args.size(), 2u);
    const std::string callback_id = args[0].GetString();
    const std::string provider_id =
        args[1].is_string() ? args[1].GetString() : std::string();
    {
      ScopedAllowBlockingForMolt allow;
      molt_ai::MoltKeysStore::Remove(provider_id);
    }
    base::DictValue result;
    result.Set("success", true);
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
  }

 private:
  base::WeakPtrFactory<MoltAISettingsHandler> weak_ptr_factory_{this};
};

// ---- Data Source ----
class MoltAISettingsDataSource : public content::URLDataSource {
 public:
  MoltAISettingsDataSource() = default;
  ~MoltAISettingsDataSource() override = default;

  std::string GetSource() override {
    return chrome::kChromeUIMoltAISettingsHost;
  }

  std::string GetMimeType(const GURL& url) override {
    return "text/html";
  }

  std::string GetContentSecurityPolicy(
      const network::mojom::CSPDirectiveName directive) override {
    if (directive == network::mojom::CSPDirectiveName::ScriptSrc) {
      return "script-src chrome://resources 'self' 'unsafe-inline';";
    }
    if (directive == network::mojom::CSPDirectiveName::StyleSrc) {
      return "style-src 'self' 'unsafe-inline';";
    }
    if (directive == network::mojom::CSPDirectiveName::TrustedTypes) {
      return "";
    }
    if (directive ==
        network::mojom::CSPDirectiveName::RequireTrustedTypesFor) {
      return "";
    }
    return content::URLDataSource::GetContentSecurityPolicy(directive);
  }

  void StartDataRequest(
      const GURL& url,
      const content::WebContents::Getter& wc_getter,
      content::URLDataSource::GotDataCallback callback) override {
    const std::string html = R"HTML(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>MoltBrowser AI Settings</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#0a0a0a;color:#e0e0e0;min-height:100vh;padding:40px 20px;position:relative;overflow-x:hidden}
/* Liquid Glass ambient ground: fixed, non-interactive, sits BEHIND all content
   so the translucent glass panels have colored light to refract. Three large,
   heavily-blurred radial orbs (MoltBrowser accent violet, a red, a cool teal). */
.lg-ambient{position:fixed;inset:0;z-index:0;pointer-events:none;overflow:hidden;
  background:
    radial-gradient(42vmax 42vmax at 12% 8%, rgba(99,102,241,0.30), transparent 60%),
    radial-gradient(38vmax 38vmax at 88% 18%, rgba(229,72,77,0.28), transparent 62%),
    radial-gradient(46vmax 46vmax at 72% 96%, rgba(43,182,196,0.26), transparent 60%),
    radial-gradient(34vmax 34vmax at 24% 88%, rgba(167,139,250,0.24), transparent 60%);
  filter:blur(80px);
  animation:lgDrift 34s ease-in-out infinite alternate}
@keyframes lgDrift{from{transform:translate3d(-2%,-1%,0) scale(1.02)}to{transform:translate3d(2%,2%,0) scale(1.08)}}
.container{max-width:640px;margin:0 auto;position:relative;z-index:1}
.logo-area{display:flex;align-items:center;gap:10px;margin-bottom:4px}.logo-img{width:44px;height:44px;border-radius:12px;box-shadow:0 2px 10px rgba(0,0,0,0.5)}.logo-text{font-size:26px;font-weight:700;background:linear-gradient(135deg,#6366f1,#a855f7);-webkit-background-clip:text;-webkit-text-fill-color:transparent}
.back-btn{display:none;align-items:center;justify-content:center;width:30px;height:30px;border:1px solid #333;border-radius:8px;color:#6366f1;text-decoration:none;font-size:16px;line-height:1;cursor:pointer;transition:all 0.2s;flex-shrink:0}
.back-btn:hover{border-color:#6366f1;background:#111}
.subtitle{color:#888;margin-bottom:24px;font-size:14px}
.nav{display:flex;gap:12px;margin-bottom:24px}
.nav a{color:#6366f1;text-decoration:none;font-size:13px;padding:6px 12px;border:1px solid #333;border-radius:8px;transition:all 0.2s}
.nav a:hover{border-color:#6366f1;background:#111}
.section{background:rgba(255,255,255,0.085);-webkit-backdrop-filter:blur(26px) saturate(1.7);backdrop-filter:blur(26px) saturate(1.7);border:1px solid rgba(255,255,255,0.12);border-radius:18px;padding:20px;margin-bottom:16px;box-shadow:0 20px 50px -18px rgba(0,0,0,0.6),inset 0 1px 0 rgba(255,255,255,0.30);transition:border-color 0.2s,transform 0.2s}
.section:hover{transform:translateY(-1px);border-color:rgba(255,255,255,0.20)}
.section h2{font-size:16px;font-weight:600;margin-bottom:16px;color:#e0e0e0;display:flex;align-items:center;gap:8px}
.section h2 .icon{font-size:18px}
.field{margin-bottom:16px}
.field:last-child{margin-bottom:0}
.field label{display:block;font-size:13px;font-weight:600;color:#aaa;margin-bottom:6px}
.field .desc{font-size:11px;color:#666;margin-bottom:6px}
.field input[type="number"],.field input[type="text"],.field textarea{width:100%;padding:10px 14px;border-radius:10px;border:1px solid rgba(255,255,255,0.10);background:rgba(0,0,0,0.25);color:#e0e0e0;font-size:13px;outline:none;transition:border-color 0.2s;font-family:inherit}
.field input:focus,.field textarea:focus{border-color:#6366f1}
.field textarea{resize:vertical;min-height:80px}
.field select{padding:10px 14px;border-radius:10px;border:1px solid rgba(255,255,255,0.10);background:rgba(0,0,0,0.25);color:#e0e0e0;font-size:13px;outline:none;width:100%;cursor:pointer}
.field .range-wrap{display:flex;align-items:center;gap:12px}
.field input[type="range"]{flex:1;accent-color:#6366f1}
.field .range-val{font-size:13px;color:#6366f1;min-width:40px;text-align:right;font-weight:600}
.field label.toggle,.toggle{display:flex;align-items:center;gap:10px;cursor:pointer;font-weight:normal;color:inherit;margin-bottom:0}
.toggle input{display:none}
.toggle .track{flex:0 0 auto;width:40px;height:22px;border-radius:11px;background:#333;position:relative;transition:background 0.2s}
.toggle input:checked + .track{background:#6366f1}
.toggle .track::after{content:'';position:absolute;top:2px;left:2px;width:18px;height:18px;border-radius:50%;background:#e0e0e0;transition:transform 0.2s}
.toggle input:checked + .track::after{transform:translateX(18px)}
.toggle .label{font-size:13px;color:#ccc}
.toggle.disabled{opacity:0.5;cursor:not-allowed}
.browser-row.not-installed{opacity:0.45}
.browser-logo{flex:0 0 auto;width:22px;height:22px;display:inline-block;vertical-align:middle}
.not-installed-label{margin-left:auto;font-size:12px;color:#777;font-style:italic}
.actions{display:flex;gap:10px;margin-top:20px}
.btn{padding:10px 24px;border-radius:10px;border:none;font-size:14px;font-weight:600;cursor:pointer;transition:all 0.2s}
.btn.primary{background:linear-gradient(135deg,#6366f1,#8b5cf6);color:white;box-shadow:0 8px 22px -8px rgba(99,102,241,0.6),inset 0 1px 0 rgba(255,255,255,0.25)}
.btn.primary:hover{opacity:0.9;transform:translateY(-1px)}
.btn.secondary{background:rgba(255,255,255,0.085);-webkit-backdrop-filter:blur(20px) saturate(1.6);backdrop-filter:blur(20px) saturate(1.6);border:1px solid rgba(255,255,255,0.12);color:#cfcfe0;box-shadow:inset 0 1px 0 rgba(255,255,255,0.14)}
.btn.secondary:hover{border-color:#6366f1;color:#e0e0e0;transform:translateY(-1px)}
.btn.danger{background:transparent;border:1px solid #f87171;color:#f87171}
.btn.danger:hover{background:rgba(248,113,113,0.12)}
.toast{position:fixed;bottom:24px;right:24px;padding:12px 20px;border-radius:14px;background:rgba(26,46,26,0.55);-webkit-backdrop-filter:blur(26px) saturate(1.7);backdrop-filter:blur(26px) saturate(1.7);color:#7ef0a0;font-size:13px;font-weight:600;border:1px solid rgba(122,222,128,0.35);box-shadow:0 20px 50px -18px rgba(0,0,0,0.6),inset 0 1px 0 rgba(255,255,255,0.16);transform:translateY(100px);opacity:0;transition:all 0.3s;z-index:2}
.toast.show{transform:translateY(0);opacity:1}
.model-dir{font-family:monospace;font-size:12px;color:#888;padding:8px 12px;background:rgba(0,0,0,0.28);border-radius:8px;border:1px solid rgba(255,255,255,0.08);margin-top:6px}
.provider-row{border:1px solid rgba(255,255,255,0.10);border-radius:14px;padding:12px 14px;margin-bottom:10px;background:rgba(255,255,255,0.04);box-shadow:inset 0 1px 0 rgba(255,255,255,0.12);transition:border-color 0.2s,transform 0.2s}
.provider-row:hover{transform:translateY(-1px);border-color:rgba(255,255,255,0.18)}
.provider-head{display:flex;align-items:center;gap:10px}
.provider-name{font-size:13px;font-weight:600;color:#e0e0e0}
.provider-badge{font-size:11px;font-weight:600;color:#4ade80;background:#12240f;border:1px solid #234d1a;border-radius:10px;padding:2px 8px}
.provider-btn{margin-left:auto;font-size:12px;padding:6px 14px}
.provider-body{margin-top:10px}
.provider-form{display:flex;flex-direction:column;gap:8px}
.provider-form input{width:100%;padding:8px 12px;border-radius:10px;border:1px solid rgba(255,255,255,0.10);background:rgba(0,0,0,0.28);color:#e0e0e0;font-size:13px;outline:none;box-sizing:border-box}
.provider-form input:focus{border-color:#6366f1}
.provider-form .row{display:flex;gap:8px}
.provider-hint{font-size:11px;color:#666;line-height:1.5}
.provider-err{font-size:12px;color:#f87171}
.model-scroll{max-height:220px;overflow-y:auto;margin-top:6px}
.model-toggle{display:flex;align-items:center;gap:10px;padding:6px 2px;font-size:13px;color:#ccc;border-top:1px solid #191919;cursor:pointer}
.model-toggle:first-child{border-top:none}
.model-toggle input{accent-color:#6366f1;width:16px;height:16px;flex:0 0 auto}
.model-toggle .mt-name{font-family:monospace;font-size:12px;word-break:break-all}
.model-chips{display:flex;flex-wrap:wrap;gap:6px;margin-top:4px}
.model-chip{display:inline-flex;align-items:center;gap:6px;background:rgba(99,102,241,0.14);border:1px solid rgba(167,139,250,0.30);color:#c7c7f0;border-radius:12px;padding:3px 10px;font-size:12px;font-family:monospace}
.model-chip button{background:none;border:none;color:#a78bfa;cursor:pointer;font-size:14px;line-height:1;padding:0}
.model-chip button:hover{color:#f87171}
/* Motion is minimal by design; fully stop the ambient drift and lift when the
   user prefers reduced motion. */
@media (prefers-reduced-motion: reduce){
  .lg-ambient{animation:none}
  .section,.provider-row,.btn.primary,.btn.secondary{transition:none}
  .section:hover,.provider-row:hover,.btn.primary:hover,.btn.secondary:hover{transform:none}
}
</style>
</head>
<body>
<div class="lg-ambient" aria-hidden="true"></div>
<div class="container">
  <div class="logo-area"><a class="back-btn" id="backBtn" href="#" title="Back" onclick="history.back();return false">&#8592;</a><img class="logo-img" src="data:image/png;base64,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" alt="MoltBrowser"><div class="logo-text">AI Settings</div></div>
  <div class="subtitle">Configure MoltBrowser's local AI assistant</div>
  <div class="nav">
    <a href="molt://ai/">AI Chat</a>
    <a href="molt://ai-chat/">Side Panel</a>
  </div>

  <div class="section">
    <h2><span class="icon">&#9881;</span> Generation</h2>
    <div class="field">
      <label>Max Tokens</label>
      <div class="desc">Maximum number of tokens to generate per response (64-2048)</div>
      <div class="range-wrap">
        <input type="range" id="maxTokens" min="64" max="2048" step="64" value="512" oninput="document.getElementById('maxTokensVal').textContent=this.value">
        <span class="range-val" id="maxTokensVal">512</span>
      </div>
    </div>
    <div class="field">
      <label>Temperature</label>
      <div class="desc">Higher values make output more random (0.1-2.0)</div>
      <div class="range-wrap">
        <input type="range" id="temperature" min="0.1" max="2.0" step="0.1" value="0.7" oninput="document.getElementById('tempVal').textContent=parseFloat(this.value).toFixed(1)">
        <span class="range-val" id="tempVal">0.7</span>
      </div>
    </div>
    <div class="field">
      <label>Top P</label>
      <div class="desc">Nucleus sampling threshold (0.1-1.0)</div>
      <div class="range-wrap">
        <input type="range" id="topP" min="0.1" max="1.0" step="0.05" value="0.9" oninput="document.getElementById('topPVal').textContent=parseFloat(this.value).toFixed(2)">
        <span class="range-val" id="topPVal">0.90</span>
      </div>
    </div>
    <div class="field">
      <label>Top K</label>
      <div class="desc">Limit token selection to top K candidates (1-100)</div>
      <div class="range-wrap">
        <input type="range" id="topK" min="1" max="100" step="1" value="40" oninput="document.getElementById('topKVal').textContent=this.value">
        <span class="range-val" id="topKVal">40</span>
      </div>
    </div>
  </div>

  <div class="section">
    <h2><span class="icon">&#128172;</span> Conversation</h2>
    <div class="field">
      <label>Max History Messages</label>
      <div class="desc">Number of previous messages to include for context (4-32)</div>
      <div class="range-wrap">
        <input type="range" id="maxHistory" min="4" max="32" step="2" value="16" oninput="document.getElementById('histVal').textContent=this.value">
        <span class="range-val" id="histVal">16</span>
      </div>
    </div>
    <div class="field">
      <label>Max Page Content (chars)</label>
      <div class="desc">Maximum characters to extract from page for context (1000-8000)</div>
      <div class="range-wrap">
        <input type="range" id="maxPageContent" min="1000" max="8000" step="500" value="4000" oninput="document.getElementById('pageVal').textContent=this.value">
        <span class="range-val" id="pageVal">4000</span>
      </div>
    </div>
    <div class="field">
      <label>System Prompt</label>
      <div class="desc">Customize the AI's behavior and personality</div>
      <textarea id="systemPrompt" rows="4"></textarea>
    </div>
  </div>

  <div class="section">
    <h2><span class="icon">&#129302;</span> Model</h2>
    <div class="field">
      <label>Default Model</label>
      <select id="defaultModel">
        <option value="tinyllama-1.1b">TinyLlama 1.1B (Fastest)</option>
        <option value="phi-3.5-3b">Phi-3.5 3B (Balanced)</option>
        <option value="mistral-7b">Mistral 7B (Quality)</option>
        <option value="llama-3.1-8b">LLaMA 3.1 8B (Best)</option>
        <option value="qwen2.5-7b">Qwen2.5 7B</option>
        <option value="gemma-2-9b">Gemma 2 9B</option>
      </select>
    </div>
    <div class="field">
      <label class="toggle">
        <input type="checkbox" id="autoLoadModel" checked>
        <span class="track"></span>
        <span class="label">Auto-load model on first prompt</span>
      </label>
    </div>
    <div class="field">
      <label>Model Directory</label>
      <div class="model-dir">~/.moltbrowser/models/</div>
    </div>
  </div>

  <div class="section" id="providersSection">
    <h2><span class="icon">&#9729;</span> Connect AI Providers</h2>
    <div class="field">
      <div class="desc">Add frontier models &mdash; OpenAI, Anthropic, Google Gemini and more &mdash; with your own API key. Your key is validated, then encrypted on this device and used only to talk directly to the provider you pick. On-device local models stay fully private and need no key.</div>
    </div>
    <div id="providersList" class="field"></div>
  </div>

  <div class="section">
    <h2><span class="icon">&#128274;</span> Privacy Network (MoltNet)</h2>
    <div class="field">
      <label class="toggle">
        <input type="checkbox" id="moltnetEnabled" onchange="toggleMoltNet(this.checked)">
        <span class="track"></span>
        <span class="label">Enable MoltNet Privacy Routing</span>
      </label>
      <div class="desc">Route traffic through encrypted relay nodes to hide your IP address</div>
    </div>
    <div class="field">
      <label>Routing Mode</label>
      <select id="routingMode" onchange="setRoutingMode(this.value)">
        <option value="direct">Direct (No Privacy Routing)</option>
        <option value="proxy">Single Proxy</option>
        <option value="multi_hop" selected>Multi-Hop (Tor)</option>
      </select>
    </div>
    <div id="moltnetStatus" style="display:none">
      <div class="field">
        <label>Connection Status</label>
        <div style="display:flex;align-items:center;gap:10px;margin-top:6px">
          <span id="moltnetStatusBadge" style="background:#1a3a1a;color:#4ade80;padding:4px 12px;border-radius:12px;font-size:12px;font-weight:600">Disconnected</span>
          <span id="moltnetIP" style="font-family:monospace;font-size:12px;color:#888"></span>
        </div>
      </div>
      <div class="field">
        <label>Circuit (Relay Chain)</label>
        <div id="circuitDisplay" style="display:flex;align-items:center;gap:8px;margin-top:8px;font-size:14px">
          <span style="color:#555">Not connected</span>
        </div>
      </div>
      <div class="field" style="display:flex;gap:10px">
        <button class="btn secondary" onclick="newCircuit()" style="font-size:12px;padding:6px 16px">New Circuit</button>
        <select id="exitCountry" onchange="setExitCountry(this.value)" style="background:#111;border:1px solid #333;color:#ccc;padding:6px 10px;border-radius:6px;font-size:12px">
          <option value="">Any country</option>
        </select>
      </div>
    </div>
  </div>

  <div class="section" id="importSection">
    <h2><span class="icon">&#128229;</span> Import &amp; Migration</h2>
    <div class="field">
      <div class="desc">Bring your bookmarks and saved passwords over from another browser installed on this computer.</div>
    </div>
    <div id="browserList" class="field"></div>
    <div class="field">
      <button class="btn" id="importAllBtn" style="display:none" onclick="importFromAll()">Import from all detected browsers</button>
    </div>
    <div id="importStatus" class="desc" style="margin-top:4px"></div>
  </div>

  <div class="section">
    <h2><span class="icon">&#128736;</span> Tools</h2>
    <div class="field">
      <a href="molt://ai-agent/" style="color:#8b5cf6;text-decoration:none;font-weight:600">Open Agent Testing UI &#8594;</a>
      <div class="desc">Test autonomous browser automation (CLICK, SCROLL, NAVIGATE, TYPE)</div>
    </div>
  </div>

  <div class="actions">
    <button class="btn primary" onclick="saveSettings()">Save Settings</button>
    <button class="btn danger" onclick="resetSettings()">Reset to Defaults</button>
  </div>
</div>

<div class="toast" id="toast">Settings saved!</div>

<script>
var idCounter = 0;
var pendingCbs = {};

function sendWithPromise(method) {
  var args = Array.prototype.slice.call(arguments, 1);
  var id = method + '_' + (++idCounter);
  return new Promise(function(resolve, reject) {
    pendingCbs[id] = {resolve: resolve, reject: reject};
    chrome.send(method, [id].concat(args));
  });
}

window.cr = window.cr || {};
cr.webUIResponse = function(id, ok, resp) {
  var cb = pendingCbs[id]; if (cb) { delete pendingCbs[id]; ok ? cb.resolve(resp) : cb.reject(resp); }
};
// C++ FireWebUIListener("event", value) calls cr.webUIListenerCallback in
// JS. This lightweight page doesn't load chrome://resources/js/cr.js, so we
// bridge listener events onto our own DOM-event-based cr.addWebUIListener
// (defined below): each fired listener becomes a CustomEvent whose detail is
// the payload. Used by both "moltnet-status" and "import-progress".
cr.webUIListenerCallback = function(event) {
  var detail = arguments.length > 1 ? arguments[1] : undefined;
  document.dispatchEvent(new CustomEvent(event, {detail: detail}));
};

function showToast(msg) {
  var t = document.getElementById('toast');
  t.textContent = msg;
  t.classList.add('show');
  setTimeout(function() { t.classList.remove('show'); }, 2000);
}

function loadSettingsIntoUI(s) {
  document.getElementById('maxTokens').value = s.max_tokens || 512;
  document.getElementById('maxTokensVal').textContent = s.max_tokens || 512;
  document.getElementById('temperature').value = s.temperature || 0.7;
  document.getElementById('tempVal').textContent = (s.temperature || 0.7).toFixed(1);
  document.getElementById('topP').value = s.top_p || 0.9;
  document.getElementById('topPVal').textContent = (s.top_p || 0.9).toFixed(2);
  document.getElementById('topK').value = s.top_k || 40;
  document.getElementById('topKVal').textContent = s.top_k || 40;
  document.getElementById('maxHistory').value = s.max_history_messages || 16;
  document.getElementById('histVal').textContent = s.max_history_messages || 16;
  document.getElementById('maxPageContent').value = s.max_page_content_chars || 4000;
  document.getElementById('pageVal').textContent = s.max_page_content_chars || 4000;
  document.getElementById('systemPrompt').value = s.system_prompt || '';
  document.getElementById('defaultModel').value = s.default_model || 'tinyllama-1.1b';
  document.getElementById('autoLoadModel').checked = s.auto_load_model !== false;
}

function gatherSettings() {
  return {
    max_tokens: parseInt(document.getElementById('maxTokens').value),
    temperature: parseFloat(document.getElementById('temperature').value),
    top_p: parseFloat(document.getElementById('topP').value),
    top_k: parseInt(document.getElementById('topK').value),
    max_history_messages: parseInt(document.getElementById('maxHistory').value),
    max_page_content_chars: parseInt(document.getElementById('maxPageContent').value),
    system_prompt: document.getElementById('systemPrompt').value,
    default_model: document.getElementById('defaultModel').value,
    auto_load_model: document.getElementById('autoLoadModel').checked
  };
}

function saveSettings() {
  var s = gatherSettings();
  sendWithPromise('saveSettings', s).then(function(r) {
    if (r.success) showToast('Settings saved!');
    else showToast('Error saving settings');
  }).catch(function() { showToast('Error saving settings'); });
}

function resetSettings() {
  sendWithPromise('resetSettings').then(function(s) {
    loadSettingsIntoUI(s);
    showToast('Settings reset to defaults');
  });
}

// ---- Import & Migration (multi-browser) ----
// Renders {phase,done,total} progress events streamed from the C++ handler
// between the bookmark and password write phases. Uses textContent
// throughout so any backend-supplied text (e.g. an error string) can't
// inject markup.
function onImportProgress(p) {
  var status = document.getElementById('importStatus');
  if (!status || !p) return;
  var label = p.phase === 'passwords' ? 'passwords' : 'bookmarks';
  var total = p.total || 0;
  status.textContent = 'Importing ' + label + '… (' +
    (p.done || 0) + (total ? '/' + total : '') + ')';
}

// Per-browser brand-style icon. Small ORIGINAL simplified marks built from each
// browser's signature colors + geometric shapes (nominative use to identify the
// SOURCE browser we import FROM — not pixel-exact trademarked logos). Returns an
// inline <svg> string sized 22x22. Falls back to a neutral globe for unknown ids.
var browserLogos = {
  // Chrome: concentric red/yellow/green ring with a blue center dot.
  chrome:
    '<svg class="browser-logo" viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">' +
    '<circle cx="12" cy="12" r="11" fill="#e8453c"/>' +
    '<path d="M12 1a11 11 0 0 1 9.5 5.5H12a5.5 5.5 0 0 0-4.9 3L3.4 5.2A11 11 0 0 1 12 1z" fill="#fbbc05"/>' +
    '<path d="M2.5 6.7 8 16a5.5 5.5 0 0 0 4 3l-3 4A11 11 0 0 1 2.5 6.7z" fill="#34a853"/>' +
    '<circle cx="12" cy="12" r="4.5" fill="#4285f4"/>' +
    '<circle cx="12" cy="12" r="3.4" fill="#fff"/><circle cx="12" cy="12" r="2.6" fill="#4285f4"/></svg>',
  // Chromium: blue/grey version of the same ring.
  chromium:
    '<svg class="browser-logo" viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">' +
    '<circle cx="12" cy="12" r="11" fill="#557aa8"/>' +
    '<path d="M12 1a11 11 0 0 1 9.5 5.5H12a5.5 5.5 0 0 0-4.9 3L3.4 5.2A11 11 0 0 1 12 1z" fill="#8fb4d6"/>' +
    '<path d="M2.5 6.7 8 16a5.5 5.5 0 0 0 4 3l-3 4A11 11 0 0 1 2.5 6.7z" fill="#a9c7e0"/>' +
    '<circle cx="12" cy="12" r="4.5" fill="#2c5c8f"/>' +
    '<circle cx="12" cy="12" r="3.4" fill="#eef4fa"/><circle cx="12" cy="12" r="2.6" fill="#2c5c8f"/></svg>',
  // Edge: teal-to-blue crescent/swirl.
  edge:
    '<svg class="browser-logo" viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">' +
    '<defs><linearGradient id="mbEdge" x1="0" y1="0" x2="1" y2="1">' +
    '<stop offset="0" stop-color="#37c6d0"/><stop offset="1" stop-color="#1b74c8"/></linearGradient></defs>' +
    '<path d="M12 1a11 11 0 0 1 10.6 8c-1.2-2.4-3.7-3.6-6.4-3.6-4 0-7.2 2.7-7.2 6 0 1.9 1 3.3 2.4 4.3C7 15 3 12.4 3 8.6 3 4.2 7 1 12 1z" fill="url(#mbEdge)"/>' +
    '<path d="M22.6 9c.3 1 .4 2 .4 3 0 6.1-4.9 11-11 11-3.4 0-6-1.4-7.7-3.6 1.6 1 3.6 1.4 5.6 1.1 4.6-.7 7.6-3.7 8.1-6.9.4-2.4-.7-4-2.2-5.1 2.3-.3 4.9.4 6.8.4z" fill="#3aa6e0"/></svg>',
  // Brave: orange shield with an angular lion-mark silhouette.
  brave:
    '<svg class="browser-logo" viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">' +
    '<path d="M12 1.5 20 4l-.6 9.2c-.2 3-2.1 5.6-4.9 6.9L12 21.5l-2.5-1.4c-2.8-1.3-4.7-3.9-4.9-6.9L4 4z" fill="#f15a24"/>' +
    '<path d="M12 4 18 5.8l-.5 7.3c-.1 2.2-1.5 4.1-3.6 5.1L12 19l-1.9-.8c-2.1-1-3.5-2.9-3.6-5.1L6 5.8z" fill="#e8471c"/>' +
    '<path d="M12 7 9.2 9.5l1 1.4-1.7 1.9 1.3 2.2L12 17l2.2-2 1.3-2.2-1.7-1.9 1-1.4z" fill="#fff"/></svg>',
  // Opera: red rounded 'O'.
  opera:
    '<svg class="browser-logo" viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">' +
    '<circle cx="12" cy="12" r="11" fill="#e8203a"/>' +
    '<ellipse cx="12" cy="12" rx="4.6" ry="7.2" fill="#fff"/></svg>',
  // Vivaldi: red rounded square with angular lines.
  vivaldi:
    '<svg class="browser-logo" viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">' +
    '<rect x="1.5" y="1.5" width="21" height="21" rx="6" fill="#ef3939"/>' +
    '<path d="M6 8h5l-2.5 8z" fill="#fff"/><path d="M18 8h-4l1.6 5z" fill="#fff"/>' +
    '<circle cx="12" cy="10" r="1.6" fill="#fff"/></svg>',
  // Firefox: orange-to-yellow rounded flame.
  firefox:
    '<svg class="browser-logo" viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">' +
    '<defs><radialGradient id="mbFf" cx="0.4" cy="0.7" r="0.9">' +
    '<stop offset="0" stop-color="#ffdd44"/><stop offset="0.5" stop-color="#ff8c1a"/>' +
    '<stop offset="1" stop-color="#e0430f"/></radialGradient></defs>' +
    '<path d="M12 1.5c1.5 2 1.2 4 .3 5.4 1.2-.6 2-.2 2.5.6.9-1 .6-2.3.6-2.3 2.8 2 4.6 5.3 4.6 9 0 5.4-4.3 9.8-9.6 9.8S1.5 19.6 1.5 14.2c0-3 1.3-5.4 3.2-6.9-.4 1.4.1 2.7.9 3.4-.6-2.6.6-5.2 2.6-6.7-.5 1.7.2 3 1.3 3.7C11.7 6.3 12.6 3.9 12 1.5z" fill="url(#mbFf)"/>' +
    '<circle cx="11.5" cy="15" r="4.5" fill="#ffd23f" opacity="0.5"/></svg>',
  // Safari: blue circle with a red/white compass needle.
  safari:
    '<svg class="browser-logo" viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">' +
    '<circle cx="12" cy="12" r="11" fill="#1b9df0"/>' +
    '<circle cx="12" cy="12" r="9" fill="#e8f3fb"/>' +
    '<path d="M12 12 16 8l-2 6z" fill="#f4402e"/>' +
    '<path d="M12 12 8 16l2-6z" fill="#c8d6e0"/></svg>',
  globe:
    '<svg class="browser-logo" viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">' +
    '<circle cx="12" cy="12" r="10.5" fill="none" stroke="#888" stroke-width="1.6"/>' +
    '<ellipse cx="12" cy="12" rx="5" ry="10.5" fill="none" stroke="#888" stroke-width="1.6"/>' +
    '<line x1="1.5" y1="12" x2="22.5" y2="12" stroke="#888" stroke-width="1.6"/></svg>'
};

// The list of browsers currently rendered, in row order — used by
// importFromAll() to iterate sequentially.
var detectedBrowsers = [];

// True when the browser is present on this machine. The detection backend sets
// an explicit `installed` flag; treat a missing flag as installed so older
// backends stay backward-compatible.
function isInstalled(b) {
  return b.installed !== false;
}

// Build one row per supported browser. INSTALLED rows show a brand logo + name,
// an 'Include passwords' toggle (disabled with a tooltip when that source has no
// readable password store on this OS), an Import button, and a per-row status
// line. NOT-INSTALLED rows are dimmed and show a 'Not installed' label in place
// of the Import button (no dead button). Installed browsers are listed first.
function renderBrowserList(browsers) {
  var all = browsers || [];
  // Installed first, otherwise preserve backend order (stable sort).
  detectedBrowsers = all.slice().sort(function(a, b) {
    return (isInstalled(a) ? 0 : 1) - (isInstalled(b) ? 0 : 1);
  });
  var list = document.getElementById('browserList');
  list.textContent = '';
  if (!detectedBrowsers.length) {
    var none = document.createElement('div');
    none.className = 'desc';
    none.textContent = 'No supported browsers to import from.';
    list.appendChild(none);
    document.getElementById('importAllBtn').style.display = 'none';
    return;
  }

  var installedCount = 0;
  detectedBrowsers.forEach(function(b) {
    var installed = isInstalled(b);
    if (installed) { installedCount++; }

    var row = document.createElement('div');
    row.className = 'field browser-row' + (installed ? '' : ' not-installed');
    row.style.display = 'flex';
    row.style.alignItems = 'center';
    row.style.gap = '10px';
    row.style.flexWrap = 'wrap';

    var name = document.createElement('span');
    name.style.fontWeight = '600';
    name.style.display = 'flex';
    name.style.alignItems = 'center';
    name.style.gap = '8px';
    name.innerHTML = (browserLogos[b.id] || browserLogos.globe) +
      '<span></span>';
    name.lastChild.textContent = b.display_name;
    row.appendChild(name);

    if (!installed) {
      // Dimmed row: no toggle, no button — just a status marker.
      var notLabel = document.createElement('span');
      notLabel.className = 'not-installed-label';
      notLabel.textContent = 'Not installed';
      row.appendChild(notLabel);
      list.appendChild(row);
      return;
    }

    var pwLabel = document.createElement('label');
    pwLabel.className = 'toggle';
    pwLabel.style.marginLeft = 'auto';
    var pwInput = document.createElement('input');
    pwInput.type = 'checkbox';
    pwInput.className = 'importPasswords';
    if (b.has_passwords_store) {
      pwInput.checked = true;
    } else {
      pwInput.checked = false;
      pwInput.disabled = true;
      pwLabel.classList.add('disabled');
      pwLabel.title = "Passwords can't be imported from " + b.display_name +
        ' on this OS';
    }
    var track = document.createElement('span');
    track.className = 'track';
    var lbl = document.createElement('span');
    lbl.className = 'label';
    lbl.textContent = 'Include passwords';
    pwLabel.appendChild(pwInput);
    pwLabel.appendChild(track);
    pwLabel.appendChild(lbl);
    row.appendChild(pwLabel);

    var btn = document.createElement('button');
    btn.className = 'btn primary';
    btn.textContent = 'Import';
    btn.onclick = function() {
      importBrowser(b.id, pwInput.checked, btn, rowStatus);
    };
    row.appendChild(btn);

    var rowStatus = document.createElement('div');
    rowStatus.className = 'desc';
    rowStatus.style.flexBasis = '100%';
    rowStatus.style.marginTop = '2px';
    row.appendChild(rowStatus);

    // Stash refs on the browser record so importFromAll() can drive the row.
    b._btn = btn;
    b._pwInput = pwInput;
    b._rowStatus = rowStatus;

    list.appendChild(row);
  });

  // The 'Import from all' button only makes sense with more than one installed
  // source.
  document.getElementById('importAllBtn').style.display =
    installedCount > 1 ? '' : 'none';
}

// Compose the final per-import summary from the resolved result.
function importSummary(r, includePasswords) {
  if (!r || !r.success) {
    if (r && r.needs_full_disk_access) {
      return 'Needs Full Disk Access — grant it in System Settings › ' +
        'Privacy & Security › Full Disk Access, then try again';
    }
    return (r && r.error) ? r.error : 'Import failed';
  }
  var msg = '✓ ' + (r.bookmarks_imported || 0) + ' bookmarks';
  if (includePasswords) {
    if (r.needs_full_disk_access) {
      msg += ', 0 passwords — Needs Full Disk Access — grant it in System ' +
        'Settings › Privacy & Security › Full Disk Access, then try again';
    } else if (r.passwords_supported === false) {
      msg += ', 0 passwords — No passwords: not supported for this browser ' +
        'on this OS';
    } else if (r.keychain_denied) {
      msg += ', 0 passwords — No passwords: Keychain permission was denied';
    } else {
      msg += ', ' + (r.passwords_imported || 0) + ' passwords';
      if (r.password_failures) {
        msg += ' (' + r.password_failures + ' could not be imported)';
      }
    }
  }
  return msg;
}

// Import one browser. Returns a promise so importFromAll() can chain. |btn|
// and |rowStatus| are the row's controls (disabled while running); the
// shared #importStatus area mirrors the per-source progress.
function importBrowser(browserId, includePasswords, btn, rowStatus) {
  var status = document.getElementById('importStatus');
  if (btn) btn.disabled = true;
  if (rowStatus) rowStatus.textContent = 'Importing…';
  status.textContent = 'Importing…';
  return sendWithPromise('importFromBrowser', browserId, includePasswords)
    .then(function(r) {
      var msg = importSummary(r, includePasswords);
      if (rowStatus) rowStatus.textContent = msg;
      status.textContent = msg;
      return r;
    }).catch(function(e) {
      var msg = (e && e.error) ? e.error : 'Import failed';
      if (rowStatus) rowStatus.textContent = msg;
      status.textContent = msg;
      return null;
    }).then(function(r) {
      if (btn) btn.disabled = false;
      return r;
    });
}

// Legacy shim so anything still calling importFromChrome() keeps working.
function importFromChrome() {
  return importBrowser('chrome', true, null, null);
}

// Import from every detected browser, one at a time. Each source may raise
// its own Keychain prompt, so we run them sequentially and note that.
function importFromAll() {
  var allBtn = document.getElementById('importAllBtn');
  var status = document.getElementById('importStatus');
  allBtn.disabled = true;
  status.textContent =
    'Importing from all detected browsers — each may prompt for Keychain ' +
    'access…';
  var chain = Promise.resolve();
  detectedBrowsers.forEach(function(b) {
    // Skip not-installed rows — they have no import controls.
    if (!isInstalled(b) || !b._btn) { return; }
    chain = chain.then(function() {
      var wantPw = b._pwInput ? b._pwInput.checked : false;
      return importBrowser(b.id, wantPw, b._btn, b._rowStatus);
    });
  });
  chain.then(function() {
    status.textContent = '✓ Finished importing from all detected browsers';
    allBtn.disabled = false;
  });
}

// Detect installed browsers and render the list on load.
function loadImportableBrowsers() {
  sendWithPromise('getImportableBrowsers').then(function(r) {
    renderBrowserList(r && r.browsers ? r.browsers : []);
  }).catch(function() {
    renderBrowserList([]);
  });
}

// ---- MoltNet Privacy Controls ----
var countryFlags = {DE:'&#127465;&#127466;',NL:'&#127475;&#127473;',SE:'&#127480;&#127466;',
  CH:'&#127464;&#127469;',US:'&#127482;&#127480;',JP:'&#127471;&#127477;',SG:'&#127480;&#127468;',
  FR:'&#127467;&#127479;',GB:'&#127468;&#127463;',CA:'&#127464;&#127462;','??':'&#127760;'};

function toggleMoltNet(enabled) {
  var status = document.getElementById('moltnetStatus');
  status.style.display = enabled ? 'block' : 'none';
  if (enabled) {
    chrome.send('moltnetConnect', [document.getElementById('routingMode').value]);
  } else {
    chrome.send('moltnetDisconnect');
    updateMoltNetUI({status: 'disconnected', relays: []});
  }
}

function setRoutingMode(mode) {
  if (document.getElementById('moltnetEnabled').checked) {
    chrome.send('moltnetConnect', [mode]);
  }
}

function newCircuit() {
  chrome.send('moltnetNewCircuit');
}

function setExitCountry(cc) {
  chrome.send('moltnetSetExitCountry', [cc]);
}

// Populate the exit-country picker from the real backend list
// (TorManager::GetAvailableExitCountries via getMoltnetExitCountries),
// the same source the chat side-panel picker uses. Keeps "Any country"
// as the first option and pre-selects whatever exit is currently set.
function loadExitCountries() {
  sendWithPromise('getMoltnetExitCountries').then(function(data) {
    var sel = document.getElementById('exitCountry');
    if (!sel) return;
    // Drop everything except the leading "Any country" option.
    while (sel.options.length > 1) sel.remove(1);
    (data.available || []).forEach(function(c) {
      var opt = document.createElement('option');
      opt.value = c.code;  // lowercase ISO code, as the backend expects
      opt.textContent = 'Exit: ' + c.name;
      sel.appendChild(opt);
    });
    sel.value = data.selected || '';
  });
}

function updateMoltNetUI(data) {
  var badge = document.getElementById('moltnetStatusBadge');
  var ip = document.getElementById('moltnetIP');
  var circuit = document.getElementById('circuitDisplay');

  if (data.status === 'connected') {
    badge.style.background = '#1a3a1a';
    badge.style.color = '#4ade80';
    badge.textContent = 'Connected';
    ip.textContent = data.apparent_ip || '';
  } else if (data.status === 'connecting') {
    badge.style.background = '#3a3a1a';
    badge.style.color = '#fbbf24';
    badge.textContent = 'Connecting...';
  } else {
    badge.style.background = '#2a1a1a';
    badge.style.color = '#f87171';
    badge.textContent = 'Disconnected';
    ip.textContent = '';
  }

  if (data.relays && data.relays.length > 0) {
    var html = '<span style="color:#4ade80">You</span>';
    data.relays.forEach(function(r) {
      var cc = (r.country || '').toUpperCase();
      var flag = countryFlags[cc] || countryFlags['??'];
      // Real relay identity from the Tor control port: operator nickname
      // (or fingerprint) plus the resolved exit IP — no synthetic latency.
      var tip = (r.relay_id || '') + (r.ip ? ' (' + r.ip + ')' : '');
      html += ' <span style="color:#555">&#8594;</span> <span title="' +
        tip + '">' + flag + '</span>';
    });
    html += ' <span style="color:#555">&#8594;</span> <span style="color:#8b5cf6">&#127760; Internet</span>';
    circuit.innerHTML = html;
  } else {
    circuit.innerHTML = '<span style="color:#555">Not connected</span>';
  }
}

// Listen for MoltNet status updates
window.cr = window.cr || {};
cr.addWebUIListener = cr.addWebUIListener || function(event, fn) {
  document.addEventListener(event, function(e) { fn(e.detail); });
};
cr.addWebUIListener('moltnet-status', updateMoltNetUI);
cr.addWebUIListener('import-progress', onImportProgress);

// In-panel mode: when opened inside the side panel (?panel=1), show a back
// arrow that returns to the chat page via the panel's own history.
var panelParams = new URLSearchParams(window.location.search);
if (panelParams.get('panel') === '1') {
  document.getElementById('backBtn').style.display = 'flex';
}

// ---- Connect AI Providers (cloud / frontier models) ----
// API keys are stored encrypted on-device by the backend and are NEVER
// returned to this page — getProviders reports only connected state + the
// enabled model list. saveProviderKey validates the key over the network
// and, on success, returns the provider's full model list for one-click
// enabling.
var providersData = [];
var fetchedModels = {};  // provider_id -> [model id]  (from the last connect)

function loadProviders() {
  sendWithPromise('getProviders').then(function(r) {
    providersData = (r && r.providers) || [];
    renderProviders();
  });
}

function providerById(id) {
  for (var i = 0; i < providersData.length; i++) {
    if (providersData[i].id === id) return providersData[i];
  }
  return null;
}

function exampleModel(id) {
  var ex = {
    openai: 'gpt-5.1', anthropic: 'claude-opus-4-8', gemini: 'gemini-2.5-pro',
    openrouter: 'anthropic/claude-opus-4', xai: 'grok-4',
    deepseek: 'deepseek-chat', groq: 'llama-3.1-70b-versatile',
    mistral: 'mistral-large-latest', perplexity: 'sonar-pro'
  };
  return ex[id] || 'model-name';
}

function renderProviders() {
  var host = document.getElementById('providersList');
  if (!host) return;
  host.innerHTML = '';
  providersData.forEach(function(p) {
    var row = document.createElement('div');
    row.className = 'provider-row';

    var head = document.createElement('div');
    head.className = 'provider-head';
    var name = document.createElement('span');
    name.className = 'provider-name';
    name.textContent = p.display_name;
    head.appendChild(name);

    var btn = document.createElement('button');
    btn.className = 'btn secondary provider-btn';
    if (p.connected) {
      var badge = document.createElement('span');
      badge.className = 'provider-badge';
      badge.textContent = '✓ Connected';
      head.appendChild(badge);
      btn.textContent = 'Disconnect';
      btn.onclick = function() { disconnectProvider(p.id); };
    } else {
      btn.textContent = 'Connect';
      btn.onclick = function() { toggleConnectForm(p.id); };
    }
    head.appendChild(btn);
    row.appendChild(head);

    var body = document.createElement('div');
    body.className = 'provider-body';
    body.id = 'pbody-' + p.id;
    if (p.connected) {
      renderConnectedBody(body, p);
    } else {
      renderConnectForm(body, p);
      body.style.display = 'none';
    }
    row.appendChild(body);
    host.appendChild(row);
  });
}

function toggleConnectForm(id) {
  var body = document.getElementById('pbody-' + id);
  if (!body) return;
  body.style.display = (body.style.display === 'none') ? 'block' : 'none';
  if (body.style.display === 'block') {
    var inp = body.querySelector('.pkey');
    if (inp) inp.focus();
  }
}

function renderConnectForm(body, p) {
  body.innerHTML = '';
  var form = document.createElement('div');
  form.className = 'provider-form';

  var key = document.createElement('input');
  key.type = 'password';
  key.className = 'pkey';
  key.placeholder = 'Paste your ' + p.display_name + ' API key';
  key.autocomplete = 'off';
  form.appendChild(key);

  var base = null;
  if (p.custom_base_url) {
    base = document.createElement('input');
    base.type = 'text';
    base.className = 'pbase';
    base.placeholder = 'Base URL (OpenAI-compatible endpoint)';
    form.appendChild(base);
  }

  var errEl = document.createElement('div');
  errEl.className = 'provider-err';
  errEl.style.display = 'none';
  form.appendChild(errEl);

  var rowb = document.createElement('div');
  rowb.className = 'row';
  var connectBtn = document.createElement('button');
  connectBtn.className = 'btn primary';
  connectBtn.style.cssText = 'font-size:12px;padding:6px 16px';
  connectBtn.textContent = 'Connect';
  connectBtn.onclick = function() {
    var k = key.value.trim();
    if (!k) {
      errEl.textContent = 'Enter an API key.';
      errEl.style.display = 'block';
      return;
    }
    errEl.style.display = 'none';
    connectBtn.disabled = true;
    connectBtn.textContent = 'Connecting…';
    var bu = base ? base.value.trim() : '';
    sendWithPromise('saveProviderKey', p.id, k, bu).then(function(r) {
      connectBtn.disabled = false;
      connectBtn.textContent = 'Connect';
      if (!r || !r.success) {
        errEl.textContent = (r && r.error) ? r.error : 'Could not connect.';
        errEl.style.display = 'block';
        return;
      }
      fetchedModels[p.id] = r.models || [];
      p.connected = true;
      p.enabled_models = r.enabled_models || [];
      showToast(p.display_name + ' connected');
      renderProviders();
    });
  };
  rowb.appendChild(connectBtn);
  form.appendChild(rowb);

  var hint = document.createElement('div');
  hint.className = 'provider-hint';
  hint.textContent = 'Your key is validated, then encrypted on this device. ' +
      'It is sent only to ' + p.display_name + ' — never to us.';
  form.appendChild(hint);

  body.appendChild(form);
}

function renderConnectedBody(body, p) {
  body.innerHTML = '';
  var enabled = p.enabled_models || [];
  var full = fetchedModels[p.id];

  if (full && full.length) {
    // Freshly fetched this session: show every model with a checkbox.
    var lbl = document.createElement('div');
    lbl.className = 'provider-hint';
    lbl.textContent = 'Choose which models to show in the chat model picker:';
    body.appendChild(lbl);
    var scroll = document.createElement('div');
    scroll.className = 'model-scroll';
    full.forEach(function(m) {
      var rowm = document.createElement('label');
      rowm.className = 'model-toggle';
      var cb = document.createElement('input');
      cb.type = 'checkbox';
      cb.checked = enabled.indexOf(m) !== -1;
      cb.onchange = function() { setCloudModelEnabled(p.id, m, cb.checked); };
      var nm = document.createElement('span');
      nm.className = 'mt-name';
      nm.textContent = m;
      rowm.appendChild(cb);
      rowm.appendChild(nm);
      scroll.appendChild(rowm);
    });
    body.appendChild(scroll);
    return;
  }

  // Reloaded session (no fetched list): enabled models as removable chips,
  // plus a free-text add box so a model can be enabled by id with no key
  // round-trip.
  if (enabled.length) {
    var chips = document.createElement('div');
    chips.className = 'model-chips';
    enabled.forEach(function(m) {
      var chip = document.createElement('span');
      chip.className = 'model-chip';
      chip.appendChild(document.createTextNode(m));
      var x = document.createElement('button');
      x.textContent = '×';
      x.title = 'Remove';
      x.onclick = function() { setCloudModelEnabled(p.id, m, false); };
      chip.appendChild(x);
      chips.appendChild(chip);
    });
    body.appendChild(chips);
  } else {
    var none = document.createElement('div');
    none.className = 'provider-hint';
    none.textContent = 'No models enabled yet — add one below.';
    body.appendChild(none);
  }

  var addRow = document.createElement('div');
  addRow.className = 'row';
  addRow.style.marginTop = '8px';
  var addInp = document.createElement('input');
  addInp.type = 'text';
  addInp.placeholder = 'Model id (e.g. ' + exampleModel(p.id) + ')';
  addInp.style.cssText = 'flex:1;padding:8px 12px;border-radius:8px;border:1px solid #333;background:#0a0a0a;color:#e0e0e0;font-size:13px;outline:none';
  var addBtn = document.createElement('button');
  addBtn.className = 'btn secondary';
  addBtn.style.cssText = 'font-size:12px;padding:6px 14px';
  addBtn.textContent = 'Add';
  var doAdd = function() {
    var m = addInp.value.trim();
    if (m) { setCloudModelEnabled(p.id, m, true); }
  };
  addBtn.onclick = doAdd;
  addInp.onkeydown = function(e) { if (e.key === 'Enter') doAdd(); };
  addRow.appendChild(addInp);
  addRow.appendChild(addBtn);
  body.appendChild(addRow);
}

function setCloudModelEnabled(providerId, model, enabled) {
  sendWithPromise('setModelEnabled', providerId, model, enabled).then(
      function(r) {
        if (r && r.success) {
          var p = providerById(providerId);
          if (p) p.enabled_models = r.enabled_models || [];
          renderProviders();
        }
      });
}

function disconnectProvider(id) {
  sendWithPromise('removeProvider', id).then(function(r) {
    if (r && r.success) {
      delete fetchedModels[id];
      var p = providerById(id);
      if (p) { p.connected = false; p.enabled_models = []; }
      renderProviders();
    }
  });
}

// Init
sendWithPromise('getSettings').then(function(s) {
  loadSettingsIntoUI(s);
});
// Drive the exit-country picker from the real backend list.
loadExitCountries();
// Detect installed browsers for the Import & Migration section.
loadImportableBrowsers();
// Load configured cloud/frontier providers.
loadProviders();

// Deep link: molt://ai-settings/?section=import scrolls to (and briefly
// highlights) the Import & Migration section. Used by the bookmarks
// side-panel "Import bookmarks" button. The molt:// rewriter preserves the
// path + query, so the query is present here.
(function() {
  var deepLinkParams = new URLSearchParams(window.location.search);
  var section = deepLinkParams.get('section');
  var targetId = section === 'import' ? 'importSection'
               : section === 'providers' ? 'providersSection'
               : null;
  if (targetId) {
    var el = document.getElementById(targetId);
    if (el) {
      el.scrollIntoView({behavior: 'smooth', block: 'start'});
      var prevTransition = el.style.transition;
      var prevShadow = el.style.boxShadow;
      el.style.transition = 'box-shadow 0.4s ease';
      el.style.boxShadow = '0 0 0 2px #6366f1';
      setTimeout(function() {
        el.style.boxShadow = prevShadow;
        setTimeout(function() {
          el.style.transition = prevTransition;
        }, 500);
      }, 1600);
    }
  }
})();
</script>
</body>
</html>)HTML";

    std::move(callback).Run(
        base::MakeRefCounted<base::RefCountedString>(std::move(html)));
  }

  bool ShouldReplaceExistingSource() override { return true; }
};

}  // namespace

MoltAISettingsUI::MoltAISettingsUI(content::WebUI* web_ui)
    : content::WebUIController(web_ui) {
  content::URLDataSource::Add(
      web_ui->GetWebContents()->GetBrowserContext(),
      std::make_unique<MoltAISettingsDataSource>());

  web_ui->AddMessageHandler(std::make_unique<MoltAISettingsHandler>());
}

MoltAISettingsUI::~MoltAISettingsUI() = default;
