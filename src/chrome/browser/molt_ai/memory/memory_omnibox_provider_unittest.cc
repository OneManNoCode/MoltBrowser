// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The @memory omnibox shortcut in the omnibox: MoltMemoryProvider's rows in
// each kind of profile and Memory state, its two-step answers, that page
// text only ever becomes match text, and the autocomplete controller's gate
// that keeps "@memory ..." and "@ai ..." from every other provider
// (docs/design/SEMANTIC_SEARCH.md).

#include "components/omnibox/browser/molt_memory_provider.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/task_environment.h"
#include "chrome/common/webui_url_constants.h"
#include "components/omnibox/browser/autocomplete_controller.h"
#include "components/omnibox/browser/autocomplete_input.h"
#include "components/omnibox/browser/autocomplete_match.h"
#include "components/omnibox/browser/autocomplete_match_type.h"
#include "components/omnibox/browser/autocomplete_provider_listener.h"
#include "components/omnibox/browser/fake_autocomplete_controller.h"
#include "components/omnibox/browser/fake_autocomplete_provider_client.h"
#include "components/omnibox/browser/mock_autocomplete_provider_client.h"
#include "components/omnibox/browser/molt_memory_query.h"
#include "components/omnibox/browser/molt_memory_search.h"
#include "components/omnibox/browser/omnibox_prefs.h"
#include "components/omnibox/browser/test_scheme_classifier.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/testing_pref_service.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/metrics_proto/omnibox_event.pb.h"
#include "url/gurl.h"

namespace {

using testing::Return;
using testing::ReturnRef;

// A profile's memory, answered by the test.
class FakeMemorySearch : public MoltMemorySearch {
 public:
  struct Request {
    std::string query;
    size_t max_results = 0;
    CancelFlag cancelled;
    ResultsCallback callback;
  };

  bool IsMemoryEnabled() const override { return enabled; }
  bool IsMemoryPaused() const override { return paused; }
  void Search(const std::string& query,
              size_t max_results,
              CancelFlag cancelled,
              ResultsCallback callback) override {
    requests.push_back(
        {query, max_results, std::move(cancelled), std::move(callback)});
  }

  bool enabled = true;
  bool paused = false;
  std::vector<Request> requests;
};

class CountingListener : public AutocompleteProviderListener {
 public:
  void OnProviderUpdate(bool updated_matches,
                        const AutocompleteProvider* provider) override {
    ++updates;
  }
  int updates = 0;
};

MoltMemorySearch::Results Hits(
    std::vector<molt_memory::MemoryHit> hits,
    bool final,
    bool semantic = false) {
  MoltMemorySearch::Results r;
  r.hits = std::move(hits);
  r.final = final;
  r.semantic = semantic;
  return r;
}

class MoltMemoryProviderTest : public testing::Test {
 protected:
  void SetUp() override {
    ON_CALL(client_, GetPrefs()).WillByDefault(Return(&prefs_));
    ON_CALL(client_, IsOffTheRecord()).WillByDefault(Return(false));
    ON_CALL(client_, IsGuestSession()).WillByDefault(Return(false));
    ON_CALL(client_, GetSchemeClassifier())
        .WillByDefault(ReturnRef(classifier_));
    MoltMemorySearch::Register(&prefs_, &search_);
    provider_ = base::MakeRefCounted<MoltMemoryProvider>(&client_, &listener_);
  }

  void TearDown() override { MoltMemorySearch::Unregister(&prefs_, &search_); }

  AutocompleteInput Input(const std::u16string& text, bool omit_async = false) {
    AutocompleteInput input(text, metrics::OmniboxEventProto::OTHER,
                            classifier_);
    input.set_omit_asynchronous_matches(omit_async);
    return input;
  }

  const ACMatches& Start(const std::u16string& text) {
    provider_->Start(Input(text), /*minimal_changes=*/false);
    return provider_->matches();
  }

