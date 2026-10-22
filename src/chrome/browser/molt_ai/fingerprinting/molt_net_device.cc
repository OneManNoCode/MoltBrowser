// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/fingerprinting/molt_net_device.h"

#include "chrome/browser/molt_ai/fingerprinting/molt_fingerprinting_browser.h"
#include "chrome/browser/molt_ai/fingerprinting/molt_net_device_values.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/common/web_preferences/web_preferences.h"
#include "third_party/blink/public/mojom/cpu_performance.mojom-shared.h"
#include "third_party/blink/public/mojom/webpreferences/web_preferences.mojom-shared.h"
#include "ui/base/pointer/pointer_device.h"

namespace molt_ai::fingerprinting {

namespace {

// The values name what they mirror.
static_assert(kMoltNetAvailablePointerTypes == ui::POINTER_TYPE_FINE);
static_assert(kMoltNetAvailableHoverTypes == ui::HOVER_TYPE_HOVER);
static_assert(kMoltNetPrimaryPointerType ==
              static_cast<int>(blink::mojom::PointerType::kPointerFineType));
static_assert(kMoltNetPrimaryHoverType ==
              static_cast<int>(blink::mojom::HoverType::kHoverHoverType));
static_assert(kMoltNetCpuPerformanceTier ==
              static_cast<int>(blink::mojom::PerformanceTier::kUnknown));

bool IsMoltNet(content::BrowserContext* context) {
  // kOff for a null context and while the kill switch is off.
  return ModeForBrowserContext(context) == RendererMode::kUniform;
}

}  // namespace

void ApplyMoltNetInputPreferences(blink::web_pref::WebPreferences& prefs) {
  prefs.touch_event_feature_detection_enabled =
      kMoltNetTouchEventFeatureDetection;
  prefs.pointer_events_max_touch_points = kMoltNetMaxTouchPoints;
  prefs.available_pointer_types = kMoltNetAvailablePointerTypes;
  prefs.primary_pointer_type =
      static_cast<blink::mojom::PointerType>(kMoltNetPrimaryPointerType);
  prefs.available_hover_types = kMoltNetAvailableHoverTypes;
  prefs.primary_hover_type =
      static_cast<blink::mojom::HoverType>(kMoltNetPrimaryHoverType);
}

void OverrideMoltNetWebPreferences(content::WebContents* web_contents,
                                   blink::web_pref::WebPreferences* prefs) {
  if (!web_contents || !prefs ||
      !IsMoltNet(web_contents->GetBrowserContext())) {
    return;
  }
  ApplyMoltNetInputPreferences(*prefs);
}

std::optional<int> MoltNetCpuPerformanceTier(content::BrowserContext* context) {
  if (!IsMoltNet(context)) {
    return std::nullopt;
  }
  return kMoltNetCpuPerformanceTier;
}

}  // namespace molt_ai::fingerprinting
