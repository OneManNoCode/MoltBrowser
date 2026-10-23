// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_uniformity.h"

#include <memory>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/input/web_pointer_event.h"
#include "third_party/blink/public/mojom/permissions/permission.mojom-blink.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_pointer_event_init.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/dom/element.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/html/html_element.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/molt_fingerprinting.h"
#include "third_party/blink/renderer/core/testing/dummy_page_holder.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/storage/blink_storage_key.h"
#include "third_party/blink/renderer/platform/testing/task_environment.h"
#include "third_party/blink/renderer/platform/text/date_components.h"
#include "third_party/blink/renderer/platform/text/platform_locale.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/size_f.h"

namespace blink::molt_fingerprinting {
namespace {

using PermissionName = mojom::blink::PermissionName;

// kUniform can't be configured here (it pins the process's time zone and
// locale for every later test), so MoltNet is simulated with
// SetMoltNetUniformityForTesting() on top of kFarble. The screen-derived
// values (Touch.screenX/Y, screen.orientation) need real kUniform and are
// only checked to stay real here; their rules are in moltnet_values.h.
class MoltNetUniformityTest : public testing::Test {
 protected:
  void SetUp() override {
    ResetForTesting();
    SetMoltNetUniformityForTesting(false);
  }
  void TearDown() override {
    SetMoltNetUniformityForTesting(false);
    Locale::SetDefaultLocaleOverride(nullptr);
    ResetForTesting();
  }

  // A window whose top-level site is `top_level_url`'s site.
  LocalDOMWindow& Window(const char* top_level_url) {
    holders_.push_back(DummyPageHolder::CreateAndCommitNavigation(
        KURL("https://page.example/")));
    LocalDOMWindow& window = *holders_.back()->GetFrame().DomWindow();
    window.SetStorageKey(BlinkStorageKey::CreateFirstParty(
        SecurityOrigin::Create(KURL(top_level_url))));
    return window;
  }

  void SimulateMoltNet() {
    Configure(Mode::kFarble);
    SetMoltNetUniformityForTesting(true);
  }

