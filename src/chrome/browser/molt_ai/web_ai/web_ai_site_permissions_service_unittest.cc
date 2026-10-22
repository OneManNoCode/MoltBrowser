// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/web_ai/web_ai_site_permissions_service.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_site_permissions.h"
#include "components/prefs/testing_pref_service.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace molt_ai::web_ai {
namespace {

url::Origin O(const char* spec) {
  return url::Origin::Create(GURL(spec));
}

class WebAISitePermissionsServiceTest : public testing::Test {
 protected:
  WebAISitePermissionsServiceTest() {
    prefs_.registry()->RegisterDictionaryPref(kSitesPref);
  }

  std::unique_ptr<WebAISitePermissions> NewService() {
    return std::make_unique<WebAISitePermissions>(&prefs_);
  }

  // Replaces the stored pref with |stored|.
  void Store(base::DictValue stored) {
    prefs_.SetDict(kSitesPref, std::move(stored));
  }

  TestingPrefServiceSimple prefs_;
};

TEST_F(WebAISitePermissionsServiceTest, AnswersSurviveARestart) {
  auto service = NewService();
  EXPECT_TRUE(service->persistent());
  service->SetAnswer(O("https://allowed.example"), SiteConsent::kAllowed);
  service->SetAnswer(O("https://blocked.example"), SiteConsent::kBlocked);
  for (int i = 0; i < 3; ++i) {
    service->RecordDismissal(O("https://paused.example"));
  }
  service->RecordDismissal(O("https://once.example"));
  service.reset();

  auto restarted = NewService();
  EXPECT_EQ(restarted->GetConsent(O("https://allowed.example")),
            SiteConsent::kAllowed);
  EXPECT_EQ(restarted->GetConsent(O("https://blocked.example")),
            SiteConsent::kBlocked);
  EXPECT_EQ(restarted->GetConsent(O("https://paused.example")),
            SiteConsent::kBlocked);
  EXPECT_EQ(restarted->GetConsent(O("https://once.example")),
            SiteConsent::kAsk);
  // The same origin however it was reached; other origins of the same site
  // are separate.
  EXPECT_EQ(restarted->GetConsent(O("https://allowed.example/some/page?q")),
            SiteConsent::kAllowed);
  EXPECT_EQ(restarted->GetConsent(O("http://allowed.example")),
            SiteConsent::kAsk);
  EXPECT_EQ(restarted->GetConsent(O("https://sub.allowed.example")),
            SiteConsent::kAsk);
  EXPECT_EQ(restarted->List().size(), 3u);
}

TEST_F(WebAISitePermissionsServiceTest, StoredFormat) {
  auto service = NewService();
  service->SetAnswer(O("https://a.example:8443"), SiteConsent::kAllowed);
  service->RecordDismissal(O("http://b.example"));
  const base::DictValue& stored = prefs_.GetDict(kSitesPref);
  EXPECT_EQ(stored.FindInt("version"), std::optional<int>(1));
  const base::DictValue* sites = stored.FindDict("sites");
  ASSERT_TRUE(sites);
  EXPECT_EQ(sites->size(), 2u);
  const base::DictValue* a = sites->FindDict("https://a.example:8443");
  ASSERT_TRUE(a);
  ASSERT_TRUE(a->FindString("answer"));
  EXPECT_EQ(*a->FindString("answer"), "allow");
  EXPECT_TRUE(a->FindDouble("answered"));
  EXPECT_FALSE(a->Find("dismissals"));
  const base::DictValue* b = sites->FindDict("http://b.example");
  ASSERT_TRUE(b);
  EXPECT_EQ(b->FindInt("dismissals"), std::optional<int>(1));
  EXPECT_TRUE(b->FindDouble("last_dismissal"));
  EXPECT_FALSE(b->Find("answer"));
}

// The pref file isn't trusted: anything that isn't a canonical http(s)
// origin with a sensible entry is dropped, and the next write cleans it.
TEST_F(WebAISitePermissionsServiceTest, DamagedPrefIsIgnored) {
  base::DictValue sites;
  const double t = 1.7e12;
  auto allow = [t]() {
    return base::DictValue().Set("answer", "allow").Set("answered", t);
  };
  sites.Set("https://good.example", allow());
  sites.Set("https://Good.example", allow());           // not canonical
  sites.Set("https://good.example:443", allow());       // default port
  sites.Set("https://good.example/", allow());          // a path
  sites.Set("ftp://good.example", allow());             // scheme
  sites.Set("chrome://settings", allow());
  sites.Set("", allow());
  sites.Set("https://string.example", "allow");         // not a dict
  sites.Set("https://empty.example", base::DictValue());
  sites.Set("https://weird.example",
            base::DictValue().Set("answer", 1).Set("dismissals", "3"));
  Store(base::DictValue().Set("version", 1).Set("sites", std::move(sites)));

  auto service = NewService();
  std::vector<SiteListEntry> rows = service->List();
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].site, "https://good.example");
  // Nothing was rewritten just by reading.
  EXPECT_EQ(prefs_.GetDict(kSitesPref).FindDict("sites")->size(), 10u);
  service->SetAnswer(O("https://new.example"), SiteConsent::kBlocked);
  EXPECT_EQ(prefs_.GetDict(kSitesPref).FindDict("sites")->size(), 2u);
}

