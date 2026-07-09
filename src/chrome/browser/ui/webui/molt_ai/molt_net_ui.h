// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// chrome://molt-net/ — MoltNet (Tor privacy routing) control surface.
// A local, self-contained "Liquid Glass" page: a connection status row, an
// enable toggle, and the exit-country picker (flags + names). Served as inline
// HTML via URLDataSource with a small WebUIMessageHandler that drives the same
// TorManager the toolbar globe menu uses. Hosted inside a translucent toolbar
// bubble (see MoltNetBubbleView); also resolves as a full tab for verification.

#ifndef CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_NET_UI_H_
#define CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_NET_UI_H_

#include "chrome/common/webui_url_constants.h"
#include "content/public/browser/web_ui_controller.h"
#include "content/public/browser/webui_config.h"
#include "content/public/common/url_constants.h"

class MoltNetUI;

class MoltNetUIConfig : public content::DefaultWebUIConfig<MoltNetUI> {
 public:
  MoltNetUIConfig()
      : DefaultWebUIConfig(content::kChromeUIScheme,
                           chrome::kChromeUIMoltNetHost) {}
};

class MoltNetUI : public content::WebUIController {
 public:
  explicit MoltNetUI(content::WebUI* web_ui);
  ~MoltNetUI() override;

  MoltNetUI(const MoltNetUI&) = delete;
  MoltNetUI& operator=(const MoltNetUI&) = delete;
};

#endif  // CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_NET_UI_H_
