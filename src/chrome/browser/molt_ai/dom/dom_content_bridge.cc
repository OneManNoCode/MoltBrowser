// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/dom/dom_content_bridge.h"

#include <utility>

#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/molt_ai/dom/dom_interpreter.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"

namespace molt_ai {

namespace {

// Runs on the thread pool: page HTML can be megabytes (S5).
StructuredPage ParseCapped(std::string html, std::string url) {
  return DOMInterpreter().ParseHTML(
      DOMInterpreter::CapHtmlForParsing(std::move(html)), url);
}

}  // namespace

DOMContentBridge::DOMContentBridge(content::WebContents* web_contents)
    : web_contents_(web_contents) {}

DOMContentBridge::~DOMContentBridge() = default;

void DOMContentBridge::GetPageHTML(HTMLCallback callback) {
  if (!web_contents_ || !web_contents_->GetPrimaryMainFrame()) {
    std::move(callback).Run(std::string());
    return;
  }

  // The page HTML, cut in the page to DOMInterpreter's cap (in UTF-16 code
  // units, so at least that many bytes survive) so a huge document isn't
  // copied through IPC only to be cut here.
  web_contents_->GetPrimaryMainFrame()->ExecuteJavaScriptInIsolatedWorld(
      u"(function(){var e=document.documentElement;"
      u"var h=e?e.outerHTML:'';return h.length>" +
          base::NumberToString16(DOMInterpreter::kMaxParseHtmlBytes) +
          u"?h.slice(0," +
          base::NumberToString16(DOMInterpreter::kMaxParseHtmlBytes) +
          u"):h;})()",
      base::BindOnce(
          [](HTMLCallback cb, base::Value result) {
            if (result.is_string()) {
              std::move(cb).Run(result.GetString());
            } else {
              std::move(cb).Run(std::string());
            }
          },
          std::move(callback)),
      content::ISOLATED_WORLD_ID_CONTENT_END);
}

void DOMContentBridge::GetPageText(TextCallback callback) {
  if (!web_contents_ || !web_contents_->GetPrimaryMainFrame()) {
    std::move(callback).Run(std::string());
    return;
  }

  web_contents_->GetPrimaryMainFrame()->ExecuteJavaScriptInIsolatedWorld(
      u"document.body ? document.body.innerText : ''",
      base::BindOnce(
          [](TextCallback cb, base::Value result) {
            if (result.is_string()) {
              std::move(cb).Run(result.GetString());
            } else {
              std::move(cb).Run(std::string());
            }
          },
          std::move(callback)),
      content::ISOLATED_WORLD_ID_CONTENT_END);
}

std::string DOMContentBridge::GetPageTitle() const {
  if (!web_contents_) {
    return std::string();
  }
  return base::UTF16ToUTF8(web_contents_->GetTitle());
}

std::string DOMContentBridge::GetPageURL() const {
  if (!web_contents_) {
    return std::string();
  }
  return web_contents_->GetLastCommittedURL().spec();
}

void DOMContentBridge::GetStructuredPage(StructuredPageCallback callback) {
  std::string url = GetPageURL();

  // Parse off the UI thread, at most kMaxParseHtmlBytes of it (S5). The
  // parse touches nothing but its arguments, so shutdown needn't wait for
  // it; |callback| runs back on this (UI) sequence.
  GetPageHTML(base::BindOnce(
      [](StructuredPageCallback cb, std::string page_url,
         const std::string& html) {
        base::ThreadPool::PostTaskAndReplyWithResult(
            FROM_HERE,
            {base::TaskPriority::USER_VISIBLE,
             base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
            base::BindOnce(&ParseCapped, html, std::move(page_url)),
            std::move(cb));
      },
      std::move(callback), std::move(url)));
}

void DOMContentBridge::GetElementAtPoint(int x, int y, TextCallback callback) {
  if (!web_contents_ || !web_contents_->GetPrimaryMainFrame()) {
    std::move(callback).Run(std::string());
    return;
  }

  // JavaScript to find element at coordinates and return its info
  std::u16string script = base::UTF8ToUTF16(
      "(() => {"
      "  const el = document.elementFromPoint(" +
      std::to_string(x) + ", " + std::to_string(y) +
      ");"
      "  if (!el) return '{}';"
      "  return JSON.stringify({"
      "    tag: el.tagName.toLowerCase(),"
      "    id: el.id || '',"
      "    className: el.className || '',"
      "    text: (el.textContent || '').substring(0, 200),"
      "    href: el.href || '',"
      "    type: el.type || '',"
      "    name: el.name || '',"
      "    value: el.value || '',"
      "    ariaLabel: el.getAttribute('aria-label') || ''"
      "  });"
      "})()");

  web_contents_->GetPrimaryMainFrame()->ExecuteJavaScriptInIsolatedWorld(
      script,
      base::BindOnce(
          [](TextCallback cb, base::Value result) {
            if (result.is_string()) {
              std::move(cb).Run(result.GetString());
            } else {
              std::move(cb).Run("{}");
            }
          },
          std::move(callback)),
      content::ISOLATED_WORLD_ID_CONTENT_END);
}

bool DOMContentBridge::IsPageReady() const {
  if (!web_contents_) {
    return false;
  }
  return !web_contents_->IsLoading();
}

}  // namespace molt_ai
