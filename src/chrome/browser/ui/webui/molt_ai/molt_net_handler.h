// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// WebUIMessageHandler backing chrome://molt-net/. Bridges the glass popover's
// chrome.send() calls to the process-global molt_ai::tor::TorManager — the
// same backend the toolbar globe menu drives. No mojo, no Profile plumbing:
// every call goes through TorManager::Get().

#ifndef CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_NET_HANDLER_H_
#define CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_NET_HANDLER_H_

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "content/public/browser/web_ui_message_handler.h"

class MoltNetHandler : public content::WebUIMessageHandler {
 public:
  MoltNetHandler();
  ~MoltNetHandler() override;

  MoltNetHandler(const MoltNetHandler&) = delete;
  MoltNetHandler& operator=(const MoltNetHandler&) = delete;

  // content::WebUIMessageHandler:
  void RegisterMessages() override;

 private:
  // Each takes [callback_id, ...args] and resolves the promise on the JS side.
  void HandleGetStatus(const base::ListValue& args);
  void HandleGetExitCountries(const base::ListValue& args);
  void HandleSetExitCountry(const base::ListValue& args);
  void HandleToggle(const base::ListValue& args);
  void HandleNewIdentity(const base::ListValue& args);

  base::WeakPtrFactory<MoltNetHandler> weak_ptr_factory_{this};
};

#endif  // CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_NET_HANDLER_H_
