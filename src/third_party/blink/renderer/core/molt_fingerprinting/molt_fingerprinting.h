// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: the Blink side.
//
// Upstream Blink calls into this file from a handful of one-to-four-line
// hooks (each marked "MoltBrowser:"):
//
//   canvas   HTMLCanvasElement::ToDataURLInternal       -> FarbleSnapshot
//            CanvasAsyncBlobCreator ctor (toBlob,
//              OffscreenCanvas.convertToBlob)            -> FarbleSnapshot
//            BaseRenderingContext2D::getImageDataInternal -> FarblePixmap
//            HTMLCanvasElement::NotifyListenersCanvasChanged
//              (captureStream frames)                    -> FarbleForExport
//   webgl    WebGLRenderingContextBase::ReadPixelsHelper -> FarbleReadPixels
//            WebGL2RenderingContextBase::readPixels(offset)
//              (into a PIXEL_PACK buffer)  -> FarblePixelPackBuffer
//            WebGLDebugRendererInfo::Supported           -> IsUniform
//   webcodecs VideoFrame::Create from a CanvasImageSource -> FarbleForExport
//   webgpu   GPUQueue::copyExternalImageToTexture        -> FarbleForExport
//   audio    AudioBuffer::getChannelData/copyFromChannel -> FarbleAudio
//            AnalyserNode::get{Float,Byte}{Frequency,TimeDomain}Data
//            AudioWorkletProcessor::Process (inputs)     -> FarbleAudio
//            ThreadedWorkletMessagingProxy::Initialize   -> ProvideToWorklet
//   navigator NavigatorBase::hardwareConcurrency        -> HardwareConcurrency
//   MoltNet  Screen::GetRect/colorDepth/isExtended,
//            LocalDOMWindow::outer{Width,Height}/screen{X,Y},
//            MouseEvent::SetCoordinatesFromWebPointerProperties
//   workers  WebSharedWorkerImpl::StartWorkerContext (storage key from
//              content's EmbeddedSharedWorkerStub)   -> ProvideToSharedWorker
//                                                       (shared_worker_key.h)
//   MoltNet  WebGL getParameter limits, getSupportedExtensions/getExtension,
//            getShaderPrecisionFormat, getInternalformatParameter(SAMPLES);
//            WebGPU GPU::RequestAdapterImpl          -> gpu_uniformity.h
//   fonts    FontFallbackList::GetFontData (system lookup)
//              -> FontSelector::GetSystemFontData, overridden by
//                 CSSFontSelectorBase               -> GetSystemFontData
//            CSSFontSelectorBase::IsPlatformFamilyMatchAvailable
//              (document.fonts.check())    -> IsSystemFontFamilyAvailable
//            LocalFontFaceSource::IsLocalFontAvailable,
//              CreateFontData                       -> AllowLocalFontFace,
//                                                      AllowInstalledFont
//            FontAccess::DidGetEnumerationResponse -> AllowEnumeratedFont
//            (all in system_fonts.h)
//   MoltNet  MediaValues::CalculateDeviceWidth/Height -> UniformScreenRect;
//            MediaValues::CalculateDevicePixelRatio,
//            LocalDOMWindow::devicePixelRatio, HTMLImageElement (srcset),
//            StyleResolverState (image-set())     -> DevicePixelRatioFor
//            (letterbox.h; the browser letterboxes the page itself)
//   MoltNet  Touch / PointerEvent (touch) coordinates and contact,
//            getScreenDetails(), the window-management permission,
//            screen and orientation "change" events, screen.orientation,
//            the form-control Locale            -> moltnet_uniformity.h
//   MoltNet  DefaultLanguage(), UserPreferredLanguages() (language.cc; the
//              default text locale)                 -> moltnet_locale.h
//   MoltNet  navigator.deviceMemory, subresource client hints
//            (frame_fetch_context.cc), navigator.userAgentData's
//            high-entropy values                -> moltnet_device.h
//
// The pure noise functions are in farbling.h. This file decides, per
// execution context, whether to farble and with which per-site key:
//
//   * documents: the top-level site of the window's storage key (plus its
//     nonce, if any, so fenced frames / credentialless iframes get their own
//     key);
//   * dedicated workers (nested too): the site of the top-level frame's
//     origin, which Blink already threads through to every dedicated worker;
//   * service and shared workers, first- and third-party: the top-level site
//     of their storage key (plus its nonce, if any), handed over by the
//     browser before any of their script runs (SetServiceWorkerStorageKey,
//     and ProvideToSharedWorker in shared_worker_key.h). A third-party worker
//     under a.example thus shares a.example's key with the pages it serves;
//   * worklets: the key of the document that created them, handed over at
//     creation (ProvideToWorklet);
//   * contexts whose top-level site isn't http(s) (chrome://, extensions)
//     are never farbled; opaque top-level sites get a random key;
//   * in kFarble mode, top-level sites the user turned MoltShield off for
//     ("Shields off", SetUnfarbledSites) are not farbled. MoltNet (kUniform)
//     ignores that list.
//
// The process gets its mode at start (command line) and its session seed a
// moment later over a channel-associated mojo message that is ordered
// before any navigation commit or service worker start. A context that
// needs a key before the seed has arrived (in practice only a shared worker
// racing the renderer's start-up) gets a random key for its lifetime: its
// values are unlinkable, just not stable.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLT_FINGERPRINTING_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLT_FINGERPRINTING_H_

