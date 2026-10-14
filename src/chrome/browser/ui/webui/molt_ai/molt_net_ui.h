// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// chrome://molt-net/ — MoltNet (Tor) control surface.
// A local, self-contained "Liquid Glass" page: the routing status of the
// window it's shown in (MoltNet window: "Private via Tor"; regular window:
// "not routed"), on/off + "new Tor window" + New Identity, the Tor route
// (from Tor's control port) and the exit-country picker. Served as inline
// HTML via URLDataSource with MoltNetHandler. Hosted inside a translucent
// toolbar bubble (see MoltNetBubbleView); also resolves as a full tab.

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
