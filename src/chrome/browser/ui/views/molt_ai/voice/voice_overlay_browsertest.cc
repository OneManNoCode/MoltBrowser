// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The voice overlay in a browser window (docs/design/TOUCHLESS_BROWSING.md
// §5.3, §5.5), on round 36's fixture pages (chrome/test/data/molt_voice/):
// badges land on the items where the page lays them out, at 100% and 200%
// zoom and on a 2x screen, and follow scrolling; they go away when the page
// navigates, the tab is switched or the page renumbers; the ring; the
// caption bar sits under the page with a fixed height, is a polite live
// region and stays with the window across tabs. Runs on the build machine.

#include "chrome/browser/ui/views/molt_ai/voice/voice_overlay.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/functional/function_ref.h"
#include "base/json/string_escape.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/bind.h"
#include "base/run_loop.h"
#include "base/test/test_timeouts.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "chrome/browser/molt_ai/voice/page_map.h"
#include "chrome/browser/molt_ai/voice/voice_page_controller.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/molt_ai/voice/voice_caption_bar.h"
#include "chrome/browser/ui/views/molt_ai/voice/voice_numbers_view.h"
#include "chrome/browser/ui/views/molt_ai/voice/voice_overlay_layout.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/zoom/zoom_controller.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "net/dns/mock_host_resolver.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/page/page_zoom.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/accessibility/ax_node_data.h"
#include "ui/base/page_transition_types.h"
#include "ui/display/display_switches.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/native/native_view_host.h"
#include "ui/views/controls/webview/webview.h"
#include "ui/views/test/views_test_utils.h"
#include "ui/views/widget/widget.h"

namespace molt_ai {
namespace voice {
namespace {

// Badges sit within this many DIPs of where the page lays its items out
// (Blink rounds the bounds it reports to assistive technology).
constexpr float kTolerance = 2;

constexpr char kResultsPath[] = "/molt_voice/video_results.html";
constexpr char kSearchPath[] = "/molt_voice/search_results.html";

class VoiceOverlayBrowserTest : public InProcessBrowserTest {
 public:
  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    host_resolver()->AddRule("*", "127.0.0.1");
    ASSERT_TRUE(embedded_test_server()->Start());
    browser()->window()->SetBounds(gfx::Rect(0, 0, 1280, 900));
    StartVoiceMode(web_contents());
  }

 protected:
  static void StartVoiceMode(content::WebContents* contents) {
    VoicePageController::CreateForWebContents(contents);
    VoicePageController::FromWebContents(contents)->Start();
  }

  content::WebContents* web_contents() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }
  VoicePageController* controller() {
    return VoicePageController::FromWebContents(web_contents());
  }
  VoiceOverlay* overlay() {
    return VoiceOverlay::GetOrCreateForWebContents(web_contents());
  }
  // The numbers view of the area that shows the active tab.
  VoiceNumbersView* numbers_view() {
    VoiceOverlay::NumbersSink* sink =
        VoiceOverlay::FindNumbersSinkForTesting(web_contents());
    return sink ? static_cast<VoiceNumbersView*>(sink) : nullptr;
  }
  // The caption bar of the area that shows the active tab.
  VoiceCaptionBar* caption_bar() {
    for (VoiceOverlay::CaptionSink* sink :
         VoiceOverlay::FindCaptionSinksForTesting(browser())) {
      auto* bar = static_cast<VoiceCaptionBar*>(sink);
      if (bar->contents_view() &&
          bar->contents_view()->web_contents() == web_contents()) {
        return bar;
      }
    }
    return nullptr;
  }

  void Navigate(std::string_view path) {
    ASSERT_TRUE(ui_test_utils::NavigateToURL(
        browser(), embedded_test_server()->GetURL("video.test", path)));
  }

  // Polls |condition| (which may run script in the page) every 50 ms, up
  // to the action timeout.
  static bool Poll(base::FunctionRef<bool()> condition) {
    const base::TimeTicks deadline =
        base::TimeTicks::Now() + TestTimeouts::action_timeout();
    while (!condition()) {
      if (base::TimeTicks::Now() > deadline) {
        return false;
      }
      base::RunLoop loop;
      base::OneShotTimer timer;
      timer.Start(FROM_HERE, base::Milliseconds(50), loop.QuitClosure());
      loop.Run();
    }
    return true;
  }

