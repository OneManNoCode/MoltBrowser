// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/webui/molt_ai/molt_ai_settings_ui.h"

#include <map>
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
#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/common/molt_blocking_scope.h"
#include "chrome/browser/molt_ai/tor/tor_manager.h"
#include "chrome/browser/molt_ai/tor/tor_service.h"
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
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#0a0a0a;color:#e0e0e0;min-height:100vh;padding:40px 20px}
.container{max-width:640px;margin:0 auto}
.logo-area{display:flex;align-items:center;gap:10px;margin-bottom:4px}.logo-img{width:44px;height:44px;border-radius:12px;box-shadow:0 2px 10px rgba(0,0,0,0.5)}.logo-text{font-size:26px;font-weight:700;background:linear-gradient(135deg,#6366f1,#a855f7);-webkit-background-clip:text;-webkit-text-fill-color:transparent}
.back-btn{display:none;align-items:center;justify-content:center;width:30px;height:30px;border:1px solid #333;border-radius:8px;color:#6366f1;text-decoration:none;font-size:16px;line-height:1;cursor:pointer;transition:all 0.2s;flex-shrink:0}
.back-btn:hover{border-color:#6366f1;background:#111}
.subtitle{color:#888;margin-bottom:24px;font-size:14px}
.nav{display:flex;gap:12px;margin-bottom:24px}
.nav a{color:#6366f1;text-decoration:none;font-size:13px;padding:6px 12px;border:1px solid #333;border-radius:8px;transition:all 0.2s}
.nav a:hover{border-color:#6366f1;background:#111}
.section{background:#111;border:1px solid #222;border-radius:12px;padding:20px;margin-bottom:16px}
.section h2{font-size:16px;font-weight:600;margin-bottom:16px;color:#e0e0e0;display:flex;align-items:center;gap:8px}
.section h2 .icon{font-size:18px}
.field{margin-bottom:16px}
.field:last-child{margin-bottom:0}
.field label{display:block;font-size:13px;font-weight:600;color:#aaa;margin-bottom:6px}
.field .desc{font-size:11px;color:#666;margin-bottom:6px}
.field input[type="number"],.field input[type="text"],.field textarea{width:100%;padding:10px 14px;border-radius:8px;border:1px solid #333;background:#0d0d0d;color:#e0e0e0;font-size:13px;outline:none;transition:border-color 0.2s;font-family:inherit}
.field input:focus,.field textarea:focus{border-color:#6366f1}
.field textarea{resize:vertical;min-height:80px}
.field select{padding:10px 14px;border-radius:8px;border:1px solid #333;background:#0d0d0d;color:#e0e0e0;font-size:13px;outline:none;width:100%;cursor:pointer}
.field .range-wrap{display:flex;align-items:center;gap:12px}
.field input[type="range"]{flex:1;accent-color:#6366f1}
.field .range-val{font-size:13px;color:#6366f1;min-width:40px;text-align:right;font-weight:600}
.toggle{display:flex;align-items:center;gap:10px;cursor:pointer}
.toggle input{display:none}
.toggle .track{width:40px;height:22px;border-radius:11px;background:#333;position:relative;transition:background 0.2s}
.toggle input:checked + .track{background:#6366f1}
.toggle .track::after{content:'';position:absolute;top:2px;left:2px;width:18px;height:18px;border-radius:50%;background:#e0e0e0;transition:transform 0.2s}
.toggle input:checked + .track::after{transform:translateX(18px)}
.toggle .label{font-size:13px}
.actions{display:flex;gap:10px;margin-top:20px}
.btn{padding:10px 24px;border-radius:10px;border:none;font-size:14px;font-weight:600;cursor:pointer;transition:all 0.2s}
.btn.primary{background:linear-gradient(135deg,#6366f1,#8b5cf6);color:white}
.btn.primary:hover{opacity:0.85}
.btn.secondary{background:#111;border:1px solid #333;color:#aaa}
.btn.secondary:hover{border-color:#6366f1;color:#e0e0e0}
.btn.danger{background:transparent;border:1px solid #f87171;color:#f87171}
.btn.danger:hover{background:#2a1111}
.toast{position:fixed;bottom:24px;right:24px;padding:12px 20px;border-radius:10px;background:#1a2e1a;color:#4ade80;font-size:13px;font-weight:600;border:1px solid #2a4a2a;transform:translateY(100px);opacity:0;transition:all 0.3s}
.toast.show{transform:translateY(0);opacity:1}
.model-dir{font-family:monospace;font-size:12px;color:#888;padding:8px 12px;background:#0a0a0a;border-radius:6px;border:1px solid #1a1a1a;margin-top:6px}
</style>
</head>
<body>
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

// In-panel mode: when opened inside the side panel (?panel=1), show a back
// arrow that returns to the chat page via the panel's own history.
var panelParams = new URLSearchParams(window.location.search);
if (panelParams.get('panel') === '1') {
  document.getElementById('backBtn').style.display = 'flex';
}

// Init
sendWithPromise('getSettings').then(function(s) {
  loadSettingsIntoUI(s);
});
// Drive the exit-country picker from the real backend list.
loadExitCountries();
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
