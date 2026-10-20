// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: the rest of the uniform MoltNet
// device. MoltNet windows (kUniform) already report one screen, time zone,
// language and GPU for everybody (molt_fingerprinting.h, gpu_uniformity.h);
// this file covers touches, the Window Management API, the screen
// orientation and the locale of form controls. The fixed values themselves
// are in moltnet_values.h.
//
// Upstream hooks (each marked "MoltBrowser:"):
//
//   touch     TouchEventManager::CreateDomTouch          -> UniformTouch
//             UpdateCommonPointerEventInit
//               (pointer_event_factory.cc)     -> UniformTouchPointerEventInit
//   screens   WindowScreenDetails::GetScreenDetails      -> HidesScreenDetails
//             PermissionStatusListener::state,
//               OnPermissionStatusChange            -> ReportsPermissionDenied
//             Screen::AreWebExposedScreenPropertiesEqual
//               (window.screen "change" events)          -> HidesScreenChanges
//   orient.   ScreenOrientation::type, angle  -> UniformOrientationType/Angle
//             ScreenOrientationController::
//               NotifyOrientationChangedInternal         -> HidesScreenChanges
//   locale    Locale::DefaultLocale (platform_locale.cc) returns the locale
//               Configure(kUniform) pins       -> CreateUniformFormLocale
//
// Which contexts. What continues an existing MoltNet treatment follows it:
// Touch.screenX/Y are exactly MouseEvent's (UniformScreenPoint), the
// orientation is derived from the uniform screen (UniformScreenRect), and
// screen change events stop wherever screen values are uniform; all three
// are process-wide, like the screen values. The new treatments (touch
// contact, getScreenDetails(), the window-management permission) apply to
// web content only, like the WebGL baselines: pages whose top-level site
// isn't http(s) (chrome:// and the like) are left alone. MoltNet ignores
// Shields off, as everywhere else. The form-control locale is per process,
// like the time zone and the Intl locale.
//
// Normal and Incognito windows (kFarble) are left alone: none of these
// values varies per site in a useful way, and fixing them would only make a
// user look like other MoltBrowser users, which normal windows don't try.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_UNIFORMITY_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_UNIFORMITY_H_

#include <cstdint>
#include <memory>
#include <optional>

#include "third_party/blink/public/mojom/permissions/permission.mojom-blink-forward.h"
#include "third_party/blink/renderer/core/core_export.h"
#include "ui/display/screen_info.h"

namespace gfx {
class SizeF;
}  // namespace gfx

namespace blink {

class ExecutionContext;
class Locale;
class LocalDOMWindow;
class PointerEventInit;
class WebPointerEvent;

namespace molt_fingerprinting {

// Whether `context` gets this file's new treatments: the process is in
// kUniform mode and `context` is web content (its top-level site is http(s)
// or opaque). A null context counts as web content (fail closed).
CORE_EXPORT bool UsesMoltNetUniformity(ExecutionContext* context);

// --- Touch -------------------------------------------------------------------

// TouchEventManager::CreateDomTouch, right before the Touch is created from
// `event` (the touch point in root-frame coordinates) and `radius` (CSS
// pixels). In MoltNet:
//   * screenX/Y become UniformScreenPoint() of the point, which is exactly
//     what MouseEvent and PointerEvent report for it: the position relative
//     to the viewport, as if the window sat at (0, 0) without browser chrome;
//   * in web content, radiusX/Y, rotationAngle and force become 0, the
//     Touch Events spec's values for "unknown": the contact size and
//     pressure tell digitizers (and fingers) apart.
CORE_EXPORT void UniformTouch(LocalDOMWindow* window,
                              WebPointerEvent& event,
                              gfx::SizeF& radius);

// End of UpdateCommonPointerEventInit (pointer_event_factory.cc): for touch
// pointers in MoltNet web content, the same contact shows through
// PointerEvent, so width/height become 1, pressure 0.5 while down (else 0),
// and tilt, twist and tangential pressure 0. Pens and mice are untouched.
CORE_EXPORT void UniformTouchPointerEventInit(LocalDOMWindow* window,
                                              const WebPointerEvent& event,
                                              PointerEventInit* init);

// --- Window Management API ---------------------------------------------------

// Whether window.getScreenDetails() rejects as if the window-management
// permission were denied ("NotAllowedError: Permission denied."), without
// asking the browser: no prompt, no stored permission state.
CORE_EXPORT bool HidesScreenDetails(LocalDOMWindow* window);

// Whether navigator.permissions reports `name` as "denied" in `context`,
// fixed for its lifetime (no "change" events): window-management in
// MoltNet web content, consistent with getScreenDetails(). Otherwise the
// state is the one MoltNet's profile inherits from the user's regular
// profile (a block there shows as "denied"), which differs between users.
CORE_EXPORT bool ReportsPermissionDenied(ExecutionContext* context,
                                         mojom::blink::PermissionName name);

// Whether real screen changes stay invisible: in MoltNet, window.screen
// reports a uniform screen and screen.orientation a fixed orientation, so
// moving the window to another monitor, changing its resolution or rotating
// it fires no "change" event (which would reveal a multi-monitor setup).
CORE_EXPORT bool HidesScreenChanges();

// --- Screen orientation ------------------------------------------------------

// screen.orientation.type and .angle in MoltNet: those of an unrotated
// display the size of the uniform screen (UniformScreenRect), i.e.
// landscape-primary and 0 unless the window is narrow enough for the
// rounded screen to be square or taller; landscape-primary without a frame.
// nullopt outside MoltNet.
CORE_EXPORT std::optional<display::mojom::ScreenOrientation>
UniformOrientationType(LocalDOMWindow* window);
CORE_EXPORT std::optional<uint16_t> UniformOrientationAngle(
    LocalDOMWindow* window);

// --- Form-control locale -----------------------------------------------------

// A Locale with fixed en-US data (moltnet_values.h): the date, time, month
// and week formats, month and weekday names, AM/PM and number symbols of
// form controls and validation messages. Configure(kUniform) makes it the
// process's Locale::DefaultLocale(), which form controls use; otherwise
// they follow the browser's UI language and, on macOS and Windows, the
// user's regional settings, even in MoltNet.
CORE_EXPORT std::unique_ptr<Locale> CreateUniformFormLocale();

// Called once by Configure(kUniform), on the main thread, before any
// document exists.
void PinUniformFormLocale();

// --- Tests -------------------------------------------------------------------

// Makes the web-content treatments above (UsesMoltNetUniformity() and
// HidesScreenChanges()) behave as in kUniform without configuring the
// process for it, which would pin the time zone and locale for every later
// test. The context must still be farbled, so tests configure kFarble. The
// screen-derived values (Touch.screenX/Y, the orientation) still need real
// kUniform, like UniformScreenRect().
CORE_EXPORT void SetMoltNetUniformityForTesting(bool uniform);

}  // namespace molt_fingerprinting
}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_UNIFORMITY_H_