  // The map once |ready| finds it complete (the trees arrive after the page
  // loads and change as its scripts run).
  PageMap WaitForMap(base::FunctionRef<bool(const PageMap&)> ready) {
    PageMap map;
    EXPECT_TRUE(Poll([&] {
      map = controller()->BuildMap();
      return ready(map);
    })) << "The map never got ready";
    return map;
  }

  // The video site's results, mapped with its twenty videos and, once the
  // page is laid out as the trees say, each title where the page has it.
  PageMap OpenVideoResults() {
    Navigate(kResultsPath);
    return WaitForResults();
  }
  PageMap WaitForResults() {
    return WaitForMap([this](const PageMap& map) {
      return !map.lists.empty() && map.lists[0].entries.size() == 20 &&
             TreesMatchLayout(map);
    });
  }

  float ScaleFactor() {
    return web_contents()->GetRenderWidgetHostView()->GetDeviceScaleFactor();
  }

  // Where the page lays out the elements |selector| matches: their border
  // boxes in DIPs from the top left of the page's view (CSS pixels times
  // the zoom; devicePixelRatio is the zoom times the scale factor).
  std::vector<gfx::RectF> PageRects(std::string_view selector) {
    const std::string script = base::StrCat(
        {"(() => { const out = []; for (const e of "
         "document.querySelectorAll(",
         base::GetQuotedJSONString(selector),
         ")) { const r = e.getBoundingClientRect(); "
         "out.push(r.left, r.top, r.width, r.height); } "
         "out.push(window.devicePixelRatio); return out; })()"});
    const content::EvalJsResult result = content::EvalJs(
        web_contents(), script, content::EXECUTE_SCRIPT_NO_USER_GESTURE);
    const base::ListValue& list = result.ExtractList();
    std::vector<gfx::RectF> rects;
    if (list.empty()) {
      return rects;
    }
    const double to_dips = list[list.size() - 1].GetDouble() / ScaleFactor();
    for (size_t i = 0; i + 4 < list.size(); i += 4) {
      rects.emplace_back(list[i].GetDouble() * to_dips,
                         list[i + 1].GetDouble() * to_dips,
                         list[i + 2].GetDouble() * to_dips,
                         list[i + 3].GetDouble() * to_dips);
    }
    return rects;
  }

  // Whether the first title's bounds in |map| are where the page has it
  // (after a zoom or a scroll, the trees catch up a little later).
  bool TreesMatchLayout(const PageMap& map) {
    const PageItem* item = map.ItemByNumber(map.lists[0].entries[0].number);
    const std::vector<gfx::RectF> titles = PageRects("a#video-title");
    if (!item || titles.empty()) {
      return false;
    }
    const float scale = ScaleFactor();
    return std::abs(item->bounds.y / scale - titles[0].y()) < kTolerance &&
           std::abs(item->bounds.height / scale - titles[0].height()) <
               kTolerance;
  }

  // The item under badge |number|, in DIPs from the top left of the page's
  // view; nullopt if it has no badge.
  std::optional<gfx::RectF> BadgedItem(int number) {
    VoiceNumbersView* view = numbers_view();
    for (const BadgePlacement& badge : view->plan().badges) {
      if (badge.number == number) {
        gfx::Point origin;
        views::View::ConvertPointToTarget(view->contents_view()->holder(),
                                          view, &origin);
        return gfx::RectF(badge.item.x - origin.x(), badge.item.y - origin.y(),
                          badge.item.width, badge.item.height);
      }
    }
    return std::nullopt;
  }

  const BadgePlacement* Badge(int number) {
    for (const BadgePlacement& badge : numbers_view()->plan().badges) {
      if (badge.number == number) {
        return &badge;
      }
    }
    return nullptr;
  }

  static void ExpectNear(const gfx::RectF& actual, const gfx::RectF& expected) {
    EXPECT_NEAR(actual.x(), expected.x(), kTolerance) << actual.ToString();
    EXPECT_NEAR(actual.y(), expected.y(), kTolerance) << actual.ToString();
    EXPECT_NEAR(actual.width(), expected.width(), kTolerance);
    EXPECT_NEAR(actual.height(), expected.height(), kTolerance);
  }