TEST_F(WebAISitePermissionsServiceTest, UnknownVersionsAndShapes) {
  base::DictValue sites;
  sites.Set("https://a.example",
            base::DictValue().Set("answer", "block").Set("answered", 1.0));
  // No version: a damaged or foreign value; nothing is read.
  Store(base::DictValue().Set("sites", sites.Clone()));
  EXPECT_TRUE(NewService()->List().empty());
  Store(base::DictValue().Set("version", "1").Set("sites", sites.Clone()));
  EXPECT_TRUE(NewService()->List().empty());
  Store(base::DictValue().Set("version", 1).Set("sites", "x"));
  EXPECT_TRUE(NewService()->List().empty());
  // A later format keeps these fields' meaning; they are read.
  Store(base::DictValue()
            .Set("version", 2)
            .Set("sites", sites.Clone())
            .Set("something_new", true));
  EXPECT_EQ(NewService()->GetConsent(O("https://a.example")),
            SiteConsent::kBlocked);
}

TEST_F(WebAISitePermissionsServiceTest, IncognitoKeepsItInMemory) {
  WebAISitePermissions incognito(/*prefs=*/nullptr);
  EXPECT_FALSE(incognito.persistent());
  incognito.SetAnswer(O("https://a.example"), SiteConsent::kAllowed);
  EXPECT_EQ(incognito.GetConsent(O("https://a.example")),
            SiteConsent::kAllowed);
  EXPECT_TRUE(prefs_.GetDict(kSitesPref).empty());
}

TEST_F(WebAISitePermissionsServiceTest, OnlyHttpOriginsHaveAnswers) {
  auto service = NewService();
  for (const char* spec :
       {"file:///etc/passwd", "chrome://settings", "data:text/html,x",
        "about:blank", "javascript:alert(1)"}) {
    service->SetAnswer(O(spec), SiteConsent::kAllowed);
    service->RecordDismissal(O(spec));
    EXPECT_EQ(service->GetConsent(O(spec)), SiteConsent::kAsk) << spec;
  }
  service->SetAnswer(url::Origin(), SiteConsent::kAllowed);  // opaque
  EXPECT_EQ(service->GetConsent(url::Origin()), SiteConsent::kAsk);
  EXPECT_TRUE(service->List().empty());
  EXPECT_TRUE(prefs_.GetDict(kSitesPref).empty());
}

TEST_F(WebAISitePermissionsServiceTest, ObserversHearEveryChange) {
  auto service = NewService();
  int changes = 0;
  base::CallbackListSubscription subscription = service->AddObserver(
      base::BindRepeating([](int* n) { ++*n; }, &changes));
  service->SetAnswer(O("https://a.example"), SiteConsent::kAllowed);
  EXPECT_EQ(changes, 1);
  // Nothing to remove, or a close for an answered site: no change.
  EXPECT_FALSE(service->Remove("https://unknown.example"));
  service->RecordDismissal(O("https://a.example"));
  EXPECT_EQ(changes, 1);
  EXPECT_TRUE(service->Remove("https://a.example"));
  EXPECT_EQ(changes, 2);
  service->RemoveAll();
  EXPECT_EQ(changes, 2);
  service->RecordDismissal(O("https://b.example"));
  service->RemoveAll();
  EXPECT_EQ(changes, 4);
}

