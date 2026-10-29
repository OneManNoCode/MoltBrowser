// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Touchless browsing's Phase 0 exit criterion, without speech
// (docs/design/TOUCHLESS_BROWSING.md §10): driven only through
// VoicePageController, a video site is searched, its results are mapped,
// the fourth is opened and its video plays with sound after the
// accessibility click. Also: a search engine's results, a sign-in form whose
// password field can't be written, and a frame from another site whose
// items are on the map and can be pressed.

#include "chrome/browser/molt_ai/voice/voice_page_controller.h"

#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>

#include "base/command_line.h"
#include "base/functional/function_ref.h"
#include "base/run_loop.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/test/test_future.h"
#include "base/test/test_timeouts.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "chrome/browser/molt_ai/voice/page_map.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/test_navigation_observer.h"
#include "media/base/media_switches.h"
#include "net/dns/mock_host_resolver.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/accessibility/ax_mode.h"
#include "ui/accessibility/ax_tree_update.h"
#include "ui/accessibility/ax_updates_and_events.h"

namespace molt_ai {
namespace voice {
namespace {

using ActionResult = VoicePageController::ActionResult;

// The video site's results (video_results.html), as the map should read
// them: title, channel, duration in seconds, views, age.
struct ExpectedVideo {
  const char* title;
  const char* channel;
  int seconds;
  int64_t views;
  const char* age;
};
constexpr ExpectedVideo kVideos[] = {
    {"Queen \xE2\x80\x93 Bohemian Rhapsody (Official Video Remastered)",
     "Queen Official", 360, 2100000000, "15 years ago"},
    {"Bohemian Rhapsody (Remastered 2011)", "Queen - Topic", 355, 312000000,
     "11 years ago"},
    {"Queen - Bohemian Rhapsody (Live at Rock Montreal 1981)", "Queen Official",
     344, 48000000, "3 years ago"},
    {"Queen - Bohemian Rhapsody (Live Aid 1985)", "Queen Official", 151,
     97000000, "7 years ago"},
    {"Bohemian Rhapsody | Muppet Music Video | The Muppets", "The Muppets", 287,
     88000000, "15 years ago"},
    {"Wayne's World - Bohemian Rhapsody scene", "Paramount Movies", 192,
     21000000, "9 years ago"},
    {"Bohemian Rhapsody | Official Trailer [HD] | 20th Century FOX",
     "20th Century Studios", 152, 31000000, "6 years ago"},
    {"Panic! At The Disco - Bohemian Rhapsody (Official Video)",
     "Fueled By Ramen", 368, 60000000, "9 years ago"},
    {"Bohemian Rhapsody - Pentatonix", "Pentatonix", 357, 45000000,
     "7 years ago"},
    {"Bohemian Rhapsody | Piano Tutorial", "Piano Lessons Daily", 495, 1200000,
     "2 years ago"},
    {"Bohemian Rhapsody (Lyrics)", "Lyric Vibes", 359, 18000000, "4 years ago"},
    {"Queen - Bohemian Rhapsody (Live at Wembley Stadium 1986)",
     "Queen Official", 358, 35000000, "5 years ago"},
    {"Bohemian Rhapsody - Rami Malek Live Aid scene", "Movieclips", 260,
     12000000, "5 years ago"},
    {"How Queen wrote Bohemian Rhapsody", "Polyphonic", 706, 6400000,
     "6 years ago"},
    {"Bohemian Rhapsody - Marching Band", "Ohio State Marching Band", 242,
     9100000, "8 years ago"},
    {"Bohemian Rhapsody on Classical Guitar", "Marcin", 372, 14000000,
     "3 years ago"},
    {"Bohemian Rhapsody - Kanye West at Glastonbury 2015", "BBC Music", 105,
     8300000, "9 years ago"},
    {"Bohemian Rhapsody - Queen + Adam Lambert (Rock in Rio 2015)",
     "Queen Official", 381, 5200000, "1 year ago"},
    {"The Story of Bohemian Rhapsody (BBC documentary)", "BBC", 3523, 2700000,
     "10 months ago"},
    {"Bohemian Rhapsody but it's lofi", "Lofi Covers", 3735, 940000,
     "2 weeks ago"},
};

// The map as text, for failure messages.
std::string Describe(const PageMap& map) {
  std::string out = "title: " + map.title + "\n";
  for (const PageItem& item : map.items) {
    base::StrAppend(&out, {base::NumberToString(item.number), " ",
                           ItemKindName(item.kind), " \"", item.label, "\"\n"});
  }
  for (const PageList& list : map.lists) {
    base::StrAppend(
        &out, {"list \"", list.label, "\" (", ListKindName(list.kind), ", ",
               base::NumberToString(list.entries.size()), " entries)\n"});
    for (const PageListEntry& entry : list.entries) {
      base::StrAppend(
          &out, {"  ", base::NumberToString(entry.position), ". #",
                 base::NumberToString(entry.number), " \"", entry.title,
                 "\" channel \"", entry.channel, "\" duration ", entry.duration,
                 " date \"", entry.date, "\"\n"});
    }
  }
  return out;
}

const PageItem* FindItem(const PageMap& map,
                         ItemKind kind,
                         std::string_view label) {
  for (const PageItem& item : map.items) {
    if (item.kind == kind && item.label == label) {
      return &item;
    }
  }
  return nullptr;
}

class VoicePageControllerBrowserTest : public InProcessBrowserTest {
 public:
  void SetUpCommandLine(base::CommandLine* command_line) override {
    InProcessBrowserTest::SetUpCommandLine(command_line);
    // Unmuted media may start only after user activation, so the video
    // test shows the accessibility click gave the page activation.
    command_line->AppendSwitchASCII(
        switches::kAutoplayPolicy,
        switches::autoplay::kDocumentUserActivationRequiredPolicy);
  }

  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    host_resolver()->AddRule("*", "127.0.0.1");
    ASSERT_TRUE(embedded_test_server()->Start());
    VoicePageController::CreateForWebContents(web_contents());
    controller()->Start();
  }

