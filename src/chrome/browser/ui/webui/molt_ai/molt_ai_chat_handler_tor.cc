// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Tor handler methods for MoltAIChatHandler. Split out of
// molt_ai_chat_handler.cc as part of MEDIUM #10 from the 2026-05-20
// code review — the parent file had grown to ~4500 lines with 44 IPC
// handlers and needed feature-based subdivision.
//
// All these methods are members of MoltAIChatHandler (declared in the
// shared molt_ai_chat_handler.h). They run on the UI thread, dispatch
// any blocking work to ThreadPool tasks, and reply via
// ResolveJavascriptCallback on completion.

#include "chrome/browser/ui/webui/molt_ai/molt_ai_chat_handler.h"

#include <string>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/tor/tor_manager.h"
#include "chrome/browser/molt_ai/tor/tor_service.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_commands.h"
#include "components/prefs/pref_service.h"
#include "components/proxy_config/proxy_config_dictionary.h"
#include "components/proxy_config/proxy_config_pref_names.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "url/gurl.h"

// ------------------------------------------------------------------
// HandleGetTorStatus: Phase B.1 of the privacy tentpole.
// Probes the local Tor control port (127.0.0.1:9051) and reports
// version + running state. Honest about scope — if tor isn't
// installed/configured we tell the user how to fix it.
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleGetTorStatus(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 1u);
  const std::string callback_id = args[0].GetString();
  auto weak_this = weak_ptr_factory_.GetWeakPtr();
  std::string cb = callback_id;
  // Snapshot binary resolution now so we can include it in the
  // response regardless of whether Tor is currently running.
  base::FilePath resolved_bin =
      molt_ai::tor::TorManager::Get()->ResolveTorBinary();
  bool is_bundled = molt_ai::tor::TorManager::Get()->IsUsingBundledTor();
  std::string resolved_path = resolved_bin.AsUTF8Unsafe();
  molt_ai::tor::TorService::Get()->Probe(base::BindOnce(
      [](base::WeakPtr<MoltAIChatHandler> self, std::string cb,
         std::string resolved_path, bool is_bundled,
         molt_ai::tor::TorStatus s) {
        if (!self) return;
        base::DictValue out;
        out.Set("running", s.running);
        out.Set("version", s.version);
        out.Set("control_port", s.control_port_addr);
        out.Set("socks_port", s.socks_port_addr);
        out.Set("binary_path", resolved_path);
        out.Set("binary_source",
                resolved_path.empty()
                    ? std::string("none")
                    : (is_bundled ? std::string("bundled")
                                  : std::string("system")));
        if (!s.running) {
          out.Set("error", s.error);
          // Two cases:
          //   - We have a binary (bundled or system) but Tor isn't
          //     running yet — tell the user to /tor launch.
          //   - We have nothing — tell them to rebuild with bundling,
          //     not to install via brew.
          if (resolved_path.empty()) {
            out.Set("install_hint",
                    "No tor binary is bundled with this build. Rebuild "
                    "after running scripts/bundle-tor.sh (or use a fresh "
                    "release DMG, which ships with Tor inside).");
          } else {
            out.Set("install_hint",
                    "Tor binary is available at " + resolved_path +
                    ". Run /tor launch to start it (~15s to bootstrap).");
          }
        }
        self->ResolveJavascriptCallback(base::Value(cb),
                                        base::Value(std::move(out)));
      },
      weak_this, cb, resolved_path, is_bundled));
}

