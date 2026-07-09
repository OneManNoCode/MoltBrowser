// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/webui/molt_ai/molt_net_handler.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "base/check_op.h"
#include "base/functional/bind.h"
#include "base/strings/string_util.h"
#include "chrome/browser/molt_ai/tor/tor_manager.h"
#include "content/public/browser/web_ui.h"

namespace {

// Display names for the curated exit-country codes. Mirrors the copy in
// molt_ai_chat_handler_tor.cc (kept file-local there); keep in sync with
// TorManager::GetAvailableExitCountries(). Unknown codes render as their
// uppercased ISO code so the picker never shows a blank row.
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
  if (it != kNames->end()) {
    return it->second;
  }
  return base::ToUpperASCII(cc);
}

// The single status shape every mutating call resolves, so the page can
// re-render from one payload: { running: bool, selected: "<lc cc or ''>" }.
base::DictValue StatusDict() {
  molt_ai::tor::TorManager* mgr = molt_ai::tor::TorManager::Get();
  base::DictValue d;
  d.Set("running", mgr->IsRunning());
  d.Set("selected", mgr->GetExitCountry());
  return d;
}

}  // namespace

MoltNetHandler::MoltNetHandler() = default;
MoltNetHandler::~MoltNetHandler() = default;

void MoltNetHandler::RegisterMessages() {
  web_ui()->RegisterMessageCallback(
      "moltnet.getStatus",
      base::BindRepeating(&MoltNetHandler::HandleGetStatus,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "moltnet.getExitCountries",
      base::BindRepeating(&MoltNetHandler::HandleGetExitCountries,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "moltnet.setExitCountry",
      base::BindRepeating(&MoltNetHandler::HandleSetExitCountry,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "moltnet.toggle",
      base::BindRepeating(&MoltNetHandler::HandleToggle,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "moltnet.newIdentity",
      base::BindRepeating(&MoltNetHandler::HandleNewIdentity,
                          base::Unretained(this)));
}

void MoltNetHandler::HandleGetStatus(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 1u);
  const std::string callback_id = args[0].GetString();
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(StatusDict()));
}

void MoltNetHandler::HandleGetExitCountries(const base::ListValue& args) {
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

void MoltNetHandler::HandleSetExitCountry(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 1u);
  const std::string callback_id = args[0].GetString();
  // Arg 1 is a lowercase ISO alpha-2 code, or "" for Auto/any.
  std::string cc = (args.size() > 1 && args[1].is_string())
                       ? base::ToLowerASCII(args[1].GetString())
                       : std::string();
  molt_ai::tor::TorManager::Get()->SetExitCountry(cc);
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(StatusDict()));
}

void MoltNetHandler::HandleToggle(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 1u);
  const std::string callback_id = args[0].GetString();
  molt_ai::tor::TorManager* mgr = molt_ai::tor::TorManager::Get();
  if (mgr->IsRunning()) {
    mgr->Stop();
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(StatusDict()));
    return;
  }
  // Launch is async; resolve once the control port is up (or the attempt
  // fails — StatusDict() reflects the real IsRunning() either way).
  mgr->Launch(base::BindOnce(
      [](base::WeakPtr<MoltNetHandler> self, std::string cb,
         molt_ai::tor::TorLaunchResult) {
        if (!self) {
          return;
        }
        self->ResolveJavascriptCallback(base::Value(cb),
                                        base::Value(StatusDict()));
      },
      weak_ptr_factory_.GetWeakPtr(), callback_id));
}

void MoltNetHandler::HandleNewIdentity(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 1u);
  const std::string callback_id = args[0].GetString();
  molt_ai::tor::TorManager* mgr = molt_ai::tor::TorManager::Get();
  // Re-applying the current exit country rewrites the torrc and, if Tor is
  // running, reloads it in place to rebuild circuits — the sanctioned NEWNYM
  // path the toolbar menu uses.
  mgr->SetExitCountry(mgr->GetExitCountry());
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(StatusDict()));
}
