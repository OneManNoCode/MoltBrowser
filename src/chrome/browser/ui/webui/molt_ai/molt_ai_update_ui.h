// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// chrome://molt-ai-update/ (molt://update/) — in-app software update page.
// Shows the current vs. latest MoltBrowser version, release notes, and the
// "Check for updates" / "Update now" controls, driven by
// molt_ai::UpdateManager (GitHub Releases API).

#ifndef CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_UPDATE_UI_H_
#define CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_UPDATE_UI_H_

#include "chrome/common/webui_url_constants.h"
#include "content/public/browser/web_ui_controller.h"
#include "content/public/browser/webui_config.h"
#include "content/public/common/url_constants.h"

class MoltAIUpdateUI;

class MoltAIUpdateUIConfig
    : public content::DefaultWebUIConfig<MoltAIUpdateUI> {
 public:
  MoltAIUpdateUIConfig()
      : DefaultWebUIConfig(content::kChromeUIScheme,
                           chrome::kChromeUIMoltAIUpdateHost) {}
};

class MoltAIUpdateUI : public content::WebUIController {
 public:
  explicit MoltAIUpdateUI(content::WebUI* web_ui);
  ~MoltAIUpdateUI() override;

  MoltAIUpdateUI(const MoltAIUpdateUI&) = delete;
  MoltAIUpdateUI& operator=(const MoltAIUpdateUI&) = delete;
};

#endif  // CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_UPDATE_UI_H_
