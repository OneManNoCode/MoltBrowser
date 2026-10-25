// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_uniformity.h"

#include <array>
#include <atomic>
#include <string_view>

#include "third_party/blink/public/common/input/web_pointer_event.h"
#include "third_party/blink/public/common/input/web_pointer_properties.h"
#include "third_party/blink/public/mojom/permissions/permission.mojom-blink.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_pointer_event_init.h"
#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/molt_fingerprinting.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_values.h"
#include "third_party/blink/renderer/core/molt_net/molt_net_renderer.h"
#include "third_party/blink/renderer/platform/text/platform_locale.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size_f.h"

namespace blink::molt_fingerprinting {

namespace {

std::atomic<bool> g_uniformity_for_testing{false};

bool UniformMode() {
  return IsUniform() ||
         g_uniformity_for_testing.load(std::memory_order_relaxed);
}

String Utf8(std::string_view text) {
  return String::FromUtf8(text);
}

template <size_t N>
const Vector<String>& Labels(Vector<String>& cache,
                             const std::array<const char*, N>& labels) {
  if (cache.empty()) {
    cache.reserve(N);
    for (const char* label : labels) {
      cache.push_back(Utf8(label));
    }
  }
  return cache;
}

// The form-control Locale of MoltNet renderers: en-US, the same on every
// platform and for every user (no OS regional settings, no UI language).
class UniformFormLocale final : public Locale {
 public:
  UniformFormLocale() = default;
  ~UniformFormLocale() override = default;

  String DateFormat() override { return Utf8(moltnet::kDateFormat); }
  String MonthFormat() override { return Utf8(moltnet::kMonthFormat); }
  String ShortMonthFormat() override {
    return Utf8(moltnet::kShortMonthFormat);
  }
  String TimeFormat() override { return Utf8(moltnet::kTimeFormat); }
  String ShortTimeFormat() override { return Utf8(moltnet::kShortTimeFormat); }
  String DateTimeFormatWithSeconds() override {
    return Utf8(moltnet::kDateTimeFormatWithSeconds);
  }
  String DateTimeFormatWithoutSeconds() override {
    return Utf8(moltnet::kDateTimeFormatWithoutSeconds);
  }
  const Vector<String>& ShortMonthLabels() override {
    return Labels(short_month_labels_, moltnet::kShortMonthLabels);
  }
  const Vector<String>& StandAloneMonthLabels() override {
    return MonthLabels();
  }
  const Vector<String>& ShortStandAloneMonthLabels() override {
    return ShortMonthLabels();
  }
  const Vector<String>& TimeAmPmLabels() override {
    return Labels(am_pm_labels_, moltnet::kAmPmLabels);
  }
  const Vector<String>& MonthLabels() override {
    return Labels(month_labels_, moltnet::kMonthLabels);
  }
  const Vector<String>& WeekDayShortLabels() override {
    return Labels(week_day_short_labels_, moltnet::kWeekDayShortLabels);
  }
  wtf_size_t FirstDayOfWeek() override { return moltnet::kFirstDayOfWeek; }
  bool IsRtl() override { return false; }

 private:
  void InitializeLocaleData() override {
    if (did_initialize_number_data_) {
      return;
    }
    did_initialize_number_data_ = true;
    constexpr std::string_view kDigits = "0123456789";
    Vector<String, kDecimalSymbolsSize> symbols;
    for (size_t i = 0; i < kDigits.size(); ++i) {
      symbols.push_back(Utf8(kDigits.substr(i, 1)));
    }
    symbols.push_back(Utf8(moltnet::kDecimalSeparator));
    symbols.push_back(Utf8(moltnet::kGroupSeparator));
    SetLocaleData(symbols, Utf8(moltnet::kPositivePrefix),
                  Utf8(moltnet::kPositiveSuffix),
                  Utf8(moltnet::kNegativePrefix),
                  Utf8(moltnet::kNegativeSuffix));
  }