TEST_F(WebAISitePermissionsServiceTest, RemoveTakesOnlyListedKeys) {
  auto service = NewService();
  service->SetAnswer(O("https://a.example"), SiteConsent::kBlocked);
  for (const char* other :
       {"https://A.example", "a.example", "https://a.example/",
        "<img src=x onerror=alert(1)>", ""}) {
    EXPECT_FALSE(service->Remove(other)) << other;
  }
  EXPECT_TRUE(service->Remove(service->List()[0].site));
  EXPECT_EQ(service->GetConsent(O("https://a.example")), SiteConsent::kAsk);
}

TEST_F(WebAISitePermissionsServiceTest, ClearBrowsingData) {
  auto service = NewService();
  service->SetAnswer(O("https://a.example"), SiteConsent::kAllowed);
  service->SetAnswer(O("https://b.example"), SiteConsent::kAllowed);
  for (int i = 0; i < 3; ++i) {
    service->RecordDismissal(O("https://a2.example"));
    service->RecordDismissal(O("https://b2.example"));
  }
  // Clearing for one site (the filter picks URLs).
  const base::RepeatingCallback<bool(const GURL&)> only_a =
      base::BindRepeating([](const GURL& url) {
        return url.host() == "a.example" || url.host() == "a2.example";
      });
  service->RemoveAnswersBetween(base::Time(), base::Time::Max(), only_a);
  EXPECT_EQ(service->GetConsent(O("https://a.example")), SiteConsent::kAsk);
  EXPECT_EQ(service->GetConsent(O("https://b.example")),
            SiteConsent::kAllowed);
  // "Site settings" doesn't touch the prompt's pauses...
  EXPECT_EQ(service->GetConsent(O("https://a2.example")),
            SiteConsent::kBlocked);
  // ...history does, and not the answers.
  service->RemovePromptHistory(base::RepeatingCallback<bool(const GURL&)>());
  EXPECT_EQ(service->GetConsent(O("https://a2.example")), SiteConsent::kAsk);
  EXPECT_EQ(service->GetConsent(O("https://b2.example")), SiteConsent::kAsk);
  EXPECT_EQ(service->GetConsent(O("https://b.example")),
            SiteConsent::kAllowed);
  // A range that ended before the answer keeps it.
  service->RemoveAnswersBetween(base::Time(),
                                base::Time::FromMillisecondsSinceUnixEpoch(1),
                                base::RepeatingCallback<bool(const GURL&)>());
  EXPECT_EQ(service->GetConsent(O("https://b.example")),
            SiteConsent::kAllowed);
  // Profile reset.
  service->RemoveAll();
  EXPECT_TRUE(service->List().empty());
  EXPECT_TRUE(
      NewService()->List().empty());  // and on disk
}

// A site closed more often than the stored count's cap still gets a new
// pause, and it is written.
TEST_F(WebAISitePermissionsServiceTest, PauseRenewedPastTheCountsCap) {
  base::DictValue sites;
  sites.Set("https://a.example", base::DictValue()
                                     .Set("dismissals", 100)
                                     .Set("last_dismissal", 1000.0));
  Store(base::DictValue().Set("version", 1).Set("sites", std::move(sites)));
  auto service = NewService();
  EXPECT_EQ(service->GetConsent(O("https://a.example")), SiteConsent::kAsk);
  service->RecordDismissal(O("https://a.example"));
  EXPECT_EQ(service->GetConsent(O("https://a.example")),
            SiteConsent::kBlocked);
  EXPECT_EQ(NewService()->GetConsent(O("https://a.example")),
            SiteConsent::kBlocked);
}

TEST_F(WebAISitePermissionsServiceTest, NoWritesAfterShutdown) {
  auto service = NewService();
  service->Shutdown();
  service->SetAnswer(O("https://a.example"), SiteConsent::kAllowed);
  EXPECT_TRUE(prefs_.GetDict(kSitesPref).empty());
}

}  // namespace
}  // namespace molt_ai::web_ai