 protected:
  content::WebContents* web_contents() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }

  VoicePageController* controller() {
    return VoicePageController::FromWebContents(web_contents());
  }

  GURL Url(std::string_view host, std::string_view path) {
    return embedded_test_server()->GetURL(host, path);
  }

  // The map once |ready| finds it complete: the trees arrive after the
  // page loads, and change as its scripts run.
  // On a timeout the test fails, printing the last map it got.
  PageMap WaitForMap(base::FunctionRef<bool(const PageMap&)> ready) {
    const base::TimeTicks deadline =
        base::TimeTicks::Now() + TestTimeouts::action_timeout();
    for (;;) {
      PageMap map = controller()->BuildMap();
      if (ready(map)) {
        return map;
      }
      const base::TimeDelta left = deadline - base::TimeTicks::Now();
      if (!left.is_positive()) {
        ADD_FAILURE() << "The map never got ready:\n" << Describe(map);
        return map;
      }
      base::RunLoop loop;
      base::OneShotTimer timeout;
      timeout.Start(FROM_HERE, left, loop.QuitClosure());
      controller()->SetTreesUpdatedCallbackForTesting(loop.QuitClosure());
      loop.Run();
      controller()->SetTreesUpdatedCallbackForTesting(base::RepeatingClosure());
    }
  }

  // Waits until |expression| is true in |frame|. Script runs without a
  // user gesture, so it never gives the page activation itself.
  bool WaitForJs(const content::ToRenderFrameHost& frame,
                 std::string_view expression) {
    const std::string script = base::StrCat(
        {"new Promise(resolve => { const check = () => { if (", expression,
         ") { resolve(true); } else { setTimeout(check, 20); } }; "
         "check(); })"});
    return content::EvalJs(frame, script,
                           content::EXECUTE_SCRIPT_NO_USER_GESTURE) == true;
  }

