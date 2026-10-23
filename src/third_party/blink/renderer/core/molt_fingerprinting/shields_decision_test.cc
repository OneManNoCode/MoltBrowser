// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/shields_decision.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "base/synchronization/waitable_event.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/platform/task_type.h"
#include "third_party/blink/public/web/web_molt_fingerprinting.h"
#include "third_party/blink/renderer/core/frame/frame_test_helpers.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/web_local_frame_impl.h"
#include "third_party/blink/renderer/core/frame/web_remote_frame_impl.h"
#include "third_party/blink/renderer/core/inspector/worker_devtools_params.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/molt_fingerprinting.h"
#include "third_party/blink/renderer/core/testing/dummy_page_holder.h"
#include "third_party/blink/renderer/core/workers/global_scope_creation_params.h"
#include "third_party/blink/renderer/core/workers/worker_backing_thread_startup_data.h"
#include "third_party/blink/renderer/core/workers/worker_clients.h"
#include "third_party/blink/renderer/core/workers/worker_thread_test_helper.h"
#include "third_party/blink/renderer/platform/scheduler/public/post_cross_thread_task.h"
#include "third_party/blink/renderer/platform/storage/blink_storage_key.h"
#include "third_party/blink/renderer/platform/testing/task_environment.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"
#include "third_party/blink/renderer/platform/wtf/cross_thread_functional.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "v8/include/v8-inspector.h"

namespace blink::molt_fingerprinting {
namespace {

std::array<uint8_t, kSessionSeedSize> TestSeed() {
  std::array<uint8_t, kSessionSeedSize> seed;
  for (size_t i = 0; i < seed.size(); ++i) {
    seed[i] = static_cast<uint8_t>(5 + 11 * i);
  }
  return seed;
}

// The window `frame` shows, keyed under a.example (so that whether it is
// farbled depends on its Shields decision alone).
LocalDOMWindow& WindowOf(WebFrame* frame) {
  LocalDOMWindow& window =
      *To<WebLocalFrameImpl>(frame)->GetFrame()->DomWindow();
  window.SetStorageKey(BlinkStorageKey::CreateFirstParty(
      SecurityOrigin::Create(KURL("https://a.example/"))));
  return window;
}

// Runs on a worker's thread. `service_worker_url`: if set, first hands the
// worker a service worker's storage key and the browser's decision, as
// ServiceWorkerGlobalScope::InitializeGlobalScope does.
void IsFarbledOnWorkerThread(WorkerThread* thread,
                             const char* service_worker_url,
                             bool shields_down,
                             bool* farbled,
                             base::WaitableEvent* done) {
  EXPECT_TRUE(thread->IsCurrentThread());
  if (service_worker_url) {
    SetServiceWorkerStorageKey(
        *thread->GlobalScope(),
        BlinkStorageKey::CreateFirstParty(
            SecurityOrigin::Create(KURL(service_worker_url))),
        shields_down);
  }
  *farbled = IsFarbled(thread->GlobalScope());
  done->Signal();
}

// A worker global scope on its own thread. With a `creator`, it is set up
// the way DedicatedWorker::CreateGlobalScopeCreationParams sets up a
// dedicated worker: the creator's decision handed over through its
// WorkerClients before the thread starts.
class TestWorker {
 public:
  explicit TestWorker(ExecutionContext* creator)
      : thread_(std::make_unique<WorkerThreadForTest>(reporting_proxy_)) {
    std::unique_ptr<GlobalScopeCreationParams> params =
        GlobalScopeCreationParams::CreateForWorkerForTesting(
            SecurityOrigin::Create(KURL("https://a.example/")).get(),
            KURL("https://a.example/worker.js"));
    if (creator) {
      ProvideShieldsDecisionToDedicatedWorker(*creator,
                                              params->worker_clients.Get());
    }
    thread_->Start(std::move(params),
                   WorkerBackingThreadStartupData::CreateDefault(),
                   std::make_unique<WorkerDevToolsParams>());
    // Once a top-level script has run, the global scope is set up.
    thread_->EvaluateClassicScript(
        KURL("https://a.example/worker.js"), String("// empty"),
        /*cached_meta_data=*/nullptr, v8_inspector::V8StackTraceId());
    reporting_proxy_.WaitUntilDidEvaluateTopLevelScript();
  }

  ~TestWorker() {
    thread_->Terminate();
    thread_->WaitForShutdownForTesting();
  }