  base::test::TaskEnvironment task_environment_;
  TestingPrefServiceSimple prefs_;
  TestSchemeClassifier classifier_;
  testing::NiceMock<MockAutocompleteProviderClient> client_;
  CountingListener listener_;
  FakeMemorySearch search_;
  scoped_refptr<MoltMemoryProvider> provider_;
};

TEST_F(MoltMemoryProviderTest, MemoryPageUrlIsTheWebUi) {
  EXPECT_EQ(std::string(molt_memory::kMemoryPageUrl),
            chrome::kChromeUIMoltAIMemoryURL);
  EXPECT_TRUE(GURL(molt_memory::MemorySearchUrl("miso soup")).is_valid());
  EXPECT_EQ(GURL(molt_memory::MemorySearchUrl("miso soup")).host(),
            chrome::kChromeUIMoltAIMemoryHost);
}

// Incognito and MoltNet (off the record) and the Guest session: nothing at
// all, not even the hint, even if a memory were registered for them.
TEST_F(MoltMemoryProviderTest, NothingOffTheRecordOrInGuest) {
  for (bool guest : {false, true}) {
    ON_CALL(client_, IsOffTheRecord()).WillByDefault(Return(!guest));
    ON_CALL(client_, IsGuestSession()).WillByDefault(Return(guest));
    for (const char16_t* text :
         {u"@memory miso", u"@memory", u"@mem", u"@", u"miso"}) {
      EXPECT_TRUE(Start(text).empty()) << guest;
      EXPECT_TRUE(provider_->done());
    }
  }
  EXPECT_TRUE(search_.requests.empty());
}

// A profile without memory (its PrefService has none registered).
TEST_F(MoltMemoryProviderTest, NothingWithoutMemory) {
  TestingPrefServiceSimple other_prefs;
  ON_CALL(client_, GetPrefs()).WillByDefault(Return(&other_prefs));
  EXPECT_TRUE(Start(u"@memory miso").empty());
  EXPECT_TRUE(Start(u"@mem").empty());
  EXPECT_TRUE(search_.requests.empty());
}

TEST_F(MoltMemoryProviderTest, MemoryOffOrPausedSaysSoAndNeverSearches) {
  search_.enabled = false;
  const ACMatches& off = Start(u"@memory miso");
  ASSERT_EQ(off.size(), 1u);
  EXPECT_EQ(off[0].destination_url, GURL(molt_memory::kMemoryPageUrl));
  EXPECT_TRUE(off[0].allowed_to_be_default_match);
  EXPECT_NE(off[0].contents.find(u"off"), std::u16string::npos);
  EXPECT_TRUE(Start(u"@mem").empty()) << "no hint while off";

  search_.enabled = true;
  search_.paused = true;
  const ACMatches& paused = Start(u"@memory miso");
  ASSERT_EQ(paused.size(), 1u);
  EXPECT_NE(paused[0].contents.find(u"paused"), std::u16string::npos);
  // The words never go into the URL.
  EXPECT_EQ(paused[0].destination_url, GURL(molt_memory::kMemoryPageUrl));
  EXPECT_TRUE(search_.requests.empty());
}

TEST_F(MoltMemoryProviderTest, HintsAtTheShortcut) {
  const ACMatches& hint = Start(u"@mem");
  ASSERT_EQ(hint.size(), 1u);
  EXPECT_FALSE(hint[0].allowed_to_be_default_match);
  EXPECT_EQ(hint[0].fill_into_edit, u"@memory ");
  EXPECT_EQ(hint[0].destination_url, GURL(molt_memory::kMemoryPageUrl));

  const ACMatches& alone = Start(u"@memory");
  ASSERT_EQ(alone.size(), 1u);
  EXPECT_TRUE(alone[0].allowed_to_be_default_match);  // Enter opens the page

  EXPECT_TRUE(Start(u"miso soup").empty());
  EXPECT_TRUE(Start(u"@ai hello").empty());
  EXPECT_TRUE(search_.requests.empty());
}

// The search row at once; keyword hits, then hits by meaning.
TEST_F(MoltMemoryProviderTest, SearchRowThenHitsThenHitsByMeaning) {
  const ACMatches& first = Start(u"@memory  miso   soup");
  ASSERT_EQ(first.size(), 1u);
  EXPECT_EQ(first[0].destination_url,
            GURL("chrome://molt-memory/?q=miso%20soup"));
  EXPECT_TRUE(first[0].allowed_to_be_default_match);
  EXPECT_EQ(first[0].relevance, molt_memory::kSearchRowRelevance);
  EXPECT_FALSE(provider_->done());
  ASSERT_EQ(search_.requests.size(), 1u);
  EXPECT_EQ(search_.requests[0].query, "miso soup");

  search_.requests[0].callback.Run(Hits(
      {{"https://a.example/miso", "Miso soup"},
       {"javascript:alert(1)", "evil"},
       {"https://b.example/", ""}},
      /*final=*/false));
  EXPECT_EQ(listener_.updates, 1);
  EXPECT_FALSE(provider_->done());
  const ACMatches& keyword = provider_->matches();
  ASSERT_EQ(keyword.size(), 3u);  // the javascript: page is never a row
  EXPECT_EQ(keyword[1].destination_url, GURL("https://a.example/miso"));
  EXPECT_EQ(keyword[1].type, AutocompleteMatchType::HISTORY_BODY);
  EXPECT_FALSE(keyword[1].allowed_to_be_default_match);
  EXPECT_LT(keyword[1].relevance, keyword[0].relevance);
  EXPECT_EQ(keyword[1].description, u"Miso soup");
  EXPECT_EQ(keyword[2].destination_url, GURL("https://b.example/"));
  for (const AutocompleteMatch& m : keyword) {
    EXPECT_TRUE(m.destination_url.SchemeIs("chrome") ||
                m.destination_url.SchemeIsHTTPOrHTTPS());
  }

  search_.requests[0].callback.Run(
      Hits({{"https://c.example/ramen", "Ramen"}}, /*final=*/true,
           /*semantic=*/true));
  EXPECT_EQ(listener_.updates, 2);
  EXPECT_TRUE(provider_->done());
  ASSERT_EQ(provider_->matches().size(), 2u);
  EXPECT_EQ(provider_->matches()[1].destination_url,
            GURL("https://c.example/ramen"));
}

// Page titles are text: never parsed, markup and controls stay inert; the
// query's words are classified as matches.
TEST_F(MoltMemoryProviderTest, TitlesOnlyBecomeClassifiedText) {
  Start(u"@memory miso");
  search_.requests[0].callback.Run(Hits(
      {{"https://a.example/x",
        "<img src=x onerror=alert(1)> Miso\n\u202Esoup"}},
      /*final=*/true));
  ASSERT_EQ(provider_->matches().size(), 2u);
  const AutocompleteMatch& m = provider_->matches()[1];
  EXPECT_EQ(m.description, u"<img src=x onerror=alert(1)> Miso soup");
  bool bold = false;
  for (const ACMatchClassification& c : m.description_class) {
    bold |= (c.style & ACMatchClassification::MATCH) != 0;
  }
  EXPECT_TRUE(bold);
}

// Stop (the stop timer, Escape) and newer input drop what comes later, and
// tell the memory worker to skip a search not yet run.
TEST_F(MoltMemoryProviderTest, LateResultsAreDropped) {
  Start(u"@memory miso");
  ASSERT_EQ(search_.requests.size(), 1u);
  provider_->Stop(AutocompleteStopReason::kInactivity);
  EXPECT_TRUE(provider_->done());
  EXPECT_TRUE(search_.requests[0].cancelled->load());
  search_.requests[0].callback.Run(
      Hits({{"https://a.example/", "A"}}, /*final=*/true, true));
  EXPECT_EQ(listener_.updates, 0);
  EXPECT_EQ(provider_->matches().size(), 1u);

  Start(u"@memory mi");
  Start(u"@memory mis");
  ASSERT_EQ(search_.requests.size(), 3u);
  EXPECT_TRUE(search_.requests[1].cancelled->load());
  EXPECT_FALSE(search_.requests[2].cancelled->load());
  search_.requests[1].callback.Run(
      Hits({{"https://a.example/", "A"}}, /*final=*/true));
  EXPECT_EQ(listener_.updates, 0);
}

// The classifier (paste-and-go, Enter without the popup) is synchronous:
// the search row, no search.
TEST_F(MoltMemoryProviderTest, SynchronousInputGetsTheSearchRowOnly) {
  provider_->Start(Input(u"@memory miso", /*omit_async=*/true), false);
  ASSERT_EQ(provider_->matches().size(), 1u);
  EXPECT_EQ(provider_->matches()[0].destination_url,
            GURL("chrome://molt-memory/?q=miso"));
  EXPECT_TRUE(provider_->done());
  EXPECT_TRUE(search_.requests.empty());
}

TEST(MoltMemorySearchRegistryTest, FoundOnlyUnderItsOwnPrefs) {
  TestingPrefServiceSimple regular;
  TestingPrefServiceSimple off_the_record;
  FakeMemorySearch search;
  EXPECT_EQ(MoltMemorySearch::ForPrefs(&regular), nullptr);
  MoltMemorySearch::Register(&regular, &search);
  EXPECT_EQ(MoltMemorySearch::ForPrefs(&regular), &search);
  EXPECT_EQ(MoltMemorySearch::ForPrefs(&off_the_record), nullptr);
  EXPECT_EQ(MoltMemorySearch::ForPrefs(nullptr), nullptr);
  MoltMemorySearch::Unregister(&regular, &search);
  EXPECT_EQ(MoltMemorySearch::ForPrefs(&regular), nullptr);
}

// ---- The controller's gate --------------------------------------------------

// "@memory ..." and "@ai ..." reach no provider but their own: not the
// search provider (suggest requests), the document provider, extensions or
// any other. Here the controller has neither of those, so nothing runs.
TEST(MoltMemoryControllerGateTest, ShortcutInputReachesNoOtherProvider) {
  base::test::TaskEnvironment task_environment(
      base::test::TaskEnvironment::TimeSource::MOCK_TIME);
  FakeAutocompleteController controller(&task_environment);
  ASSERT_FALSE(controller.providers().empty());

  for (const char16_t* text :
       {u"@memory miso soup", u"@memory", u"  @MEMORY x",
        u"@ai what is on my calendar", u"@AI ", u"@ai\u3000x"}) {
    controller.input_ = FakeAutocompleteController::CreateInput(text);
    for (const auto& provider : controller.providers()) {
      EXPECT_FALSE(controller.ShouldRunProvider(provider.get()))
          << provider->GetName();
    }
  }
  // Other text, including the keywords' prefixes and "?" (a web search),
  // runs them as before.
  for (const char16_t* text : {u"@memo", u"@memoryx", u"miso soup", u"@ai",
                               u"@aim high", u"?what is gravity"}) {
    controller.input_ = FakeAutocompleteController::CreateInput(text);
    bool search_runs = false;
    for (const auto& provider : controller.providers()) {
      if (provider->type() == AutocompleteProvider::TYPE_SEARCH) {
        search_runs = controller.ShouldRunProvider(provider.get());
      }
    }
    EXPECT_TRUE(search_runs);
  }
}

// Starting local-only input stops what the other providers still do for
// the text typed before it, so none of them updates the popup later.
TEST(MoltMemoryControllerGateTest, ShortcutInputStopsTheOthers) {
  for (const char16_t* text : {u"@memory miso", u"@ai what is this"}) {
    base::test::TaskEnvironment task_environment(
        base::test::TaskEnvironment::TimeSource::MOCK_TIME);
    FakeAutocompleteController controller(&task_environment);
    for (size_t i = 0; i < controller.providers().size(); ++i) {
      controller.GetFakeProvider(i).done_ = false;
    }
    controller.Start(FakeAutocompleteController::CreateInput(text));
    for (size_t i = 0; i < controller.providers().size(); ++i) {
      EXPECT_TRUE(controller.GetFakeProvider(i).done_)
          << controller.providers()[i]->GetName();
    }
  }
}

// The omnibox's controllers (TYPE_BUILTIN) have both local providers, and
// each local input reaches exactly one provider: "@ai ..." the AI prompt
// provider (its row opens molt://ai with the prompt), "@memory ..." the
// memory one. "?" is not local input.
TEST(MoltMemoryControllerGateTest, BuiltinControllersRouteLocalInput) {
  base::test::TaskEnvironment task_environment;
  AutocompleteControllerConfig config;
  config.provider_types = AutocompleteProvider::TYPE_BUILTIN;
  auto client = std::make_unique<FakeAutocompleteProviderClient>();
  omnibox::RegisterProfilePrefs(static_cast<PrefRegistrySimple*>(
      client->GetPrefs()->DeprecatedGetPrefRegistry()));
  AutocompleteController controller(std::move(client), config);
  // BuiltinProvider, AiPromptProvider, MoltMemoryProvider.
  ASSERT_EQ(controller.providers().size(), 3u);

  auto running = [&controller]() {
    std::vector<AutocompleteProvider*> out;
    for (const auto& provider : controller.providers()) {
      if (controller.ShouldRunProvider(provider.get())) {
        out.push_back(provider.get());
      }
    }
    return out;
  };

  controller.Start(
      FakeAutocompleteController::CreateInput(u"@ai what is gravity"));
  std::vector<AutocompleteProvider*> ai = running();
  ASSERT_EQ(ai.size(), 1u);
  ASSERT_FALSE(ai[0]->matches().empty());
  EXPECT_EQ(ai[0]->matches()[0].destination_url,
            GURL("chrome://molt-ai/?q=what+is+gravity"));

  controller.Start(FakeAutocompleteController::CreateInput(u"@memory miso"));
  std::vector<AutocompleteProvider*> memory = running();
  ASSERT_EQ(memory.size(), 1u);
  EXPECT_NE(memory[0], ai[0]);

  controller.Start(FakeAutocompleteController::CreateInput(u"?what"));
  EXPECT_EQ(running().size(), 3u);
}

}  // namespace