  content::EvalJsResult Js(const content::ToRenderFrameHost& frame,
                           std::string_view script) {
    return content::EvalJs(frame, script,
                           content::EXECUTE_SCRIPT_NO_USER_GESTURE);
  }
};

IN_PROC_BROWSER_TEST_F(VoicePageControllerBrowserTest,
                       SearchesAVideoSiteAndPlaysTheFourthResult) {
  EXPECT_TRUE(web_contents()->GetAccessibilityMode().has_mode(
      VoicePageController::AccessibilityMode().flags()));

  // The home page: type into the search box and submit.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), Url("video.test", "/molt_voice/video_home.html")));
  PageMap home = WaitForMap([](const PageMap& map) {
    return FindItem(map, ItemKind::kTextField, "Search") != nullptr;
  });
  const PageItem* box = FindItem(home, ItemKind::kTextField, "Search");
  ASSERT_TRUE(box);
  EXPECT_TRUE(box->writable);
  EXPECT_EQ(controller()->SetText(box->handle, "Bohemian Rhapsody"),
            ActionResult::kDone);
  // The page gets the value and the trusted input and change events sites
  // listen to.
  ASSERT_TRUE(WaitForJs(web_contents(),
                        "document.getElementById('search').value === "
                        "'Bohemian Rhapsody'"));
  EXPECT_TRUE(WaitForJs(web_contents(),
                        "moltEvents.includes('input:trusted') && "
                        "moltEvents.includes('change:trusted')"));

  content::TestNavigationObserver to_results(web_contents());
  base::test::TestFuture<ActionResult> submitted;
  controller()->Submit(box->handle, submitted.GetCallback());
  EXPECT_EQ(submitted.Get(), ActionResult::kDone);
  to_results.Wait();
  EXPECT_EQ(web_contents()->GetLastCommittedURL().path(),
            "/molt_voice/video_results.html");
  EXPECT_EQ(web_contents()->GetLastCommittedURL().query(),
            "search_query=Bohemian%20Rhapsody");

  // The results: the main list is the twenty videos, with their fields.
  PageMap results = WaitForMap([](const PageMap& map) {
    return !map.lists.empty() && map.lists[0].entries.size() == 20;
  });
  const PageList& videos = results.lists[0];
  SCOPED_TRACE(Describe(results));
  for (size_t i = 0; i < std::size(kVideos); ++i) {
    SCOPED_TRACE(i);
    const PageListEntry& entry = videos.entries[i];
    EXPECT_EQ(entry.position, static_cast<int>(i + 1));
    EXPECT_EQ(entry.title, kVideos[i].title);
    EXPECT_EQ(entry.channel, kVideos[i].channel);
    EXPECT_EQ(entry.duration_seconds, kVideos[i].seconds);
    EXPECT_EQ(entry.date, kVideos[i].age);
    ASSERT_EQ(entry.counts.size(), 1u);
    EXPECT_EQ(entry.counts[0].value, kVideos[i].views);
    EXPECT_EQ(entry.counts[0].unit, "views");
    const PageItem* link = results.ItemByNumber(entry.number);
    ASSERT_TRUE(link);
    EXPECT_EQ(link->kind, ItemKind::kLink);
  }

  // "Play the fourth one": open it.
  const PageListEntry& fourth = videos.entries[3];
  content::TestNavigationObserver to_watch(web_contents());
  EXPECT_EQ(controller()->Press(fourth.handle), ActionResult::kDone);
  to_watch.Wait();
  EXPECT_EQ(web_contents()->GetLastCommittedURL().query(), "v=video4");

  // The watch page: play the video through its default action.
  PageMap watch =
      WaitForMap([](const PageMap& map) { return !map.media.empty(); });
  ASSERT_TRUE(watch.media[0].video);
  EXPECT_EQ(false,
            Js(web_contents(), "navigator.userActivation.hasBeenActive"));
  EXPECT_EQ(controller()->PlayMedia(watch.media[0].handle),
            ActionResult::kDone);
  EXPECT_EQ("playing", Js(web_contents(), "whenPlaying()"));
  // The page saw a trusted click with user activation, and the video plays
  // with sound.
  EXPECT_EQ(true, Js(web_contents(), "clickInfo.trusted && clickInfo.active"));
  EXPECT_EQ(true, Js(web_contents(), "navigator.userActivation.hasBeenActive"));
  EXPECT_EQ(true, Js(web_contents(),
                     "const p = document.getElementById('player'); "
                     "!p.paused && !p.muted && p.volume > 0"));

  // Pause and resume as the media keys do.
  EXPECT_EQ(controller()->PauseMedia(), ActionResult::kDone);
  EXPECT_TRUE(
      WaitForJs(web_contents(), "document.getElementById('player').paused"));
  EXPECT_EQ(controller()->ResumeMedia(), ActionResult::kDone);
  EXPECT_TRUE(WaitForJs(web_contents(),
                        "!document.getElementById('player').paused && "
                        "!document.getElementById('player').muted"));
}

IN_PROC_BROWSER_TEST_F(VoicePageControllerBrowserTest, MapsSearchResults) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), Url("search.test", "/molt_voice/search_results.html")));
  PageMap map = WaitForMap([](const PageMap& map) {
    return !map.lists.empty() && map.lists[0].entries.size() == 10;
  });
  const PageList& results = map.lists[0];
  SCOPED_TRACE(Describe(map));
  EXPECT_TRUE(results.in_main);
  EXPECT_EQ(results.entries[0].title, "Bohemian Rhapsody");
  EXPECT_EQ(results.entries[0].channel, "Wikipedia");
  EXPECT_EQ(results.entries[1].title,
            "Queen - Bohemian Rhapsody (Official Video Remastered)");
  EXPECT_EQ(results.entries[1].channel, "YouTube");
  EXPECT_EQ(results.entries[1].date, "Aug 1, 2008");
  EXPECT_EQ(results.entries[9].channel, "Queen Online");
  const PageItem* first = map.ItemByNumber(results.entries[0].number);
  ASSERT_TRUE(first);
  EXPECT_EQ(first->url, "https://en.wikipedia.org/wiki/Bohemian_Rhapsody");
  // The search box is the first item, with its query.
  ASSERT_FALSE(map.items.empty());
  EXPECT_EQ(map.items[0].kind, ItemKind::kTextField);
  EXPECT_EQ(map.items[0].value, "bohemian rhapsody");
}