  // The map's item of |kind| labelled |label|, 0 if none.
  static int NumberOf(const PageMap& map,
                      ItemKind kind,
                      std::string_view label) {
    for (const PageItem& item : map.items) {
      if (item.kind == kind && item.label == label) {
        return item.number;
      }
    }
    return 0;
  }

  // Checks badges against where the page lays the items out: the results'
  // title links, the filter chips (buttons) and the guide's links. An item
  // with a badge has it where the page has the item; an item wholly out of
  // the viewport has none. (Close items, like the guide's, may have no
  // badge: there may be no room.) Returns how many badges were checked.
  int CheckBadges(const PageMap& map) {
    const OverlayRect viewport = numbers_view()->PageViewport();
    std::vector<gfx::RectF> rects;
    std::vector<int> numbers;
    const std::vector<gfx::RectF> titles = PageRects("a#video-title");
    EXPECT_EQ(titles.size(), 20u);
    for (size_t i = 0; i < titles.size(); ++i) {
      rects.push_back(titles[i]);
      numbers.push_back(map.lists[0].entries[i].number);
    }
    const std::vector<gfx::RectF> chips =
        PageRects("yt-chip-cloud-chip-renderer button");
    const char* const kChips[] = {"All",       "Shorts",  "Videos",
                                  "Unwatched", "Watched", "Recently uploaded"};
    EXPECT_EQ(chips.size(), std::size(kChips));
    for (size_t i = 0; i < chips.size() && i < std::size(kChips); ++i) {
      rects.push_back(chips[i]);
      numbers.push_back(NumberOf(map, ItemKind::kButton, kChips[i]));
    }
    const std::vector<gfx::RectF> guide = PageRects("#guide a");
    const char* const kGuide[] = {"Home",      "Shorts",      "Subscriptions",
                                  "History",   "Playlists",   "Watch later",
                                  "Liked videos"};
    EXPECT_EQ(guide.size(), std::size(kGuide));
    for (size_t i = 0; i < guide.size() && i < std::size(kGuide); ++i) {
      rects.push_back(guide[i]);
      numbers.push_back(NumberOf(map, ItemKind::kLink, kGuide[i]));
    }

    int checked = 0;
    for (size_t i = 0; i < rects.size(); ++i) {
      SCOPED_TRACE(i);
      EXPECT_GT(numbers[i], 0);
      const gfx::RectF& expected = rects[i];
      const std::optional<gfx::RectF> item = BadgedItem(numbers[i]);
      if (item) {
        ExpectNear(*item, expected);
        ++checked;
      }
      if (expected.bottom() <= 0 || expected.y() >= viewport.height ||
          expected.right() <= 0 || expected.x() >= viewport.width) {
        EXPECT_FALSE(item);
      }
    }
    // Badges are apart and wholly in the page's viewport.
    const BadgePlan& plan = numbers_view()->plan();
    for (size_t a = 0; a < plan.badges.size(); ++a) {
      EXPECT_TRUE(viewport.Contains(plan.badges[a].badge));
      for (size_t b = a + 1; b < plan.badges.size(); ++b) {
        EXPECT_FALSE(plan.badges[a].badge.Intersects(plan.badges[b].badge));
      }
    }
    return checked;
  }

  void LayOut() {
    views::test::RunScheduledLayout(numbers_view()->GetWidget());
  }

