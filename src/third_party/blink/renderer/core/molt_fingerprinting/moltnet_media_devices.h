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

#include <string>

#include "base/functional/callback.h"
#include "third_party/blink/public/mojom/mediastream/media_devices.mojom-blink-forward.h"
#include "third_party/blink/public/mojom/mediastream/media_devices.mojom-shared.h"
#include "third_party/blink/public/mojom/mediastream/media_stream.mojom-shared.h"
#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"

namespace blink {

class LocalDOMWindow;
struct MediaStreamDevice;
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

// --- Names and groups once the page may see the devices ---------------------
//
// MoltNet's generic device names and groups (moltnet_media_values.h,
// DeviceLabel(), DeviceGroup(), ListsAudioDevice()), wherever a page reads
// them: enumerateDevices() (MediaDevices::DevicesEnumerated), a track's label
// (UserMediaProcessor::InitializeSourceObject), its getSettings() and
// getCapabilities() groups (UserMediaProcessor::OnStreamsGenerated and
// OnDeviceChanged), and the groups getUserMedia()'s `groupId` constraints are
// checked against (UserMediaProcessor's device capabilities). Elsewhere
// everything is unchanged.

// Whether enumerateDevices() lists `device` of `kind`, and getUserMedia() may
// select it: in MoltNet, not Windows' communications device.
CORE_EXPORT bool ListsMediaDevice(mojom::MediaDeviceType kind,
                                  const WebMediaDeviceInfo& device);
CORE_EXPORT bool ListsCaptureDevice(const String& device_id);

// enumerateDevices()' entry `device` of `kind` with MoltNet's label (where
// Chromium shows one) and group.
CORE_EXPORT void UseUniformNames(mojom::MediaDeviceType kind,
                                 WebMediaDeviceInfo& device);

// The label of a track of the camera or microphone `device` getUserMedia()
// opened; other capture (screens, windows, tabs) keeps its name. The device
// itself keeps the browser's name: the renderer matches the browser's
// messages about a device to its own copy by it (MediaStreamDevice::
// IsSameDevice()).
CORE_EXPORT std::string UniformName(const MediaStreamDevice& device);

// A camera or microphone getUserMedia() opened, with MoltNet's group (its
// tracks' getSettings() and getCapabilities()). Other capture is unchanged.
CORE_EXPORT void UseUniformGroup(MediaStreamDevice& device);

// The group a `groupId` constraint is checked against for the capture device
// `device_id` whose group Chromium reports as `group_id`.
CORE_EXPORT String UniformGroupId(const String& device_id,
                                  const String& group_id);

// enumerateDevices()' InputDeviceInfo.getCapabilities() of a microphone in
// MoltNet: its sampleRate, channelCount and latency ranges from the one
// format every MoltNet microphone has (media::PortableCaptureParameters()),
// and no platform effects. Elsewhere unchanged but for the effects
// (media::PortableCaptureEffects()).
CORE_EXPORT void UseUniformCapabilities(
    mojom::blink::AudioInputDeviceCapabilities& capabilities);

}  // namespace molt_fingerprinting
}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_MEDIA_DEVICES_H_
