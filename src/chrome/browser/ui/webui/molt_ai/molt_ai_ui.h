// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// chrome://molt-ai/ — AI prompt interface
// Handles AI queries from the omnibox (@ai prefix)

#ifndef CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_UI_H_
#define CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_UI_H_

#include "chrome/common/webui_url_constants.h"
#include "content/public/browser/web_ui_controller.h"
#include "content/public/browser/webui_config.h"
#include "content/public/common/url_constants.h"

class MoltAIUI;

class MoltAIUIConfig : public content::DefaultWebUIConfig<MoltAIUI> {
 public:
  MoltAIUIConfig()
      : DefaultWebUIConfig(content::kChromeUIScheme,
                           chrome::kChromeUIMoltAIHost) {}
};

class MoltAIUI : public content::WebUIController {
 public:
  explicit MoltAIUI(content::WebUI* web_ui);
  ~MoltAIUI() override;

  MoltAIUI(const MoltAIUI&) = delete;
  MoltAIUI& operator=(const MoltAIUI&) = delete;
};

#endif  // CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_UI_H_
