// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// chrome://molt-ai-settings/ — AI settings configuration page

#ifndef CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_SETTINGS_UI_H_
#define CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_SETTINGS_UI_H_

#include "chrome/common/webui_url_constants.h"
#include "content/public/browser/web_ui_controller.h"
#include "content/public/browser/webui_config.h"
#include "content/public/common/url_constants.h"

class MoltAISettingsUI;

class MoltAISettingsUIConfig
    : public content::DefaultWebUIConfig<MoltAISettingsUI> {
 public:
  MoltAISettingsUIConfig()
      : DefaultWebUIConfig(content::kChromeUIScheme,
                           chrome::kChromeUIMoltAISettingsHost) {}
};

class MoltAISettingsUI : public content::WebUIController {
 public:
  explicit MoltAISettingsUI(content::WebUI* web_ui);
  ~MoltAISettingsUI() override;

  MoltAISettingsUI(const MoltAISettingsUI&) = delete;
  MoltAISettingsUI& operator=(const MoltAISettingsUI&) = delete;
};

#endif  // CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_SETTINGS_UI_H_
