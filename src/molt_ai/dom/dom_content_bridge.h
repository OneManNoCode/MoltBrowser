// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// DOMContentBridge connects MoltBrowser's DOMInterpreter to Chromium's
// content layer. It extracts page HTML, DOM structure, and element info
// from live browser tabs via content::WebContents.

#ifndef CHROME_BROWSER_MOLT_AI_DOM_DOM_CONTENT_BRIDGE_H_
#define CHROME_BROWSER_MOLT_AI_DOM_DOM_CONTENT_BRIDGE_H_

#include <string>
#include <functional>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"

namespace content {
class WebContents;
}  // namespace content

namespace molt_ai {

struct StructuredPage;

// DOMContentBridge provides methods to extract page content from
// live Chromium browser tabs for AI processing.
//
// Usage:
//   DOMContentBridge bridge(web_contents);
//   bridge.GetPageHTML(base::BindOnce(&OnHTMLReceived));
//
// All extraction methods are asynchronous because they require
// JavaScript execution in the renderer process.
class DOMContentBridge {
 public:
  using HTMLCallback = base::OnceCallback<void(const std::string& html)>;
  using TextCallback = base::OnceCallback<void(const std::string& text)>;
  using StructuredPageCallback =
      base::OnceCallback<void(const StructuredPage& page)>;

  explicit DOMContentBridge(content::WebContents* web_contents);
  ~DOMContentBridge();

  DOMContentBridge(const DOMContentBridge&) = delete;
  DOMContentBridge& operator=(const DOMContentBridge&) = delete;

  // Get the full page HTML (document.documentElement.outerHTML)
  void GetPageHTML(HTMLCallback callback);

  // Get visible text content only (document.body.innerText)
  void GetPageText(TextCallback callback);

  // Get the page title
  std::string GetPageTitle() const;

  // Get the current URL
  std::string GetPageURL() const;

  // Get structured page data (parses HTML through DOMInterpreter)
  void GetStructuredPage(StructuredPageCallback callback);

  // Execute JavaScript to extract specific element data
  void GetElementAtPoint(int x, int y, TextCallback callback);

  // Check if the page has finished loading
  bool IsPageReady() const;

 private:
  raw_ptr<content::WebContents> web_contents_;
};

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_DOM_DOM_CONTENT_BRIDGE_H_
