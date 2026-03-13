// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// chrome://molt-ai-chat/ — AI chat sidebar interface

#ifndef CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_CHAT_UI_H_
#define CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_CHAT_UI_H_

#include "chrome/common/webui_url_constants.h"
#include "content/public/browser/web_ui_controller.h"
#include "content/public/browser/webui_config.h"
#include "content/public/common/url_constants.h"

class MoltAIChatUI;

class MoltAIChatUIConfig
    : public content::DefaultWebUIConfig<MoltAIChatUI> {
 public:
  MoltAIChatUIConfig()
      : DefaultWebUIConfig(content::kChromeUIScheme,
                           chrome::kChromeUIMoltAIChatHost) {}
};

class MoltAIChatUI : public content::WebUIController {
 public:
  explicit MoltAIChatUI(content::WebUI* web_ui);
  ~MoltAIChatUI() override;

  MoltAIChatUI(const MoltAIChatUI&) = delete;
  MoltAIChatUI& operator=(const MoltAIChatUI&) = delete;
};

#endif  // CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_CHAT_UI_H_