#include <cstdint>
#include <optional>

#include <string>
#include <vector>

#include "base/containers/span.h"
#include "base/memory/scoped_refptr.h"
#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/farbling.h"
#include "third_party/blink/renderer/platform/graphics/gpu/webgl_image_conversion.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/rect.h"

class SkPixmap;

namespace gpu::gles2 {
class GLES2Interface;
}  // namespace gpu::gles2

namespace v8 {
class Isolate;
}  // namespace v8

namespace blink {

class BlinkStorageKey;
class ExecutionContext;
class Image;
class LocalDOMWindow;
class LocalFrame;
class StaticBitmapImage;
class WorkerClients;

namespace molt_fingerprinting {

enum class Mode { kOff, kFarble, kUniform };

// Process mode, set once by WebMoltFingerprinting::Configure() on the main
// thread before any script runs (workers start later and only read it). In
// kUniform mode this also pins the time zone (Etc/UTC) and the ICU / Intl
// default locale (en-US) for the whole process.
CORE_EXPORT void Configure(Mode mode);
// The profile's session seed (main thread). Only the first call with exactly
// kSessionSeedSize bytes has an effect; it is ignored when the mode is kOff.
CORE_EXPORT void SetSessionSeed(base::span<const uint8_t> session_seed);
// Top-level sites ("https://example.com", as BlinkSchemefulSite serializes
// them) with MoltShield turned off by the user: kFarble mode doesn't farble
// documents and workers of those sites. Replaces the previous list; any
// thread may read it concurrently.
CORE_EXPORT void SetUnfarbledSites(std::vector<std::string> sites);
CORE_EXPORT Mode GetMode();
// True in MoltNet renderer processes.
CORE_EXPORT bool IsUniform();
CORE_EXPORT void ResetForTesting();

// The farbler for `context`, or nullopt when nothing should be farbled there
// (protection off, non-web context, Shields off for the site). Main thread
// or the context's thread.
CORE_EXPORT std::optional<Farbler> FarblerFor(ExecutionContext* context);
CORE_EXPORT bool IsFarbled(ExecutionContext* context);
// Whether canvas frames leaving as video (captureStream(), new VideoFrame()
// from a canvas) are farbled for `context`. Farbling every frame costs a CPU
// pass per frame (about 16 ms per 720p camera-like frame on a slow VM), which
// canvas-effects video calls would pay all the time, so this is only on in
// MoltNet (uniform) windows. Normal windows keep the readback hooks
// (getImageData, toDataURL, readPixels, ...); see docs/FINGERPRINTING.md for
// the gap this leaves.
CORE_EXPORT bool FarblesVideoExports(ExecutionContext* context);

// --- Worklets ---------------------------------------------------------------

// Called on the main thread when `window` creates a threaded worklet (e.g.
// an AudioWorklet): the worklet will farble with `window`'s key, decided now.
CORE_EXPORT void ProvideToWorklet(LocalDOMWindow& window,
                                  WorkerClients* worker_clients);

// --- Service workers --------------------------------------------------------

// Called on a service worker's thread when its global scope is initialized,
// before any of its script runs: the worker farbles with the top-level site
// (and nonce) of its registration's storage key, so third-party service
// workers get their top-level site's key too.
CORE_EXPORT void SetServiceWorkerStorageKey(ExecutionContext& worker,
                                            const BlinkStorageKey& storage_key);

// --- Canvas -----------------------------------------------------------------

// toDataURL / toBlob / convertToBlob: returns `image` itself when nothing is
// farbled, else an unpremultiplied raster copy with the noise applied (the
// canvas's own snapshot is never modified).
CORE_EXPORT scoped_refptr<StaticBitmapImage> FarbleSnapshot(
    ExecutionContext* context,
    scoped_refptr<StaticBitmapImage> image);
// Canvas (and image) pixels leaving towards a video or GPU pipeline:
// VideoFrame construction, captureStream frames, WebGPU
// copyExternalImageToTexture. Returns `image` itself when nothing is
// farbled, else a raster copy with the noise applied and the *same* alpha
// type: 8-bit premultiplied images are farbled as stored (never moving a
// channel above alpha), float premultiplied ones unpremultiplied and then
// premultiplied again. Any thread with a context.
CORE_EXPORT scoped_refptr<StaticBitmapImage> FarbleForExport(
    ExecutionContext* context,
    scoped_refptr<StaticBitmapImage> image);
CORE_EXPORT scoped_refptr<Image> FarbleForExport(ExecutionContext* context,
                                                 scoped_refptr<Image> image);
// The copy FarbleForExport makes, or null if `image` can't be read back.
CORE_EXPORT scoped_refptr<StaticBitmapImage> FarbleForExportWith(
    const Farbler& farbler,
    Image& image);
// getImageData: farbles the freshly read, unpremultiplied ImageData pixels in
// place (RGBA/BGRA 8-bit, F16, F32; other formats are left alone).
CORE_EXPORT void FarblePixmap(ExecutionContext* context,
                              const SkPixmap& pixmap);
CORE_EXPORT void FarblePixmapWith(const Farbler& farbler,
                                  const SkPixmap& pixmap);

// --- WebGL ------------------------------------------------------------------

// readPixels into client memory. Farbled: RED, RG, RGB and RGBA with
// UNSIGNED_BYTE (FarbleUnorm8), FLOAT (FarbleFloat32) and HALF_FLOAT /
// HALF_FLOAT_OES (FarbleFloat16). Other formats and types (integer, packed
// 16-bit, 10/11-bit, norm16, ALPHA, LUMINANCE) are left alone. The pack
// parameters give the row layout of `destination`.
CORE_EXPORT void FarbleReadPixels(
    ExecutionContext* context,
    GLenum format,
    GLenum type,
    int width,
    int height,
    const WebGLImageConversion::PixelStoreParams& pack,
    base::span<uint8_t> destination);
CORE_EXPORT void FarbleReadPixelsWith(
    const Farbler& farbler,
    GLenum format,
    GLenum type,
    int width,
    int height,
    const WebGLImageConversion::PixelStoreParams& pack,
    base::span<uint8_t> destination);
// WebGL2 readPixels into the bound PIXEL_PACK_BUFFER at `offset`, called
// right after the read: maps the rows just written (read + write, at most a
// few MiB at a time), farbles them as above and writes them back, so
// getBufferSubData, copyBufferSubData and texture uploads from the buffer
// all see farbled pixels. Costs a synchronous round trip to the GPU process
// per farbled read.
CORE_EXPORT void FarblePixelPackBuffer(
    ExecutionContext* context,
    gpu::gles2::GLES2Interface* gl,
    GLenum format,
    GLenum type,
    int width,
    int height,
    const WebGLImageConversion::PixelStoreParams& pack,
    int64_t offset);
CORE_EXPORT void FarblePixelPackBufferWith(
    const Farbler& farbler,
    gpu::gles2::GLES2Interface* gl,
    GLenum format,
    GLenum type,
    int width,
    int height,
    const WebGLImageConversion::PixelStoreParams& pack,
    int64_t offset);

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

// --- Fonts (system_fonts.h) -------------------------------------------------

// FarblerFor(), plus whether its key is the context's stable per-site key
// (false for the random keys of opaque contexts and of contexts that needed
// a key before the seed arrived, and for worklets' handed-over keys).
CORE_EXPORT std::optional<Farbler> SiteFarblerFor(ExecutionContext* context,
                                                  bool* is_site_key);

// --- MoltNet uniformity -----------------------------------------------------

// In kUniform mode: the screen rect every Screen getter reports -- origin
// (0, 0), size = the outermost main frame's size in DIPs (scrollbars
// included, as innerWidth/innerHeight) rounded down to the letterbox steps
// (UniformScreenSizeOf in letterbox.h). nullopt otherwise.
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
