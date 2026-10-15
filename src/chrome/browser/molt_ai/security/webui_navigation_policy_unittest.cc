// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/security/webui_navigation_policy.h"

#include <optional>

#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "url/url_util.h"

namespace molt_ai::security {
namespace {

// chrome://, devtools:// etc. are standard schemes in the browser (content/
// and chrome/ register them); without that their origins would be opaque.
class WebUINavigationPolicyTest : public testing::Test {
 protected:
  WebUINavigationPolicyTest() {
    for (const char* scheme : {"chrome", "chrome-untrusted", "devtools",
                               "chrome-extension", "molt"}) {
      url::AddStandardScheme(scheme, url::SCHEME_WITH_HOST);
    }
  }

 private:
  url::ScopedSchemeRegistryForTests scheme_registry_;
};

url::Origin O(const char* url) {
  return url::Origin::Create(GURL(url));
}

bool Block(const char* url,
           std::optional<url::Origin> initiator,
           bool renderer_initiated = true,
           bool history_or_reload = false) {
  return ShouldBlockWebInitiatedWebUINavigation(
      GURL(url), renderer_initiated, history_or_reload, initiator);
}

// What a web page could reach through the molt:// alias before: the
// rewritten chrome:// URL (noopener popup, OpenURL, frame proxy) or the
// molt:// URL itself (same-process link).
TEST_F(WebUINavigationPolicyTest, WebPagesCannotOpenWebUI) {
  const auto web = O("https://evil.example/");
  EXPECT_TRUE(Block("chrome://molt-ai-settings/", web));
  EXPECT_TRUE(Block("chrome://molt-ai-settings/?section=providers", web));
  EXPECT_TRUE(Block("chrome://settings/resetProfileSettings", web));
  EXPECT_TRUE(Block("molt://ai-settings/", web));
  EXPECT_TRUE(Block("molt://ai/?q=hello", web));
  EXPECT_TRUE(Block("chrome://molt-ai/", O("http://192.168.0.1/")));
  EXPECT_TRUE(Block("chrome://molt-ai/", O("file:///tmp/x.html")));
  EXPECT_TRUE(
      Block("chrome://molt-ai/", O("chrome-extension://abcdefghijklmnop/")));
  // chrome-untrusted:// hosts untrusted content; it doesn't get chrome://.
  EXPECT_TRUE(Block("chrome://molt-ai/", O("chrome-untrusted://print/")));
}

TEST_F(WebUINavigationPolicyTest, OpaqueOrMissingInitiatorIsBlocked) {
  EXPECT_TRUE(Block("chrome://molt-ai/", url::Origin()));
  EXPECT_TRUE(Block("chrome://molt-ai/", O("data:text/html,x")));
  EXPECT_TRUE(Block("molt://ai/", std::nullopt));
}

TEST_F(WebUINavigationPolicyTest, WebUIPagesMayLinkToEachOther) {
  EXPECT_FALSE(Block("chrome://molt-ai-settings/?section=import",
                     O("chrome://settings/")));
  EXPECT_FALSE(Block("molt://ai-settings/", O("chrome://molt-home/")));
  EXPECT_FALSE(Block("chrome://molt-ai/", O("chrome://molt-ai-chat/")));
  EXPECT_FALSE(Block("chrome://inspect/", O("devtools://devtools/")));
}

TEST_F(WebUINavigationPolicyTest, BrowserInitiatedHistoryAndReloadAllowed) {
  const auto web = O("https://evil.example/");
  // Omnibox, bookmarks, MoltBrowser's own UI.
  EXPECT_FALSE(Block("chrome://molt-ai/", web, /*renderer_initiated=*/false));
  EXPECT_FALSE(Block("molt://ai/", std::nullopt, /*renderer_initiated=*/false));
  // history.back() into an entry the user opened themselves; reloads.
  EXPECT_FALSE(Block("chrome://molt-ai/", web, true,
                     /*history_or_reload=*/true));
}

TEST_F(WebUINavigationPolicyTest, OtherSchemesUntouched) {
  const auto web = O("https://evil.example/");
  EXPECT_FALSE(Block("https://example.com/", web));
  EXPECT_FALSE(Block("about:blank", web));
  EXPECT_FALSE(Block("chrome-untrusted://print/", web));  // not our concern
  EXPECT_FALSE(Block("https://chrome.example/", web));
  EXPECT_FALSE(Block("https://molt.example/", web));
}

}  // namespace
}  // namespace molt_ai::security
