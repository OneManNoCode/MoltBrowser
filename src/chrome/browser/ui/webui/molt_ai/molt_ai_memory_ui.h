// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// chrome://molt-memory/ — Personal Vector Memory manager UI.
// Reachable as molt://memory/ in the address bar.

#ifndef CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_MEMORY_UI_H_
#define CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_MEMORY_UI_H_

#include "chrome/common/webui_url_constants.h"
#include "content/public/browser/web_ui_controller.h"
#include "content/public/browser/webui_config.h"
#include "content/public/common/url_constants.h"

class MoltAIMemoryUI;

class MoltAIMemoryUIConfig
    : public content::DefaultWebUIConfig<MoltAIMemoryUI> {
 public:
  MoltAIMemoryUIConfig()
      : DefaultWebUIConfig(content::kChromeUIScheme,
                            chrome::kChromeUIMoltAIMemoryHost) {}
};

class MoltAIMemoryUI : public content::WebUIController {
 public:
  explicit MoltAIMemoryUI(content::WebUI* web_ui);
  ~MoltAIMemoryUI() override;

  MoltAIMemoryUI(const MoltAIMemoryUI&) = delete;
  MoltAIMemoryUI& operator=(const MoltAIMemoryUI&) = delete;
};

#endif  // CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_MEMORY_UI_H_
