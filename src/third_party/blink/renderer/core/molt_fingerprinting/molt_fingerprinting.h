// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: the Blink side.
//
// Upstream Blink calls into this file from a handful of one-to-three-line
// hooks (each marked "MoltBrowser:"):
//
//   canvas   HTMLCanvasElement::ToDataURLInternal       -> FarbleSnapshot
//            CanvasAsyncBlobCreator ctor (toBlob,
//              OffscreenCanvas.convertToBlob)            -> FarbleSnapshot
//            BaseRenderingContext2D::getImageDataInternal -> FarblePixmap
//   webgl    WebGLRenderingContextBase::ReadPixelsHelper -> FarbleReadPixels
//            WebGLDebugRendererInfo::Supported           -> IsUniform
//   audio    AudioBuffer::getChannelData/copyFromChannel -> FarbleAudio
//            AnalyserNode::get{Float,Byte}{Frequency,TimeDomain}Data
//   navigator NavigatorBase::hardwareConcurrency        -> HardwareConcurrency
//   MoltNet  Screen::GetRect/colorDepth/isExtended,
//            LocalDOMWindow::outer{Width,Height}/screen{X,Y},
//            MouseEvent::SetCoordinatesFromWebPointerProperties
//
// The pure noise functions are in farbling.h. This file decides, per
// execution context, whether to farble and with which per-site key:
//
//   * documents: the top-level site of the window's storage key (plus its
//     nonce, if any, so fenced frames / credentialless iframes get their own
//     key);
//   * dedicated workers (nested too): the site of the top-level frame's
//     origin, which Blink already threads through to every dedicated worker;
//   * shared and service workers: Blink doesn't know their top-level site, so
//     they get a random key of their own (unlinkable, but not stable across
//     worker restarts) -- see docs/FINGERPRINTING.md;
//   * contexts whose top-level site isn't http(s) (chrome://, extensions)
//     are never farbled; opaque top-level sites get a random key.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLT_FINGERPRINTING_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLT_FINGERPRINTING_H_

#include <cstdint>
#include <optional>

#include "base/containers/span.h"
#include "base/memory/scoped_refptr.h"
#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/farbling.h"
#include "third_party/blink/renderer/platform/graphics/gpu/webgl_image_conversion.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/rect.h"

class SkPixmap;

namespace v8 {
class Isolate;
}  // namespace v8

namespace blink {

class ExecutionContext;
class LocalDOMWindow;
class LocalFrame;
class StaticBitmapImage;

namespace molt_fingerprinting {

enum class Mode { kOff, kFarble, kUniform };

// Process configuration, set once by WebMoltFingerprinting::Configure() on
// the main thread before any script runs (workers start later and only read
// it). In kUniform mode this also pins the time zone (Etc/UTC) and the ICU /
// Intl default locale (en-US) for the whole process.
CORE_EXPORT void Configure(Mode mode, base::span<const uint8_t> session_seed);
CORE_EXPORT Mode GetMode();
// True in MoltNet renderer processes.
CORE_EXPORT bool IsUniform();
CORE_EXPORT void ResetForTesting();

// The farbler for `context`, or nullopt when nothing should be farbled there
// (protection off, non-web context). Main thread or the context's thread.
CORE_EXPORT std::optional<Farbler> FarblerFor(ExecutionContext* context);

// --- Canvas -----------------------------------------------------------------

// toDataURL / toBlob / convertToBlob: returns `image` itself when nothing is
// farbled, else an unpremultiplied raster copy with the noise applied (the
// canvas's own snapshot is never modified).
CORE_EXPORT scoped_refptr<StaticBitmapImage> FarbleSnapshot(
    ExecutionContext* context,
    scoped_refptr<StaticBitmapImage> image);
// getImageData: farbles the freshly read, unpremultiplied ImageData pixels in
// place (RGBA/BGRA 8-bit, F16, F32; other formats are left alone).
CORE_EXPORT void FarblePixmap(ExecutionContext* context,
                              const SkPixmap& pixmap);
CORE_EXPORT void FarblePixmapWith(const Farbler& farbler,
                                  const SkPixmap& pixmap);

// --- WebGL ------------------------------------------------------------------

// readPixels into client memory. Only RGBA / UNSIGNED_BYTE reads are farbled
// (`is_rgba8`); the pack parameters give the row layout of `destination`.
CORE_EXPORT void FarbleReadPixels(
    ExecutionContext* context,
    bool is_rgba8,
    int width,
    int height,
    const WebGLImageConversion::PixelStoreParams& pack,
    base::span<uint8_t> destination);

// --- Web Audio --------------------------------------------------------------

CORE_EXPORT void FarbleAudio(ExecutionContext* context,
                             base::span<float> samples);
// `neutral` is never changed: 0 for frequency data, 128 for time-domain data.
CORE_EXPORT void FarbleAnalyserBytes(ExecutionContext* context,
                                     base::span<uint8_t> bytes,
                                     uint8_t neutral);
// The execution context of the script calling into a binding (AudioBuffer
// has no context of its own), or null outside a script call.
CORE_EXPORT ExecutionContext* ContextOfCurrentCall(v8::Isolate* isolate);

// --- navigator --------------------------------------------------------------

// kUniform: kUniformHardwareConcurrency. kFarble: a per-site value in
// [2, real]. Otherwise `real`.
CORE_EXPORT unsigned HardwareConcurrency(ExecutionContext* context,
                                         unsigned real);

// --- MoltNet uniformity -----------------------------------------------------

// In kUniform mode: the screen rect every Screen getter reports -- origin
// (0, 0), size = the outermost main frame's viewport in DIPs rounded down to
// the letterbox steps. nullopt otherwise.
CORE_EXPORT std::optional<gfx::Rect> UniformScreenRect(LocalFrame* frame);
// In kUniform mode: MouseEvent.screenX/Y for a pointer at
// `position_in_widget` (physical pixels): the same point in DIPs relative to
// the widget, i.e. as if the window sat at (0, 0) without browser chrome.
CORE_EXPORT std::optional<gfx::PointF> UniformScreenPoint(
    const LocalDOMWindow* window,
    const gfx::PointF& position_in_widget);

}  // namespace molt_fingerprinting
}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLT_FINGERPRINTING_H_