IN_PROC_BROWSER_TEST_F(VoicePageControllerBrowserTest,
                       NeverWritesAPasswordField) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), Url("signin.test", "/molt_voice/sign_in.html")));
  PageMap map = WaitForMap([](const PageMap& map) {
    return FindItem(map, ItemKind::kTextField, "Password") != nullptr;
  });
  EXPECT_TRUE(map.has_credential_fields);
  const PageItem* email = FindItem(map, ItemKind::kTextField, "Email");
  const PageItem* password = FindItem(map, ItemKind::kTextField, "Password");
  const PageItem* code = FindItem(map, ItemKind::kTextField, "Code");
  ASSERT_TRUE(email);
  ASSERT_TRUE(password);
  ASSERT_TRUE(code);
  EXPECT_EQ(password->credential, CredentialKind::kPassword);
  EXPECT_FALSE(password->writable);
  EXPECT_EQ(code->credential, CredentialKind::kOneTimeCode);

  EXPECT_EQ(controller()->SetText(password->handle, "hunter2"),
            ActionResult::kCredentialField);
  EXPECT_EQ(controller()->SetText(code->handle, "123456"),
            ActionResult::kCredentialField);
  EXPECT_EQ(controller()->SetText(email->handle, "ana@example.com"),
            ActionResult::kDone);
  ASSERT_TRUE(WaitForJs(web_contents(),
                        "document.getElementById('email').value === "
                        "'ana@example.com'"));
  EXPECT_EQ("",
            Js(web_contents(), "document.getElementById('password').value"));
  EXPECT_EQ("", Js(web_contents(), "document.getElementById('code').value"));
  EXPECT_EQ("email:true", Js(web_contents(), "moltEvents.join(',')"));
}

IN_PROC_BROWSER_TEST_F(VoicePageControllerBrowserTest,
                       MapsAndPressesACrossSiteFramesItems) {
  const GURL widget = Url("b.test", "/molt_voice/frame_widget.html");
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(),
      Url("a.test", "/molt_voice/frame_host.html?frame=" + widget.spec())));
  PageMap map = WaitForMap([](const PageMap& map) {
    return FindItem(map, ItemKind::kButton, "Like") != nullptr;
  });
  const PageItem* home = FindItem(map, ItemKind::kLink, "Home");
  const PageItem* like = FindItem(map, ItemKind::kButton, "Like");
  const PageItem* after = FindItem(map, ItemKind::kButton, "After frame");
  ASSERT_TRUE(home);
  ASSERT_TRUE(like);
  ASSERT_TRUE(after);
  // In reading order, the frame's items between the host's; its own tree.
  EXPECT_LT(home->number, like->number);
  EXPECT_LT(like->number, after->number);
  EXPECT_NE(like->handle.tree, home->handle.tree);

  content::RenderFrameHost* frame =
      content::ChildFrameAt(web_contents()->GetPrimaryMainFrame(), 0);
  ASSERT_TRUE(frame);
  EXPECT_EQ(frame->GetLastCommittedOrigin().host(), "b.test");
  EXPECT_EQ(controller()->Press(like->handle), ActionResult::kDone);
  EXPECT_TRUE(WaitForJs(frame, "window.likes === 1"));
}