// ------------------------------------------------------------------
// HandleGetTorCircuits: returns the live circuit list from Tor's
// control port — id, state, purpose, and the ordered list of relay
// hops (guard → middle → exit). Each hop has a fingerprint and (when
// the operator set one) a nickname.
//
// Phase B.2 will enrich each hop with IP + country/city via GeoIP.
// For now the visualizer shows nicknames + fingerprints which are
// already useful — relay operators choose memorable nicknames
// (artichoke, F3Netze, niftyforestbear, etc.).
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleGetTorCircuits(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 1u);
  const std::string callback_id = args[0].GetString();
  auto weak_this = weak_ptr_factory_.GetWeakPtr();
  std::string cb = callback_id;
  // Phase B.2: use the *enriched* call so each hop carries IP +
  // country. Costs ~50ms extra over the local socket (a few extra
  // GETINFO round-trips). Worth it — relay nicknames mean nothing
  // to a non-power-user, but country flags are immediately legible.
  molt_ai::tor::TorService::Get()->GetCircuitsEnriched(base::BindOnce(
      [](base::WeakPtr<MoltAIChatHandler> self, std::string cb,
         std::vector<molt_ai::tor::TorCircuit> circuits) {
        if (!self) return;
        base::ListValue arr;
        for (const auto& c : circuits) {
          base::DictValue d;
          d.Set("id", c.id);
          d.Set("state", c.state);
          d.Set("purpose", c.purpose);
          base::ListValue hops;
          for (const auto& h : c.hops) {
            base::DictValue hd;
            hd.Set("fingerprint", h.fingerprint);
            hd.Set("nickname", h.nickname);
            hd.Set("ip", h.ip);
            hd.Set("country", h.country);
            hops.Append(std::move(hd));
          }
          d.Set("hops", std::move(hops));
          arr.Append(std::move(d));
        }
        base::DictValue out;
        out.Set("circuits", std::move(arr));
        out.Set("count", static_cast<int>(circuits.size()));
        self->ResolveJavascriptCallback(base::Value(cb),
                                        base::Value(std::move(out)));
      },
      weak_this, cb));
}

// ------------------------------------------------------------------
// HandleLaunchTor: Phase B.2 — find a tor binary at common install
// paths and spawn it as a child process of MoltBrowser. Waits up to
// 45s for the control port to come up before reporting success.
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleLaunchTor(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 1u);
  const std::string callback_id = args[0].GetString();
  auto weak_this = weak_ptr_factory_.GetWeakPtr();
  std::string cb = callback_id;
  molt_ai::tor::TorManager::Get()->Launch(base::BindOnce(
      [](base::WeakPtr<MoltAIChatHandler> self, std::string cb,
         molt_ai::tor::TorLaunchResult r) {
        if (!self) return;
        base::DictValue out;
        out.Set("success", r.success);
        out.Set("binary_path", r.binary_path);
        out.Set("pid", r.pid);
        if (!r.error.empty()) out.Set("error", r.error);
        self->ResolveJavascriptCallback(base::Value(cb),
                                        base::Value(std::move(out)));
      },
      weak_this, cb));
}

void MoltAIChatHandler::HandleStopTor(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 1u);
  const std::string callback_id = args[0].GetString();
  molt_ai::tor::TorManager::Get()->Stop();
  base::DictValue out;
  out.Set("success", true);
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}

// ------------------------------------------------------------------
// HandleOpenTorTab: configure the primary OTR profile's proxy pref
// to route through Tor's SOCKS5 port (127.0.0.1:9050), then open
// |url| in a new OTR window. Any subsequent tab opened in that same
// OTR profile shares the proxy. To revert, the user runs /tor close
// which clears the pref.
//
// This is the v1 routing approach — coarse-grained (whole-profile)
// but honest. Per-tab routing within a single profile would require
// a custom NetworkContext, which is the Phase B.3 polish.
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleOpenTorTab(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 2u);
  const std::string callback_id = args[0].GetString();
  std::string url_str = args[1].is_string() ? args[1].GetString() : "";
  base::DictValue out;
  if (url_str.empty()) {
    out.Set("success", false);
    out.Set("error", "missing url");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  if (!url_str.starts_with("http://") && !url_str.starts_with("https://")) {
    url_str = "https://" + url_str;
  }
  GURL g(url_str);
  if (!g.is_valid()) {
    out.Set("success", false);
    out.Set("error", "invalid url");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  Profile* profile = Profile::FromBrowserContext(
      web_ui()->GetWebContents()->GetBrowserContext());
  if (!profile) {
    out.Set("success", false);
    out.Set("error", "no profile");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  Profile* original = profile->GetOriginalProfile();
  Profile* otr = original->GetPrimaryOTRProfile(/*create_if_needed=*/true);
  if (!otr) {
    out.Set("success", false);
    out.Set("error", "could not create OTR profile");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }
  // Configure SOCKS5 proxy on the OTR profile's prefs. The proxy
  // string format is "socks5://host:port".
  base::Value proxy_dict(ProxyConfigDictionary::CreateFixedServers(
      "socks5://127.0.0.1:9050", /*bypass_list=*/std::string()));
  otr->GetPrefs()->Set(proxy_config::prefs::kProxy, proxy_dict);

  chrome::OpenURLOffTheRecord(original, g);

  out.Set("success", true);
  out.Set("url", g.spec());
  out.Set("proxy", "socks5://127.0.0.1:9050");
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}
