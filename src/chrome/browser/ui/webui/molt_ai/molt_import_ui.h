// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// chrome://molt-import/ — Import (browser migration) control surface.
// A local, self-contained "Liquid Glass" page: a detected-browser list, an
// "include saved passwords" toggle (default off), and import actions. Served
// as inline HTML via URLDataSource with a small WebUIMessageHandler that drives
// the same molt_ai::BrowserImporter the settings import section uses. Hosted
// inside a translucent toolbar bubble (see MoltImportBubbleView); also resolves
// as a full tab for verification. Sibling of MoltNetUI.

#ifndef CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_IMPORT_UI_H_
#define CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_IMPORT_UI_H_

#include "chrome/common/webui_url_constants.h"
#include "content/public/browser/web_ui_controller.h"
#include "content/public/browser/webui_config.h"
#include "content/public/common/url_constants.h"

class MoltImportUI;

class MoltImportUIConfig : public content::DefaultWebUIConfig<MoltImportUI> {
 public:
  MoltImportUIConfig()
      : DefaultWebUIConfig(content::kChromeUIScheme,
                           chrome::kChromeUIMoltImportHost) {}
};

class MoltImportUI : public content::WebUIController {
 public:
  explicit MoltImportUI(content::WebUI* web_ui);
  ~MoltImportUI() override;

  MoltImportUI(const MoltImportUI&) = delete;
  MoltImportUI& operator=(const MoltImportUI&) = delete;
};

#endif  // CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_IMPORT_UI_H_