  Vector<String> month_labels_;
  Vector<String> short_month_labels_;
  Vector<String> week_day_short_labels_;
  Vector<String> am_pm_labels_;
  bool did_initialize_number_data_ = false;
};

display::mojom::ScreenOrientation ToMojom(moltnet::Orientation orientation) {
  switch (orientation) {
    case moltnet::Orientation::kPortraitPrimary:
      return display::mojom::ScreenOrientation::kPortraitPrimary;
    case moltnet::Orientation::kLandscapePrimary:
      return display::mojom::ScreenOrientation::kLandscapePrimary;
  }
  return display::mojom::ScreenOrientation::kLandscapePrimary;
}

}  // namespace

bool UsesMoltNetUniformity(ExecutionContext* context) {
  if (!UniformMode()) {
    return false;
  }
  // In kUniform, IsFarbled() is false only for contexts whose top-level
  // site isn't web content and for contexts without a site (worklets);
  // Shields-off sites are farbled.
  return !context || IsFarbled(context);
}

void UniformTouch(LocalDOMWindow* window,
                  WebPointerEvent& event,
                  gfx::SizeF& radius) {
  if (!UniformMode()) {
    return;
  }
  if (const std::optional<gfx::PointF> screen_point =
          UniformScreenPoint(window, event.PositionInWidget())) {
    event.SetPositionInScreen(*screen_point);
  }
  if (!UsesMoltNetUniformity(window)) {
    return;
  }
  radius = gfx::SizeF(moltnet::kTouchRadius, moltnet::kTouchRadius);
  event.rotation_angle = moltnet::kTouchRotationAngle;
  event.force = moltnet::kTouchForce;
}

void UniformTouchPointerEventInit(LocalDOMWindow* window,
                                  const WebPointerEvent& event,
                                  PointerEventInit* init) {
  if (!init || !UniformMode() ||
      event.pointer_type != WebPointerProperties::PointerType::kTouch ||
      !UsesMoltNetUniformity(window)) {
    return;
  }
  init->setWidth(moltnet::kPointerContactSize);
  init->setHeight(moltnet::kPointerContactSize);
  init->setPressure(moltnet::PointerPressure(init->buttons() != 0));
  init->setTiltX(moltnet::kPointerTilt);
  init->setTiltY(moltnet::kPointerTilt);
  init->setAltitudeAngle(moltnet::kPointerAltitudeAngle);
  init->setAzimuthAngle(moltnet::kPointerAzimuthAngle);
  init->setTangentialPressure(moltnet::kPointerTangentialPressure);
  init->setTwist(moltnet::kPointerTwist);
}

bool HidesScreenDetails(LocalDOMWindow* window) {
  return UsesMoltNetUniformity(window);
}

bool ReportsPermissionDenied(ExecutionContext* context,
                             mojom::blink::PermissionName name) {
  // display-capture: wherever the process hosts MoltNet, as screen, window
  // and tab capture are refused there (molt_net_renderer.h), whatever the
  // fingerprinting mode.
  if (name == mojom::blink::PermissionName::DISPLAY_CAPTURE) {
    return MoltNetDisplayCaptureDenied();
  }
  return name == mojom::blink::PermissionName::WINDOW_MANAGEMENT &&
         UsesMoltNetUniformity(context);
}

bool HidesScreenChanges() {
  return UniformMode();
}

std::optional<display::mojom::ScreenOrientation> UniformOrientationType(
    LocalDOMWindow* window) {
  // Paired with UniformScreenRect(), which is on exactly in kUniform.
  if (!IsUniform()) {
    return std::nullopt;
  }
  const std::optional<gfx::Rect> screen =
      window ? UniformScreenRect(window->GetFrame()) : std::nullopt;
  if (!screen) {
    // No frame (e.g. a detached iframe's ScreenOrientation, which would
    // otherwise report the real orientation it cached): the common answer.
    return ToMojom(moltnet::Orientation::kLandscapePrimary);
  }
  return ToMojom(moltnet::OrientationOf(screen->width(), screen->height()));
}

std::optional<uint16_t> UniformOrientationAngle(LocalDOMWindow* window) {
  if (!IsUniform()) {
    return std::nullopt;
  }
  return moltnet::kOrientationAngle;
}

std::unique_ptr<Locale> CreateUniformFormLocale() {
  return std::make_unique<UniformFormLocale>();
}

void PinUniformFormLocale() {
  Locale::SetDefaultLocaleOverride(CreateUniformFormLocale());
}

void SetMoltNetUniformityForTesting(bool uniform) {
  g_uniformity_for_testing.store(uniform, std::memory_order_relaxed);
}

}  // namespace blink::molt_fingerprinting