  bool Farbled(const char* service_worker_url = nullptr,
               bool shields_down = false) {
    bool farbled = false;
    base::WaitableEvent done;
    WorkerThread* thread = thread_.get();
    PostCrossThreadTask(
        *thread->GetTaskRunner(TaskType::kInternalTest), FROM_HERE,
        CrossThreadBindOnce(&IsFarbledOnWorkerThread,
                            CrossThreadUnretained(thread),
                            CrossThreadUnretained(service_worker_url),
                            shields_down, CrossThreadUnretained(&farbled),
                            CrossThreadUnretained(&done)));
    done.Wait();
    return farbled;
  }

 private:
  MockWorkerReportingProxy reporting_proxy_;
  std::unique_ptr<WorkerThreadForTest> thread_;
};

// These tests never configure kUniform (see molt_fingerprinting_test.cc);
// the scratch harness described in docs/FINGERPRINTING.md covers MoltNet.
class MoltShieldsDecisionTest : public testing::Test {
 protected:
  void SetUp() override {
    ResetForTesting();
    Configure(Mode::kFarble);
    SetSessionSeed(TestSeed());
  }
  void TearDown() override { ResetForTesting(); }

  // A main-frame window showing `url`, with that origin's storage key.
  LocalDOMWindow& Window(const char* url) {
    holders_.push_back(DummyPageHolder::CreateAndCommitNavigation(KURL(url)));
    LocalDOMWindow& window = *holders_.back()->GetFrame().DomWindow();
    window.SetStorageKey(
        BlinkStorageKey::CreateFirstParty(SecurityOrigin::Create(KURL(url))));
    return window;
  }