// The frames a page embeds can't take the page's place: with no room left
// for frames' trees (in number, or in nodes), the page's own tree is still
// kept, whichever arrives first, and the frame's items are left out.
IN_PROC_BROWSER_TEST_F(VoicePageControllerBrowserTest,
                       KeepsThePagesTreeWhenFramesFillTheCaps) {
  const GURL widget = Url("b.test", "/molt_voice/frame_widget.html");
  const GURL host =
      Url("a.test", "/molt_voice/frame_host.html?frame=" + widget.spec());
  for (const bool by_nodes : {false, true}) {
    SCOPED_TRACE(by_nodes ? "nodes" : "trees");
    controller()->Stop();
    if (by_nodes) {
      controller()->SetTreeCapsForTesting(VoicePageController::kMaxTrees, 1);
    } else {
      controller()->SetTreeCapsForTesting(0, 1000);
    }
    controller()->Start();
    ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), host));
    content::RenderFrameHost* frame =
        content::ChildFrameAt(web_contents()->GetPrimaryMainFrame(), 0);
    ASSERT_TRUE(frame);
    ASSERT_TRUE(WaitForJs(frame, "window.likes === 0"));
    const PageMap map = WaitForMap([](const PageMap& map) {
      return FindItem(map, ItemKind::kButton, "After frame") != nullptr;
    });
    EXPECT_TRUE(controller()->HasPageTree());
    EXPECT_TRUE(FindItem(map, ItemKind::kLink, "Home"));
    EXPECT_FALSE(FindItem(map, ItemKind::kButton, "Like"));
  }
}

// Tree data alone (the browser sends some when the focus moves, as when a
// tab becomes active again) waits for the tree it describes instead of
// failing on the empty tree voice mode starts with; the whole tree still
// comes and maps.
IN_PROC_BROWSER_TEST_F(VoicePageControllerBrowserTest,
                       TreeDataAloneWaitsForItsTree) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), Url("signin.test", "/molt_voice/sign_in.html")));
  WaitForMap([](const PageMap& map) { return !map.items.empty(); });
  const VoicePageController::Stats before = controller()->stats();

  controller()->Stop();
  controller()->Start();
  ui::AXUpdatesAndEvents details;
  details.ax_tree_id = web_contents()->GetPrimaryMainFrame()->GetAXTreeID();
  ui::AXTreeUpdate data_only;
  data_only.has_tree_data = true;
  data_only.tree_data.tree_id = details.ax_tree_id;
  details.updates.push_back(data_only);
  static_cast<content::WebContentsObserver*>(controller())
      ->AccessibilityEventReceived(details);
  EXPECT_EQ(controller()->stats().unserialize_failures,
            before.unserialize_failures);
  EXPECT_EQ(controller()->stats().resets, before.resets);
  EXPECT_FALSE(controller()->HasPageTree());

  const PageMap map =
      WaitForMap([](const PageMap& map) { return !map.items.empty(); });
  EXPECT_FALSE(map.items.empty());
  EXPECT_EQ(controller()->stats().unserialize_failures,
            before.unserialize_failures);
}

IN_PROC_BROWSER_TEST_F(VoicePageControllerBrowserTest,
                       RefusesStaleTargetsAndStops) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), Url("video.test", "/molt_voice/video_home.html")));
  PageMap home = WaitForMap([](const PageMap& map) {
    return FindItem(map, ItemKind::kTextField, "Search") != nullptr;
  });
  const PageNodeHandle box =
      FindItem(home, ItemKind::kTextField, "Search")->handle;

  // Another document: the old handle names nothing now.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), Url("signin.test", "/molt_voice/sign_in.html")));
  EXPECT_EQ(controller()->Press(box), ActionResult::kStale);
  EXPECT_EQ(controller()->SetText(box, "x"), ActionResult::kStale);
  EXPECT_EQ(controller()->Press(PageNodeHandle{9999, 1}), ActionResult::kStale);

  // Off: no map, no actions, and the mode is released.
  controller()->Stop();
  EXPECT_FALSE(controller()->active());
  EXPECT_TRUE(controller()->BuildMap().items.empty());
  EXPECT_EQ(controller()->Press(box), ActionResult::kNotActive);
  EXPECT_EQ(controller()->PauseMedia(), ActionResult::kNotActive);
  EXPECT_FALSE(web_contents()->GetAccessibilityMode().has_mode(
      VoicePageController::AccessibilityMode().flags()));
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
