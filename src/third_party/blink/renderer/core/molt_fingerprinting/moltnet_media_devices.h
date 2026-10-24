// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: which cameras, microphones and
// output devices a MoltNet page (kUniform) can tell exist before it may use
// them. Every MoltNet machine has one of each. The rules are in
// moltnet_media_values.h.
//
// Upstream hooks (each marked "MoltBrowser:"):
//
//   enumerateDevices()  MediaDevices::DevicesEnumerated (modules/mediastream/
//                         media_devices.cc)   -> ShouldListUniformDevice
//   devicechange        MediaDevices::MaybeFireDeviceChangeEvent (the same
//                         file)                -> DeviceChangeNeedsPermission
//   getUserMedia()      UserMediaProcessor::GetUserMediaRequestFailed
//                         (modules/mediastream/user_media_processor.cc)
//                                              -> ConfirmUserMediaFailure
//
// Before a permission Chromium lists one entry, with no ID or label, of each
// kind of device the machine has; MoltNet lists one of each kind on every
// machine. A device change fires `devicechange` only once the page may use
// the devices (Chromium's DeviceChangeRequiresPermission, which MoltNet keeps
// whatever the feature's state). And getUserMedia() for a kind the machine
// has none of, or with constraints no device meets, fails only once the user
// was asked, as a request a device could meet would be answered.
//
// Which contexts: windows (these APIs aren't in workers). Normal and
// Incognito windows are unchanged.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_MEDIA_DEVICES_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_MEDIA_DEVICES_H_

#include "base/functional/callback.h"
#include "third_party/blink/public/mojom/mediastream/media_devices.mojom-shared.h"
#include "third_party/blink/public/mojom/mediastream/media_stream.mojom-shared.h"
#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"

namespace blink {

class LocalDOMWindow;
struct WebMediaDeviceInfo;

namespace molt_fingerprinting {

// enumerateDevices() in MoltNet: whether to list one entry of `kind`, with no
// ID, label or group, after the entries of that kind Chromium listed in
// `enumeration` (indexed by mojom::MediaDeviceType): when it listed none and
// the page doesn't see that kind's devices (an entry of the kind, or for the
// audio kinds of either, has an ID; moltnet::ListsUniformDevice). false
// outside MoltNet.
CORE_EXPORT bool ShouldListUniformDevice(
    mojom::MediaDeviceType kind,
    const Vector<Vector<WebMediaDeviceInfo>>& enumeration);

// Whether `devicechange` fires only once the page may use the devices that
// changed, whatever the DeviceChangeRequiresPermission feature says: true in
// MoltNet.
CORE_EXPORT bool DeviceChangeNeedsPermission();

// getUserMedia() is about to fail with `result`. In MoltNet, when that is a
// missing device (NO_HARDWARE) or unmet constraints
// (CONSTRAINT_NOT_SATISFIED) for a camera (`video`) or microphone (`audio`)
// request, asks for those permissions first (the prompt of a request a
// device could meet; nothing is asked of a permission already granted or
// denied), returns true and calls `done` with the result to report:
// `result` if all were granted, else PERMISSION_DENIED or
// PERMISSION_DISMISSED (moltnet::UserMediaFailureVerdict). `done` isn't
// called if the window goes away first. Returns false (and drops `done`)
// otherwise: the failure stays as it is.
CORE_EXPORT bool ConfirmUserMediaFailure(
    LocalDOMWindow* window,
    bool audio,
    bool video,
    mojom::MediaStreamRequestResult result,
    base::OnceCallback<void(mojom::MediaStreamRequestResult)> done);

}  // namespace molt_fingerprinting
}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_MEDIA_DEVICES_H_
