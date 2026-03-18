// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// chrome://molt-ai-agent/ — Agent testing and automation interface

#ifndef CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_AGENT_UI_H_
#define CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_AGENT_UI_H_

#include "chrome/common/webui_url_constants.h"
#include "content/public/browser/web_ui_controller.h"
#include "content/public/browser/webui_config.h"
#include "content/public/common/url_constants.h"

class MoltAIAgentUI;

class MoltAIAgentUIConfig
    : public content::DefaultWebUIConfig<MoltAIAgentUI> {
 public:
  MoltAIAgentUIConfig()
      : DefaultWebUIConfig(content::kChromeUIScheme,
                           chrome::kChromeUIMoltAIAgentHost) {}
};

class MoltAIAgentUI : public content::WebUIController {
 public:
  explicit MoltAIAgentUI(content::WebUI* web_ui);
  ~MoltAIAgentUI() override;

  MoltAIAgentUI(const MoltAIAgentUI&) = delete;
  MoltAIAgentUI& operator=(const MoltAIAgentUI&) = delete;
};

#endif  // CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_AGENT_UI_H_
