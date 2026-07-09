// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// chrome://molt-home/ — MoltBrowser new-tab / home page.
// A local, self-contained "Liquid Glass" landing page: a search field that
// routes to the web or the AI, plus quick-access widget cards. Served as
// inline HTML (no remote dependency), so the new tab loads instantly and
// privately.

#ifndef CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_HOME_UI_H_
#define CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_HOME_UI_H_

#include "chrome/common/webui_url_constants.h"
#include "content/public/browser/web_ui_controller.h"
#include "content/public/browser/webui_config.h"
#include "content/public/common/url_constants.h"

class MoltHomeUI;

class MoltHomeUIConfig : public content::DefaultWebUIConfig<MoltHomeUI> {
 public:
  MoltHomeUIConfig()
      : DefaultWebUIConfig(content::kChromeUIScheme,
                           chrome::kChromeUIMoltHomeHost) {}
};

class MoltHomeUI : public content::WebUIController {
 public:
  explicit MoltHomeUI(content::WebUI* web_ui);
  ~MoltHomeUI() override;

  MoltHomeUI(const MoltHomeUI&) = delete;
  MoltHomeUI& operator=(const MoltHomeUI&) = delete;
};

#endif  // CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_HOME_UI_H_
