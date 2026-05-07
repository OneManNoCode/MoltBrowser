// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// chrome://molt-ai-automation/ — Web automation manager UI.
// Reachable as molt://ai-automation/ in the address bar.

#ifndef CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_AUTOMATION_UI_H_
#define CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_AUTOMATION_UI_H_

#include "chrome/common/webui_url_constants.h"
#include "content/public/browser/web_ui_controller.h"
#include "content/public/browser/webui_config.h"
#include "content/public/common/url_constants.h"

class MoltAIAutomationUI;

class MoltAIAutomationUIConfig
    : public content::DefaultWebUIConfig<MoltAIAutomationUI> {
 public:
  MoltAIAutomationUIConfig()
      : DefaultWebUIConfig(content::kChromeUIScheme,
                           chrome::kChromeUIMoltAIAutomationHost) {}
};

class MoltAIAutomationUI : public content::WebUIController {
 public:
  explicit MoltAIAutomationUI(content::WebUI* web_ui);
  ~MoltAIAutomationUI() override;

  MoltAIAutomationUI(const MoltAIAutomationUI&) = delete;
  MoltAIAutomationUI& operator=(const MoltAIAutomationUI&) = delete;
};

#endif  // CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_AUTOMATION_UI_H_