  int InnerHeight() {
    return content::EvalJs(web_contents(), "window.innerHeight",
                           content::EXECUTE_SCRIPT_NO_USER_GESTURE)
        .ExtractInt();
  }
};

IN_PROC_BROWSER_TEST_F(VoiceOverlayBrowserTest, BadgesSitOnTheItems) {
  const PageMap map = OpenVideoResults();
  ASSERT_TRUE(overlay()->ShowNumbers(map));
  EXPECT_TRUE(overlay()->numbers_shown());
  ASSERT_TRUE(numbers_view());
  EXPECT_TRUE(numbers_view()->GetVisible());
  LayOut();
  EXPECT_GE(CheckBadges(map), 4);
  // Full size: the results are well apart.
  const BadgePlacement* first = Badge(map.lists[0].entries[0].number);
  ASSERT_TRUE(first);
  EXPECT_FLOAT_EQ(first->scale, 1);
  EXPECT_GE(first->badge.height, numbers_view()->sizes().badge_font);
}

IN_PROC_BROWSER_TEST_F(VoiceOverlayBrowserTest, BadgesSitOnTheItemsAt200Zoom) {
  PageMap map = OpenVideoResults();
  ASSERT_TRUE(overlay()->ShowNumbers(map));
  LayOut();
  const BadgePlacement* at_100 = Badge(map.lists[0].entries[0].number);
  ASSERT_TRUE(at_100);
  const float badge_height = at_100->badge.height;

  // The page reflows at twice the size; the trees follow.
  ASSERT_TRUE(zoom::ZoomController::FromWebContents(web_contents())
                  ->SetZoomLevel(blink::ZoomFactorToZoomLevel(2.0)));
  map = WaitForResults();
  ASSERT_TRUE(overlay()->ShowNumbers(map));
  LayOut();
  EXPECT_GE(CheckBadges(map), 2);
  // The badges are the browser's: the page's zoom doesn't size them.
  const BadgePlacement* at_200 = Badge(map.lists[0].entries[0].number);
  ASSERT_TRUE(at_200);
  EXPECT_FLOAT_EQ(at_200->badge.height, badge_height);
}

IN_PROC_BROWSER_TEST_F(VoiceOverlayBrowserTest, NumbersFollowScrolling) {
  const PageMap map = OpenVideoResults();
  ASSERT_TRUE(overlay()->ShowNumbers(map));
  LayOut();
  const int first = map.lists[0].entries[0].number;
  const int fourth = map.lists[0].entries[3].number;
  ASSERT_TRUE(BadgedItem(first));

  // Scrolled by 700 CSS pixels: the first title leaves the screen, the
  // fourth comes up; the badges follow (the overlay maps the page again
  // after the scroll's location changes) under the same numbers.
  ASSERT_TRUE(content::ExecJs(web_contents(), "window.scrollTo(0, 700)",
                              content::EXECUTE_SCRIPT_NO_USER_GESTURE));
  const std::vector<gfx::RectF> titles = PageRects("a#video-title");
  ASSERT_EQ(titles.size(), 20u);
  ASSERT_TRUE(Poll([&] {
    const std::optional<gfx::RectF> item = BadgedItem(fourth);
    return item && !BadgedItem(first) &&
           std::abs(item->y() - titles[3].y()) < kTolerance;
  }));
  EXPECT_TRUE(overlay()->numbers_shown());
  EXPECT_GE(CheckBadges(map), 2);
}

IN_PROC_BROWSER_TEST_F(VoiceOverlayBrowserTest, NumbersHideOnNavigation) {
  std::vector<OverlayHidden> hidden;
  overlay()->SetHiddenCallback(base::BindRepeating(
      [](std::vector<OverlayHidden>* out, OverlayHidden reason) {
        out->push_back(reason);
      },
      &hidden));
  const PageMap map = OpenVideoResults();
  ASSERT_TRUE(overlay()->ShowNumbers(map));
  ASSERT_TRUE(overlay()->SetCurrentItem(map, 3));

  // A jump to a fragment of the page keeps them.
  Navigate(std::string(kResultsPath) + "#contents");
  EXPECT_TRUE(overlay()->numbers_shown());
  EXPECT_TRUE(hidden.empty());

  Navigate(kSearchPath);
  ASSERT_EQ(hidden.size(), 1u);
  EXPECT_EQ(hidden[0], OverlayHidden::kNavigated);
  EXPECT_FALSE(overlay()->numbers_shown());
  EXPECT_EQ(overlay()->current_item(), 0);
  EXPECT_TRUE(numbers_view()->plan().badges.empty());
  EXPECT_FALSE(numbers_view()->plan().ring);
  EXPECT_FALSE(numbers_view()->GetVisible());
}

IN_PROC_BROWSER_TEST_F(VoiceOverlayBrowserTest, NumbersHideOnTabSwitch) {
  std::vector<OverlayHidden> hidden;
  VoiceOverlay* first_tab = overlay();
  first_tab->SetHiddenCallback(base::BindRepeating(
      [](std::vector<OverlayHidden>* out, OverlayHidden reason) {
        out->push_back(reason);
      },
      &hidden));
  ASSERT_TRUE(first_tab->ShowNumbers(OpenVideoResults()));

  ASSERT_TRUE(AddTabAtIndex(1,
                            embedded_test_server()->GetURL("video.test",
                                                           kSearchPath),
                            ui::PAGE_TRANSITION_TYPED));
  ASSERT_EQ(hidden.size(), 1u);
  EXPECT_EQ(hidden[0], OverlayHidden::kTabHidden);
  EXPECT_FALSE(first_tab->numbers_shown());
  // The area shows the new tab, which has no numbers.
  ASSERT_TRUE(numbers_view());
  EXPECT_FALSE(numbers_view()->GetVisible());
  EXPECT_TRUE(numbers_view()->plan().badges.empty());
  // Numbers for a tab that isn't shown are refused.
  EXPECT_FALSE(first_tab->ShowNumbers(PageMap()));
}

IN_PROC_BROWSER_TEST_F(VoiceOverlayBrowserTest, NumbersHideWhenTheyMove) {
  std::vector<OverlayHidden> hidden;
  overlay()->SetHiddenCallback(base::BindRepeating(
      [](std::vector<OverlayHidden>* out, OverlayHidden reason) {
        out->push_back(reason);
      },
      &hidden));
  const PageMap map = OpenVideoResults();
  ASSERT_TRUE(overlay()->ShowNumbers(map));

  // A link added after the last item keeps every number.
  ASSERT_TRUE(content::ExecJs(
      web_contents(),
      "const a = document.createElement('a'); a.href = 'watch.html?v=more';"
      "a.textContent = 'More results'; document.body.append(a);",
      content::EXECUTE_SCRIPT_NO_USER_GESTURE));
  ASSERT_TRUE(Poll([&] {
    return controller()->BuildMap().items.size() > map.items.size();
  }));
  overlay()->FollowPageNowForTesting();
  EXPECT_TRUE(overlay()->numbers_shown());
  EXPECT_TRUE(hidden.empty());

  // One added before the first moves every number after it.
  ASSERT_TRUE(content::ExecJs(
      web_contents(),
      "const b = document.createElement('a'); b.href = 'watch.html?v=top';"
      "b.textContent = 'Top'; document.body.prepend(b);",
      content::EXECUTE_SCRIPT_NO_USER_GESTURE));
  ASSERT_TRUE(Poll([&] { return !hidden.empty(); }));
  EXPECT_EQ(hidden[0], OverlayHidden::kPageChanged);
  EXPECT_FALSE(overlay()->numbers_shown());
}

IN_PROC_BROWSER_TEST_F(VoiceOverlayBrowserTest, TheCurrentItemHasARing) {
  const PageMap map = OpenVideoResults();
  const int second = map.lists[0].entries[1].number;
  ASSERT_TRUE(overlay()->SetCurrentItem(map, second));
  EXPECT_FALSE(overlay()->numbers_shown());  // the ring alone
  LayOut();
  const BadgePlan& plan = numbers_view()->plan();
  ASSERT_TRUE(plan.ring);
  const std::optional<gfx::RectF> item = BadgedItem(second);
  ASSERT_TRUE(item);
  ASSERT_FALSE(plan.badges.empty());
  EXPECT_EQ(plan.badges[0].number, second);
  EXPECT_TRUE(plan.badges[0].current);
  EXPECT_TRUE(plan.ring->Contains(plan.badges[0].item));

  // On the last map, by number; an unknown number clears it.
  EXPECT_TRUE(overlay()->SetCurrentItem(map.lists[0].entries[2].number));
  EXPECT_EQ(overlay()->current_item(), map.lists[0].entries[2].number);
  EXPECT_FALSE(overlay()->SetCurrentItem(100000));
  EXPECT_EQ(overlay()->current_item(), 0);
  EXPECT_FALSE(numbers_view()->GetVisible());
}

// The overlay is not part of the page and stays out of its way.
IN_PROC_BROWSER_TEST_F(VoiceOverlayBrowserTest, NumbersStayOutOfThePagesWay) {
  const PageMap map = OpenVideoResults();
  web_contents()->Focus();
  ASSERT_TRUE(overlay()->ShowNumbers(map));
  VoiceNumbersView* view = numbers_view();
  EXPECT_FALSE(view->GetCanProcessEventsWithinSubtree());
  EXPECT_FALSE(view->IsFocusable());
  EXPECT_NE(view->GetFocusManager()->GetFocusedView(), view);
  ui::AXNodeData data;
  view->GetViewAccessibility().GetAccessibleNodeData(&data);
  EXPECT_TRUE(data.HasState(ax::mojom::State::kInvisible));
  // Drawn on a layer of its own, above the page's.
  ASSERT_TRUE(view->layer());
  EXPECT_FALSE(view->layer()->fills_bounds_opaquely());
  // Nothing in the page changed.
  EXPECT_EQ(content::EvalJs(web_contents(),
                            "document.querySelectorAll('[class*=molt],"
                            "[id*=molt]').length",
                            content::EXECUTE_SCRIPT_NO_USER_GESTURE),
            0);
}

IN_PROC_BROWSER_TEST_F(VoiceOverlayBrowserTest, TheCaptionBarSitsUnderThePage) {
  Navigate(kResultsPath);
  VoiceCaptionBar* bar = caption_bar();
  ASSERT_TRUE(bar);
  EXPECT_FALSE(bar->GetVisible());
  views::WebView* page = bar->contents_view();
  const gfx::Rect page_before = page->bounds();
  const int inner_before = InnerHeight();

  ASSERT_TRUE(overlay()->SetCaption(VoiceState::kListening,
                                    "play the live one by Queen", ""));
  views::test::RunScheduledLayout(bar->GetWidget());
  ASSERT_TRUE(bar->GetVisible());
  EXPECT_GT(bar->height(), 0);
  // Under the page, as wide; the page area gave it its height.
  EXPECT_EQ(bar->y(), page->bounds().bottom());
  EXPECT_EQ(bar->width(), page->width());
  EXPECT_EQ(page->height(), page_before.height() - bar->height());
  ASSERT_TRUE(Poll([&] { return InnerHeight() < inner_before; }));
  const int inner_with_bar = InnerHeight();

  // New text: the same height, so the page isn't resized.
  const int height = bar->height();
  ASSERT_TRUE(overlay()->SetCaption(
      VoiceState::kSpeaking, "play the live one by Queen",
      "Playing: Bohemian Rhapsody, Live Aid, and a very long title that goes "
      "on and on to see that the bar never grows a line"));
  views::test::RunScheduledLayout(bar->GetWidget());
  EXPECT_EQ(bar->height(), height);
  EXPECT_EQ(page->height(), page_before.height() - height);
  EXPECT_EQ(InnerHeight(), inner_with_bar);

  ASSERT_TRUE(overlay()->HideCaption());
  views::test::RunScheduledLayout(bar->GetWidget());
  EXPECT_FALSE(bar->GetVisible());
  EXPECT_EQ(page->bounds(), page_before);
  ASSERT_TRUE(Poll([&] { return InnerHeight() == inner_before; }));
}

IN_PROC_BROWSER_TEST_F(VoiceOverlayBrowserTest, TheCaptionBarIsALiveRegion) {
  Navigate(kResultsPath);
  ASSERT_TRUE(overlay()->SetCaption(VoiceState::kListening,
                                    "play the live one by Queen",
                                    "Playing: Bohemian Rhapsody, Live Aid"));
  VoiceCaptionBar* bar = caption_bar();
  ASSERT_TRUE(bar);
  ui::AXNodeData data;
  bar->GetViewAccessibility().GetAccessibleNodeData(&data);
  EXPECT_EQ(data.role, ax::mojom::Role::kStatus);
  EXPECT_EQ(data.GetStringAttribute(ax::mojom::StringAttribute::kName),
            "Voice mode");
  EXPECT_EQ(data.GetStringAttribute(ax::mojom::StringAttribute::kLiveStatus),
            "polite");
  EXPECT_EQ(
      data.GetStringAttribute(ax::mojom::StringAttribute::kContainerLiveStatus),
      "polite");
  EXPECT_EQ(data.GetStringAttribute(ax::mojom::StringAttribute::kDescription),
            "Listening");
  EXPECT_EQ(bar->heard_label()->GetText(),
            u"Heard: play the live one by Queen");
  EXPECT_EQ(bar->status_label()->GetText(),
            u"Playing: Bohemian Rhapsody, Live Aid");
  EXPECT_EQ(bar->state_label()->GetText(), u"Listening");
  // The state is the description, not announced at each change.
  ui::AXNodeData state;
  bar->state_label()->GetViewAccessibility().GetAccessibleNodeData(&state);
  EXPECT_TRUE(state.IsIgnored());
  // Never focused: the magnifier and the keyboard stay with the page.
  EXPECT_FALSE(bar->IsFocusable());

  // Text from a page is cleaned: no bidi overrides or control characters.
  ASSERT_TRUE(overlay()->SetCaption(
      VoiceState::kSpeaking, "",
      "Playing: \xE2\x80\xAEmoc.live\x1B[2J Title\r\nTwo"));
  EXPECT_EQ(bar->heard_label()->GetText(), u"");
  EXPECT_EQ(bar->status_label()->GetText(),
            u"Playing: moc.live[2J Title Two");
  bar->GetViewAccessibility().GetAccessibleNodeData(&data);
  EXPECT_EQ(data.GetStringAttribute(ax::mojom::StringAttribute::kDescription),
            "Speaking");
}

IN_PROC_BROWSER_TEST_F(VoiceOverlayBrowserTest,
                       TheCaptionStaysWithTheWindowAcrossTabs) {
  Navigate(kResultsPath);
  ASSERT_TRUE(overlay()->SetCaption(VoiceState::kSpeaking, "new tab",
                                    "Opened a new tab"));
  ASSERT_TRUE(AddTabAtIndex(1,
                            embedded_test_server()->GetURL("video.test",
                                                           kSearchPath),
                            ui::PAGE_TRANSITION_TYPED));
  VoiceCaptionBar* bar = caption_bar();
  ASSERT_TRUE(bar);
  EXPECT_TRUE(bar->GetVisible());
  ASSERT_TRUE(bar->caption());
  EXPECT_EQ(bar->caption()->status, "Opened a new tab");

  // The window-wide call reaches the same bar.
  VoiceOverlay::SetCaptionForWindow(browser(), VoiceState::kWaiting, "", "");
  EXPECT_EQ(bar->caption()->state, VoiceState::kWaiting);
  VoiceOverlay::HideCaptionForWindow(browser());
  EXPECT_FALSE(bar->GetVisible());
}

// Not a test: the overlay for a person to look at (with the OS magnifier,
// high contrast, a screen reader), until the window is closed. The active
// tab gets numbers (shown again when its page renumbers or navigates), a
// ring on its third item and a caption. The command is in
// docs/BUILD_MACHINE_CHECKLIST.md §7.3 (voice-overlay).
IN_PROC_BROWSER_TEST_F(VoiceOverlayBrowserTest, DISABLED_ManualLook) {
  OpenVideoResults();
  auto show = [this] {
    content::WebContents* contents = web_contents();
    if (!VoicePageController::FromWebContents(contents)) {
      StartVoiceMode(contents);
    }
    VoiceOverlay* tab = VoiceOverlay::GetOrCreateForWebContents(contents);
    const PageMap map =
        VoicePageController::FromWebContents(contents)->BuildMap();
    if (!tab->numbers_shown() && !map.items.empty()) {
      tab->ShowNumbers(map);
      tab->SetCurrentItem(map,
                          std::min(3, static_cast<int>(map.items.size())));
      tab->SetCaption(VoiceState::kSpeaking, "show numbers",
                      base::StrCat({"Showing ",
                                    base::NumberToString(map.items.size()),
                                    " numbers on ", map.title}));
    }
  };
  base::RepeatingTimer again;
  again.Start(FROM_HERE, base::Seconds(1), base::BindLambdaForTesting(show));
  show();
  ui_test_utils::BrowserDestroyedObserver(browser()).Wait();
}

// On a screen at twice the scale factor, Blink's pixels are twice the
// DIPs; the badges land in the same places.
class VoiceOverlayHighDpiBrowserTest : public VoiceOverlayBrowserTest {
 public:
  void SetUpCommandLine(base::CommandLine* command_line) override {
    VoiceOverlayBrowserTest::SetUpCommandLine(command_line);
    command_line->AppendSwitchASCII(switches::kForceDeviceScaleFactor, "2");
  }
};

IN_PROC_BROWSER_TEST_F(VoiceOverlayHighDpiBrowserTest, BadgesSitOnTheItems) {
  const PageMap map = OpenVideoResults();
  EXPECT_FLOAT_EQ(ScaleFactor(), 2);
  ASSERT_TRUE(overlay()->ShowNumbers(map));
  LayOut();
  EXPECT_GE(CheckBadges(map), 4);
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