  test::TaskEnvironment task_environment_;
  std::vector<std::unique_ptr<DummyPageHolder>> holders_;
};

TEST_F(MoltShieldsDecisionTest, DocumentsWithoutADecisionAreFarbled) {
  LocalDOMWindow& a = Window("https://a.example/");
  EXPECT_FALSE(AreShieldsDown(a));
  EXPECT_TRUE(IsFarbled(&a));
}

TEST_F(MoltShieldsDecisionTest, TheDecisionSiteIsTheTopLevelSite) {
  EXPECT_EQ(ShieldsDecisionSiteOf(Window("https://www.a.example/page")),
            "https://a.example");
  EXPECT_EQ(ShieldsDecisionSiteOf(Window("http://a.example:8080/")),
            "http://a.example");
}

TEST_F(MoltShieldsDecisionTest, ADecisionCountsOnlyForItsOwnSite) {
  LocalDOMWindow& a = Window("https://www.a.example/");
  // Made for another top-level site (a commit racing a document the renderer
  // made on its own, or a bug): farbled.
  SetWindowShieldsDecision(a, /*shields_down=*/true, "https://b.example");
  EXPECT_FALSE(AreShieldsDown(a));
  EXPECT_TRUE(IsFarbled(&a));
  SetWindowShieldsDecision(a, /*shields_down=*/true, String());
  EXPECT_TRUE(IsFarbled(&a));
  // Made for its own site.
  SetWindowShieldsDecision(a, /*shields_down=*/true, "https://a.example");
  EXPECT_TRUE(AreShieldsDown(a));
  EXPECT_FALSE(IsFarbled(&a));
  EXPECT_EQ(HardwareConcurrency(&a, 12), 12u);
  // Shields on.
  SetWindowShieldsDecision(a, /*shields_down=*/false, "https://a.example");
  EXPECT_TRUE(IsFarbled(&a));
}

TEST_F(MoltShieldsDecisionTest, NonWebTopLevelSitesStayUnfarbled) {
  LocalDOMWindow& local = Window("file:///tmp/page.html");
  EXPECT_FALSE(IsFarbled(&local));
  SetWindowShieldsDecision(local, /*shields_down=*/false, "file://");
  EXPECT_FALSE(IsFarbled(&local));
}

TEST_F(MoltShieldsDecisionTest, IframesInheritTheirParentsDecision) {
  frame_test_helpers::WebViewHelper helper;
  helper.Initialize();
  WebLocalFrameImpl* main = helper.LocalMainFrame();
  frame_test_helpers::LoadHTMLString(main, "<iframe></iframe>",
                                     KURL("https://a.example/"));
  ASSERT_TRUE(main->FirstChild());
  LocalDOMWindow& parent = WindowOf(main);
  LocalDOMWindow& child = WindowOf(main->FirstChild());

  // No decision anywhere yet.
  InheritWindowShieldsDecision(child);
  EXPECT_TRUE(IsFarbled(&child));

  // The parent's document got "Shields off"; the iframe's initial empty
  // document takes it (its top-level site is the parent's).
  SetWindowShieldsDecision(parent, /*shields_down=*/true, "https://a.example");
  EXPECT_EQ(ShieldsDecisionSiteOf(child), "https://a.example");
  InheritWindowShieldsDecision(child);
  EXPECT_FALSE(IsFarbled(&child));

  SetWindowShieldsDecision(parent, /*shields_down=*/false, "https://a.example");
  InheritWindowShieldsDecision(child);
  EXPECT_TRUE(IsFarbled(&child));
}

TEST_F(MoltShieldsDecisionTest, ChildOfARemoteParentChecksTheRemoteTop) {
  frame_test_helpers::WebViewHelper helper;
  helper.InitializeRemote(SecurityOrigin::Create(KURL("https://a.example/")));
  WebLocalFrameImpl* local =
      helper.CreateLocalChild(*helper.RemoteMainFrame());
  LocalDOMWindow& window = WindowOf(local);
  // Nothing to inherit from a parent in another process: farbled.
  InheritWindowShieldsDecision(window);
  EXPECT_TRUE(IsFarbled(&window));
  // The top-level site is the remote main frame's replicated origin's.
  EXPECT_EQ(ShieldsDecisionSiteOf(window), "https://a.example");
  SetWindowShieldsDecision(window, /*shields_down=*/true, "https://a.example");
  EXPECT_FALSE(IsFarbled(&window));
}

TEST_F(MoltShieldsDecisionTest, PopupsInheritTheirOpenersDecision) {
  frame_test_helpers::WebViewHelper opener_helper;
  opener_helper.Initialize();
  WebLocalFrameImpl* opener = opener_helper.LocalMainFrame();
  frame_test_helpers::LoadHTMLString(opener, "", KURL("https://a.example/"));
  SetWindowShieldsDecision(WindowOf(opener), /*shields_down=*/true,
                           "https://a.example");

  frame_test_helpers::WebViewHelper popup_helper;
  popup_helper.InitializeWithOpener(opener);
  LocalDOMWindow& popup = WindowOf(popup_helper.LocalMainFrame());
  // The popup's initial empty document has its opener's origin.
  ASSERT_EQ(ShieldsDecisionSiteOf(popup), "https://a.example");
  InheritWindowShieldsDecision(popup);
  EXPECT_FALSE(IsFarbled(&popup));
}

TEST_F(MoltShieldsDecisionTest, ThePublicEntryPointsActOnTheCurrentDocument) {
  frame_test_helpers::WebViewHelper helper;
  helper.Initialize();
  WebLocalFrameImpl* main = helper.LocalMainFrame();
  frame_test_helpers::LoadHTMLString(main, "", KURL("https://a.example/"));
  WebMoltFingerprinting::SetDocumentShieldsDecision(main, true,
                                                    "https://a.example");
  EXPECT_FALSE(IsFarbled(&WindowOf(main)));
  // A main frame without an opener has nothing to inherit.
  WebMoltFingerprinting::InheritDocumentShieldsDecision(main);
  EXPECT_TRUE(IsFarbled(&WindowOf(main)));
  // Null frames are ignored.
  WebMoltFingerprinting::SetDocumentShieldsDecision(nullptr, true,
                                                    "https://a.example");
  WebMoltFingerprinting::InheritDocumentShieldsDecision(nullptr);
}

TEST_F(MoltShieldsDecisionTest, DedicatedWorkersInheritTheirCreatorsDecision) {
  LocalDOMWindow& down = Window("https://a.example/");
  SetWindowShieldsDecision(down, /*shields_down=*/true, "https://a.example");
  LocalDOMWindow& up = Window("https://a.example/");

  TestWorker from_down(&down);
  EXPECT_FALSE(from_down.Farbled());
  TestWorker from_up(&up);
  EXPECT_TRUE(from_up.Farbled());
  // Nothing handed over: farbled.
  TestWorker orphan(nullptr);
  EXPECT_TRUE(orphan.Farbled());
}

TEST_F(MoltShieldsDecisionTest, ServiceWorkersTakeTheBrowsersDecision) {
  TestWorker down(nullptr);
  EXPECT_FALSE(down.Farbled("https://a.example/", /*shields_down=*/true));
  TestWorker up(nullptr);
  EXPECT_TRUE(up.Farbled("https://a.example/", /*shields_down=*/false));
}

TEST_F(MoltShieldsDecisionTest, NoDecisionCountsWhenProtectionIsOff) {
  ResetForTesting();  // kOff
  LocalDOMWindow& a = Window("https://a.example/");
  SetWindowShieldsDecision(a, /*shields_down=*/true, "https://a.example");
  EXPECT_FALSE(AreShieldsDown(a));
  EXPECT_FALSE(IsFarbled(&a));
}

}  // namespace
}  // namespace blink::molt_fingerprinting
