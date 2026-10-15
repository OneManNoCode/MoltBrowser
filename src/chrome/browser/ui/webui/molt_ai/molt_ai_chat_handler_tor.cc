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

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/tor/exit_country_names.h"
#include "chrome/browser/molt_ai/tor/molt_net_window.h"
#include "chrome/browser/molt_ai/tor/tor_manager.h"
#include "chrome/browser/molt_ai/tor/tor_service.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/web_contents.h"
#include "chrome/browser/ui/webui/molt_ai/molt_ai_chat_handler_helpers.h"
#include "content/public/browser/web_ui.h"
#include "url/gurl.h"

using molt_ai::chat_handler_helpers::HasCallbackArgs;

// ------------------------------------------------------------------
// HandleGetTorStatus: reports whether MoltNet's own Tor is running (an
// authenticated probe of its control port — never a system Tor on
// 127.0.0.1:9051) plus where the binary comes from.
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleGetTorStatus(const base::ListValue& args) {
  if (!HasCallbackArgs(args, 1u, __func__)) return;
  AllowJavascript();
  const std::string callback_id = args[0].GetString();
  auto weak_this = weak_ptr_factory_.GetWeakPtr();
  std::string cb = callback_id;
  // Snapshot binary resolution now so we can include it in the
  // response regardless of whether Tor is currently running.
  const molt_ai::tor::TorBinaryInfo tor =
      molt_ai::tor::TorManager::Get()->FindTorBinary();
  bool is_bundled = tor.bundled;
  std::string resolved_path = tor.path.AsUTF8Unsafe();
  std::string missing_hint = tor.error;
  molt_ai::tor::TorService::Get()->Probe(base::BindOnce(
      [](base::WeakPtr<MoltAIChatHandler> self, std::string cb,
         std::string resolved_path, bool is_bundled, std::string missing_hint,
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
          //   - We have nothing — TorManager says what to do ("Install the
          //     tor package" on Linux, a bundled build elsewhere), never to
          //     install via brew.
          if (resolved_path.empty()) {
            out.Set("install_hint", missing_hint);
          } else {
            out.Set("install_hint",
                    "Tor binary is available at " + resolved_path +
                    ". Run /tor launch to start it (~15s to bootstrap).");
          }
        }
        self->ResolveJavascriptCallback(base::Value(cb),
                                        base::Value(std::move(out)));
      },
      weak_this, cb, resolved_path, is_bundled, missing_hint));
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
  if (!HasCallbackArgs(args, 1u, __func__)) return;
  AllowJavascript();
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
  if (!HasCallbackArgs(args, 1u, __func__)) return;
  AllowJavascript();
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
  if (!HasCallbackArgs(args, 1u, __func__)) return;
  AllowJavascript();
  const std::string callback_id = args[0].GetString();
  // Closes MoltNet windows too: with Tor gone they could only show errors.
  molt_ai::tor::MoltNetWindow::TurnOff();
  base::DictValue out;
  out.Set("success", true);
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}

// ------------------------------------------------------------------
// HandleGetTorExitCountries: populate the exit-country picker. Returns
//   { selected: "<cc or ''>",
//     available: [ { code: "<cc>", name: "<display name>" }, ... ] }
// The available list comes from TorManager's curated static list
// (lowercase ISO alpha-2), named via the shared exit_country_names.h.
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleGetTorExitCountries(
    const base::ListValue& args) {
  if (!HasCallbackArgs(args, 1u, __func__)) return;
  AllowJavascript();
  const std::string callback_id = args[0].GetString();

  molt_ai::tor::TorManager* mgr = molt_ai::tor::TorManager::Get();
  base::DictValue out;
  out.Set("selected", mgr->GetExitCountry());
  base::ListValue available;
  for (const std::string& cc : mgr->GetAvailableExitCountries()) {
    base::DictValue entry;
    entry.Set("code", cc);
    entry.Set("name", molt_ai::tor::ExitCountryDisplayName(cc));
    available.Append(std::move(entry));
  }
  out.Set("available", std::move(available));
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}

// ------------------------------------------------------------------
// HandleSetTorExitCountry: constrain (or clear) the Tor exit relay's
// country. Arg 1 is a lowercase ISO alpha-2 code, or "" for "any". The pin
// is session-only and, if Tor is running, applied live via SETCONF +
// NEWNYM. Resolves { success, selected[, error] }.
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleSetTorExitCountry(
    const base::ListValue& args) {
  if (!HasCallbackArgs(args, 1u, __func__)) return;
  AllowJavascript();
  const std::string callback_id = args[0].GetString();
  std::string cc = (args.size() > 1 && args[1].is_string())
                       ? args[1].GetString()
                       : std::string();
  // Normalize to lowercase so the backend's ISO comparison is stable
  // regardless of what the UI sent.
  cc = base::ToLowerASCII(cc);

  // Session-only; applied live (SETCONF + NEWNYM) if Tor is running.
  molt_ai::tor::TorManager::Get()->SetExitCountry(
      cc, base::BindOnce(
              [](base::WeakPtr<MoltAIChatHandler> self, std::string cb, bool ok,
                 std::string error) {
                if (!self) return;
                base::DictValue out;
                out.Set("success", ok);
                out.Set("selected",
                        molt_ai::tor::TorManager::Get()->GetExitCountry());
                if (!ok) out.Set("error", error);
                self->ResolveJavascriptCallback(base::Value(cb),
                                                base::Value(std::move(out)));
              },
              weak_ptr_factory_.GetWeakPtr(), callback_id));
}

// ------------------------------------------------------------------
// HandleOpenTorTab (/tor open <url>): opens |url| in a MoltNet window —
// the isolated Tor profile, verified to route through Tor before anything
// loads. It never proxies (or opens the page in) the primary incognito
// profile, whose cookies are shared with the user's other private windows.
// Resolves { success, url } or { success: false, error }.
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleOpenTorTab(const base::ListValue& args) {
  if (!HasCallbackArgs(args, 2u, __func__)) return;
  AllowJavascript();
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
  molt_ai::tor::MoltNetWindow::Open(
      profile, g,
      base::BindOnce(
          [](base::WeakPtr<MoltAIChatHandler> self, std::string cb,
             std::string url, bool ok, std::string error) {
            if (!self) return;
            base::DictValue result;
            result.Set("success", ok);
            if (ok) {
              result.Set("url", url);
            } else {
              result.Set("error", error);
            }
            self->ResolveJavascriptCallback(base::Value(cb),
                                            base::Value(std::move(result)));
          },
          weak_ptr_factory_.GetWeakPtr(), callback_id, g.spec()));
}