  test::TaskEnvironment task_environment_;
  std::vector<std::unique_ptr<DummyPageHolder>> holders_;
};

// A touch on a digitizer that reports contact size, angle and pressure.
WebPointerEvent RealTouch() {
  WebPointerProperties properties(
      /*id_param=*/1, WebPointerProperties::PointerType::kTouch,
      WebPointerProperties::Button::kLeft,
      /*position_in_widget=*/gfx::PointF(40, 30),
      /*position_in_screen=*/gfx::PointF(1240, 830));
  properties.force = 0.75f;
  properties.twist = 30;
  properties.tangential_pressure = 0.25f;
  WebPointerEvent event(WebInputEvent::Type::kPointerDown, properties,
                        /*width_param=*/22, /*height_param=*/18);
  event.rotation_angle = 15;
  return event;
}

// What UpdateCommonPointerEventInit sets from RealTouch().
PointerEventInit* RealPointerInit() {
  PointerEventInit* init = PointerEventInit::Create();
  init->setButtons(1);
  init->setWidth(22);
  init->setHeight(18);
  init->setPressure(0.75f);
  init->setTiltX(0);
  init->setTiltY(0);
  init->setTangentialPressure(0.25f);
  init->setTwist(30);
  return init;
}

void ExpectTouchUntouched(LocalDOMWindow* window) {
  WebPointerEvent event = RealTouch();
  gfx::SizeF radius(11, 9);
  UniformTouch(window, event, radius);
  EXPECT_EQ(event.PositionInScreen(), gfx::PointF(1240, 830));
  EXPECT_EQ(radius, gfx::SizeF(11, 9));
  EXPECT_EQ(event.rotation_angle, 15);
  EXPECT_EQ(event.force, 0.75f);

  PointerEventInit* init = RealPointerInit();
  UniformTouchPointerEventInit(window, RealTouch(), init);
  EXPECT_EQ(init->width(), 22);
  EXPECT_EQ(init->height(), 18);
  EXPECT_EQ(init->pressure(), 0.75f);
  EXPECT_EQ(init->twist(), 30);
  EXPECT_EQ(init->tangentialPressure(), 0.25f);
}

void ExpectEverythingReal(LocalDOMWindow* window) {
  EXPECT_FALSE(UsesMoltNetUniformity(window));
  ExpectTouchUntouched(window);
  EXPECT_FALSE(HidesScreenDetails(window));
  EXPECT_FALSE(
      ReportsPermissionDenied(window, PermissionName::WINDOW_MANAGEMENT));
  EXPECT_FALSE(UniformOrientationType(window).has_value());
  EXPECT_FALSE(UniformOrientationAngle(window).has_value());
}

TEST_F(MoltNetUniformityTest, OffLeavesEverythingReal) {
  LocalDOMWindow& window = Window("https://a.example/");
  ExpectEverythingReal(&window);
  EXPECT_FALSE(UsesMoltNetUniformity(nullptr));
  EXPECT_FALSE(HidesScreenDetails(nullptr));
  EXPECT_FALSE(HidesScreenChanges());
  EXPECT_FALSE(UniformOrientationType(nullptr).has_value());
}

TEST_F(MoltNetUniformityTest, NormalWindowsLeaveEverythingReal) {
  Configure(Mode::kFarble);
  LocalDOMWindow& window = Window("https://a.example/");
  ASSERT_TRUE(IsFarbled(&window));
  ExpectEverythingReal(&window);
  EXPECT_FALSE(HidesScreenChanges());
}

TEST_F(MoltNetUniformityTest, MoltNetHidesTheTouchContact) {
  SimulateMoltNet();
  LocalDOMWindow& window = Window("https://a.example/");
  ASSERT_TRUE(UsesMoltNetUniformity(&window));

  WebPointerEvent event = RealTouch();
  gfx::SizeF radius(11, 9);
  UniformTouch(&window, event, radius);
  EXPECT_EQ(radius, gfx::SizeF(0, 0));
  EXPECT_EQ(event.rotation_angle, 0);
  EXPECT_EQ(event.force, 0);
  // screenX/Y follow UniformScreenPoint(), which needs real kUniform.
  EXPECT_EQ(event.PositionInScreen(), gfx::PointF(1240, 830));
  EXPECT_EQ(event.PositionInWidget(), gfx::PointF(40, 30));

  // A finger that is down: pressure 0.5, no size, angle or barrel.
  PointerEventInit* down = RealPointerInit();
  UniformTouchPointerEventInit(&window, RealTouch(), down);
  EXPECT_EQ(down->width(), 1);
  EXPECT_EQ(down->height(), 1);
  EXPECT_EQ(down->pressure(), 0.5f);
  EXPECT_EQ(down->tiltX(), 0);
  EXPECT_EQ(down->tiltY(), 0);
  EXPECT_DOUBLE_EQ(down->altitudeAngle(), 1.5707963267948966);
  EXPECT_EQ(down->azimuthAngle(), 0);
  EXPECT_EQ(down->twist(), 0);
  EXPECT_EQ(down->tangentialPressure(), 0);
  // A finger that was lifted: pressure 0.
  PointerEventInit* up = RealPointerInit();
  up->setButtons(0);
  UniformTouchPointerEventInit(&window, RealTouch(), up);
  EXPECT_EQ(up->pressure(), 0);
}

TEST_F(MoltNetUniformityTest, MoltNetLeavesPensAndMiceAlone) {
  SimulateMoltNet();
  LocalDOMWindow& window = Window("https://a.example/");
  for (auto type : {WebPointerProperties::PointerType::kPen,
                    WebPointerProperties::PointerType::kMouse}) {
    WebPointerEvent event = RealTouch();
    event.pointer_type = type;
    PointerEventInit* init = RealPointerInit();
    UniformTouchPointerEventInit(&window, event, init);
    EXPECT_EQ(init->width(), 22);
    EXPECT_EQ(init->pressure(), 0.75f);
    EXPECT_EQ(init->twist(), 30);
  }
  UniformTouchPointerEventInit(&window, RealTouch(), nullptr);  // No crash.
}

TEST_F(MoltNetUniformityTest, MoltNetHidesScreenDetails) {
  SimulateMoltNet();
  LocalDOMWindow& window = Window("https://a.example/");
  EXPECT_TRUE(HidesScreenDetails(&window));
  EXPECT_TRUE(
      ReportsPermissionDenied(&window, PermissionName::WINDOW_MANAGEMENT));
  // Other permissions keep their real state.
  EXPECT_FALSE(ReportsPermissionDenied(&window, PermissionName::GEOLOCATION));
  EXPECT_FALSE(ReportsPermissionDenied(&window, PermissionName::NOTIFICATIONS));
  EXPECT_TRUE(HidesScreenChanges());
  // (That MoltNet ignores Shields off needs real kUniform; the simulation
  // runs on kFarble, where Shields-off sites aren't farbled.)
}

TEST_F(MoltNetUniformityTest, MoltNetOpaqueAndNullContextsFailClosed) {
  SimulateMoltNet();
  LocalDOMWindow& opaque = Window("data:text/html,x");
  EXPECT_TRUE(UsesMoltNetUniformity(&opaque));
  EXPECT_TRUE(HidesScreenDetails(&opaque));
  EXPECT_TRUE(UsesMoltNetUniformity(nullptr));
  EXPECT_TRUE(
      ReportsPermissionDenied(nullptr, PermissionName::WINDOW_MANAGEMENT));
  WebPointerEvent event = RealTouch();
  gfx::SizeF radius(11, 9);
  UniformTouch(nullptr, event, radius);
  EXPECT_EQ(radius, gfx::SizeF(0, 0));
}

TEST_F(MoltNetUniformityTest, MoltNetLeavesNonWebPagesReal) {
  SimulateMoltNet();
  // Like chrome:// pages: a top-level site that isn't http(s).
  LocalDOMWindow& local = Window("file:///home/user/page.html");
  ASSERT_FALSE(IsFarbled(&local));
  ExpectEverythingReal(&local);
  // The screen itself is uniform for the whole process, so is its stillness.
  EXPECT_TRUE(HidesScreenChanges());
}

TEST_F(MoltNetUniformityTest, UniformFormLocaleIsEnUs) {
  std::unique_ptr<Locale> locale = CreateUniformFormLocale();
  EXPECT_EQ(locale->DateFormat(), "M/d/yy");
  EXPECT_EQ(locale->MonthFormat(), "MMMM yyyy");
  EXPECT_EQ(locale->ShortMonthFormat(), "MMM yyyy");
  EXPECT_EQ(locale->TimeFormat(), String::FromUtf8("h:mm:ss a"));
  EXPECT_EQ(locale->ShortTimeFormat(), String::FromUtf8("h:mm a"));
  EXPECT_EQ(locale->DateTimeFormatWithSeconds(),
            String::FromUtf8("M/d/yy, h:mm:ss a"));
  EXPECT_EQ(locale->DateTimeFormatWithoutSeconds(),
            String::FromUtf8("M/d/yy, h:mm a"));
  ASSERT_EQ(locale->MonthLabels().size(), 12u);
  EXPECT_EQ(locale->MonthLabels()[2], "March");
  EXPECT_EQ(locale->StandAloneMonthLabels()[11], "December");
  ASSERT_EQ(locale->ShortMonthLabels().size(), 12u);
  EXPECT_EQ(locale->ShortMonthLabels()[8], "Sep");
  EXPECT_EQ(locale->ShortStandAloneMonthLabels()[0], "Jan");
  ASSERT_EQ(locale->WeekDayShortLabels().size(), 7u);
  EXPECT_EQ(locale->WeekDayShortLabels()[0], "S");
  EXPECT_EQ(locale->WeekDayShortLabels()[1], "M");
  ASSERT_EQ(locale->TimeAmPmLabels().size(), 2u);
  EXPECT_EQ(locale->TimeAmPmLabels()[1], "PM");
  EXPECT_EQ(locale->FirstDayOfWeek(), 0u);
  EXPECT_FALSE(locale->IsRtl());

  // Numbers: ASCII digits, "." and a plain minus.
  EXPECT_EQ(locale->ConvertToLocalizedNumber("-1234.5"), "-1234.5");
  EXPECT_EQ(locale->ConvertFromLocalizedNumber("-1234.5"), "-1234.5");
  EXPECT_EQ(locale->LocalizedDecimalSeparator(), ".");
  EXPECT_TRUE(locale->UsesSingleCharNumberFiltering());
  EXPECT_EQ(locale->StripInvalidNumberCharacters("1,5e3x", "+-.Ee"), "15e3");
}

TEST_F(MoltNetUniformityTest, UniformFormLocaleFormatsDates) {
  std::unique_ptr<Locale> locale = CreateUniformFormLocale();
  wtf_size_t end = 0;
  DateComponents date;
  ASSERT_TRUE(date.ParseDate("2024-03-05", 0, end));
  EXPECT_EQ(locale->FormatDateTime(date), "03/05/2024");
  DateComponents month;
  ASSERT_TRUE(month.ParseMonth("2024-03", 0, end));
  EXPECT_EQ(locale->FormatDateTime(month), "March 2024");
  EXPECT_EQ(locale->FormatDateTime(month, Locale::kFormatTypeShort),
            "Mar 2024");
  DateComponents time;
  ASSERT_TRUE(time.ParseTime("13:04:09", 0, end));
  EXPECT_EQ(locale->FormatDateTime(time), String::FromUtf8("1:04:09 PM"));
  EXPECT_EQ(locale->FormatDateTime(time, Locale::kFormatTypeShort),
            String::FromUtf8("1:04 PM"));
  DateComponents date_time;
  ASSERT_TRUE(date_time.ParseDateTimeLocal("2024-03-05T00:04:09", 0, end));
  EXPECT_EQ(locale->FormatDateTime(date_time),
            String::FromUtf8("03/05/2024, 12:04:09 AM"));
}

// Configure(kUniform) pins the locale through Locale::DefaultLocale(),
// which every form control of a document reaches (Element::GetLocale()).
TEST_F(MoltNetUniformityTest, PinnedLocaleReachesFormControls) {
  std::unique_ptr<Locale> uniform = CreateUniformFormLocale();
  Locale* pinned = uniform.get();
  Locale::SetDefaultLocaleOverride(std::move(uniform));
  EXPECT_EQ(&Locale::DefaultLocale(), pinned);
  // Resetting the platform default (as language overrides do) keeps it.
  Locale::ResetDefaultLocale();
  EXPECT_EQ(&Locale::DefaultLocale(), pinned);

  LocalDOMWindow& window = Window("https://a.example/");
  Element* body = window.document()->body();
  ASSERT_TRUE(body);
  EXPECT_EQ(&body->GetLocale(), pinned);
  EXPECT_EQ(body->GetLocale().DateFormat(), "M/d/yy");

  Locale::SetDefaultLocaleOverride(nullptr);
  EXPECT_NE(&Locale::DefaultLocale(), pinned);
}

}  // namespace
}  // namespace blink::molt_fingerprinting
