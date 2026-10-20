# Fingerprinting protection (v1.1)

MoltBrowser's answer to audit finding **P3** ("no fingerprinting defense").
Everything happens in the engine (Blink and the browser process), not in
page scripts, so pages can't remove it and it covers workers too.

* **Normal and Incognito windows: farbling.** Device-dependent outputs
  (canvas and WebGL pixels, Web Audio samples, `hardwareConcurrency`) get
  small, deterministic noise keyed **per site and per session**. A site sees a
  stable fingerprint for the whole session (pages don't break or flicker);
  two sites see unrelated fingerprints; the next session sees new ones.
  Turning MoltShield off for a site ("Shields off") turns farbling off there
  too.
* **MoltNet (Tor) windows: uniformity + farbling.** Values that vary between
  users are replaced by one value for everybody (UTC, en-US, letterboxed
  screen metrics, 4 cores, no unmasked GPU strings), and the farbling above
  stays on, keyed by a seed that changes with every New Identity. MoltNet
  ignores "Shields off".

The design follows Brave's "farbling" and Tor Browser's uniformity
(letterboxing, UTC, en-US). No Brave or Tor Browser code is used.

What changed in v1.1: the readback paths that bypassed v1 (WebGL2 pack
buffers, float and narrow `readPixels`, `VideoFrame` from a canvas,
`captureStream`, WebGPU uploads from a canvas, `AudioWorklet`) are closed
or documented below (the two per-frame video paths only in MoltNet windows,
for performance); "Shields off" disables farbling per site; the session
seed left the renderer command line; service workers and first-party
shared workers farble with their site's key.

## Threat model

Protects against: websites and third-party scripts that read
device-dependent values to recognise the same browser across sites (or, in
MoltNet, to tell MoltNet users apart), including scripts running in iframes,
dedicated workers, service workers and audio worklets.

Does not protect against (see [Known gaps and attacks](#known-gaps-and-attacks)):

* an adaptive attacker who targets MoltBrowser's noise specifically (for
  instance by moving pixels into a WebGL2 transform-feedback buffer or a
  WebGPU storage buffer, see below);
* local attackers who can read a renderer's memory (the seed is no longer
  on the command line, but it is in every renderer of the profile);
* a compromised renderer;
* fingerprinting surfaces outside the list below (font fallback and text
  metrics, WebGL parameters, WebGPU rendering, `GPUAdapter.info`, ...).

## How it works

```
 browser process                                 renderer process
 ---------------                                 ----------------
 Profile (regular / Incognito / MoltNet)
   session seed: 32 random bytes, in memory
   only (BrowserContext user data)
        |
 ChromeContentBrowserClient::
   AppendExtraCommandLineSwitches ------------>  --molt-fingerprinting-mode=farble|uniform
   RenderProcessWillLaunch                       (ChromeContentRendererClient::
   (molt_fingerprinting_browser.cc)                RenderThreadStarted
        |                                          -> WebMoltFingerprinting::Configure(mode))
        |   channel-associated mojo,
        |   FingerprintingConfiguration:
        +-- SetSessionSeed(seed) ---------------->  WebMoltFingerprinting::SetSessionSeed
        +-- SetUnfarbledSites(Shields-off) ------>  WebMoltFingerprinting::SetUnfarbledSites
              (again on every change)                       |
                                                 Blink (core/molt_fingerprinting): per
                                                 execution context, key =
                                                 HMAC-SHA256(seed, top-level site)
                                                 -> hooks in canvas / WebGL / WebCodecs /
                                                    WebGPU / audio / navigator / screen /
                                                    window / mouse events
```

**Why one mode and seed per renderer process is exact.** A renderer process
only ever hosts one BrowserContext: `RenderProcessHostImpl::IsSuitableHost`
rejects any host whose BrowserContext differs from the one being navigated,
and the spare-renderer manager only hands out a spare whose BrowserContext
matches (both checked in the pinned sources). The only exception is
`--single-process`, which supports a single context anyway. A crashed
renderer is relaunched for the same context, so it gets the same seed.

**How the seed gets there.** The mode is not a secret (a MoltNet renderer
is recognisable by its sandbox and proxy settings anyway), so it stays a
command-line switch and Blink knows it from the first instant; the kill
switch and MoltNet's time zone / locale / screen uniformity work exactly as
in v1. The seed is a secret, so it travels over
`molt_ai.fingerprinting.mojom.FingerprintingConfiguration`, a
channel-associated interface (the same mechanism as Chrome's own
`chrome::mojom::RendererConfiguration`). It is sent from
`RenderProcessWillLaunch`, which `RenderProcessHostImpl::Init` calls after
creating the channel and before `InitializeRenderer`, any frame creation,
navigation commit or `SetUpEmbeddedWorkerChannelForServiceWorker`; channel
messages are delivered in order, so documents and service workers always
have it. Shared workers are started over their own pipe
(`SharedWorkerFactory`), so in principle one could start before the seed
is processed. A shared worker's key is only derived when the worker first
needs one (once its script has been fetched and runs), not when it starts,
so the seed only has to arrive before that first farbled read. Any context
that does need a key before the seed arrived gets a random key for its
whole life (fail closed: its values are unlinkable, just not stable). The
renderer exposes the interface only when its mode is farble or uniform.

**Seed lifetime.**

| Profile | Seed lives | New seed when |
|---|---|---|
| Regular | while the browser runs | relaunch |
| Incognito | while the Incognito profile exists | all Incognito windows closed |
| MoltNet (`MoltNet::Tor-<uuid>` OTR profile) | while that profile exists | New Identity (the profile is destroyed and recreated), MoltNet turned off and on |

**Per-site key.** `key = HMAC-SHA256(seed, "molt-fp-site-v1" 0x00 be32(len)
site be32(len) partition)` (`farbling.cc`, `DeriveSiteKey`), where:

| Execution context | `site` | `partition` |
|---|---|---|
| Document | top-level site of the window's storage key, e.g. `https://example.com` (scheme + eTLD+1) | the storage key's nonce, if any (fenced frames, credentialless iframes) |
| Dedicated worker (and workers nested in it) | site of the top-level frame's origin, which Blink already passes to every dedicated worker | - |
| Service worker (first- and third-party) | top-level site of its registration's storage key, handed to Blink in `ServiceWorkerGlobalScope::InitializeGlobalScope` before any script runs | the storage key's nonce, if any |
| Shared worker (first- and third-party) | top-level site of its storage key (the constructor's, which the browser sends when it starts the worker), handed to Blink in `WebSharedWorkerImpl::StartWorkerContext` before the worker thread starts (see [Shared workers](#shared-workers)) | the storage key's nonce, if any |
| Worklet (e.g. `AudioWorklet`) | the creating document's key, handed over at creation (`ThreadedWorkletMessagingProxy::Initialize`) | (document's) |
| Top-level site opaque (e.g. CSP `sandbox` top-level document) | random per-context key | - |
| Top-level site not http(s) (`chrome://`, extensions, `file://`) | **not farbled** | - |

Third-party iframes therefore use the **top-level** site's key: a tracker
embedded on two sites sees two unrelated fingerprints.

## Shared workers

A shared worker farbles with exactly the key a document with its storage
key gets: the storage key's top-level site, plus its nonce (if any) as the
partition. A shared worker's storage key is the one of the context that
constructed it, and storage partitioning gives every top-level site its own
instance. So a tracker's shared worker started from its iframes on
a.example farbles with a.example's key, like a.example's pages and the
tracker's iframes there; the same script under b.example is a different
worker and gets b.example's key. The key no longer changes when the worker
restarts, only with the session seed. First-party shared workers get the
same key as before: their top-level site is their own site.

**Where the storage key comes from.** Blink is only told a shared worker's
constructor *origin*, so the storage key travels alongside it, all of it
from the browser:

1. `SharedWorkerHost::Start` already sends the creator's storage key
   (`SharedWorkerInstance::creator_storage_key()`, computed from the
   creating frame) to the worker's renderer as `constructor_key` in
   `SharedWorkerFactory.CreateSharedWorker`. No mojo change.
2. `EmbeddedSharedWorkerStub` (content) passes it to
   `WebSharedWorker::CreateAndStart`, which has one new argument for it.
3. `WebSharedWorkerImpl::StartWorkerContext` hands it to the new worker's
   `WorkerClients` on the main thread, before the worker thread starts
   (`ProvideToSharedWorker` in `molt_fingerprinting/shared_worker_key.{h,cc}`),
   the way worklets get their document's key.
4. On the worker thread, the first farbled read resolves the key from it
   with the same rules as for documents: Shields off and non-http(s)
   top-level sites are not farbled, an opaque top-level site gets a random
   key, and so does a worker that needs one before the seed arrived.

Nothing the page controls (the worker's URL, name or options) goes into the
key.

**Choices.**

* `data:` URL shared workers run with an opaque origin (and, depending on
  a Chromium feature, a storage key derived from the constructor's). They
  farble with the constructor's key, like `data:` URL dedicated workers,
  which use the top-level frame's site.
* A first-party shared worker created with `sameSiteCookies: 'none'` used
  to get a random key (its site for cookies is null); it now gets its
  site's key like any other first-party worker.
* A shared worker created through the Storage Access API
  (`requestStorageAccess({SharedWorker: true})`, then
  `handle.SharedWorker()`) is a first-party worker of the embedded site and
  farbles with that site's key, which the embedded frame on another site
  can then read through it. The frame only gets that worker once it holds
  storage access, the same permission that unlocks the site's first-party
  cookies and storage, so this links nothing new.
* Where third-party storage partitioning is off for an origin (enterprise
  policy), its storage keys carry its own site as the top-level site, so
  its shared workers get that site's key, the same as its frames.

**Verification.** The glue and `shared_worker_key.cc` type-check with the
strict clang flags against stand-ins of the 152 signatures, and the scratch
harness runs them under ASan/UBSan: 8 shared-worker cases on top of the 21
earlier ones (first-party key unchanged, third-party under two top-level
sites, restarts, nonce, opaque top-level site, seed late, Shields off,
non-web top-level sites, `sameSiteCookies: 'none'`, nothing handed over
when off). Five of them fail when the new lookup is removed.
`shared_worker_key_test.cc` (`MoltSharedWorkerKeyTest.*`, real worker
threads) and the upstream hooks (`web_shared_worker.h`,
`web_shared_worker_impl.{h,cc}`,
`content/renderer/worker/embedded_shared_worker_stub.cc`) need the build
machine.

## Shields off

MoltShield's per-site switch is stored as an explicit `ADS` content-settings
exception (`molt_shield_site_settings.h`). In normal and Incognito windows,
farbling is off for every top-level site the user turned Shields off for:
the page, its iframes (they use the top-level site), its dedicated workers,
worklets it creates and service and shared workers under that site
(third-party ones included).
MoltNet keeps farbling regardless: there the point is to look like every
other MoltNet user, and the real canvas/audio values would link New
Identity sessions on that site. MoltShield's *global* switch and the `ADS`
*default* don't affect farbling (independence from `--disable-molt-shield`
is deliberate, see the kill switch below).

* **Which exceptions count.** The browser turns each Shields-down exception
  into its top-level site (`net::SchemefulSite` of the pattern's host,
  for each scheme the pattern allows) and keeps it only if that site's own
  URL has Shields off (`molt_shield_site_exceptions.cc`). The site-scoped
  `https://[*.]example.com` the MoltShield UI writes therefore turns
  farbling off on all of example.com; a narrower exception (one subdomain)
  doesn't, because the farbling key is shared by the whole site.
* **Propagation.** The list goes to every renderer of the profile at launch
  and again whenever it changes (a content-settings observer per farbling
  profile), over the same ordered channel as the seed. Blink matches it
  against each context's resolved top-level site on every read, so a reload
  after toggling Shields commits with the new state, and already-open pages
  switch on their next read.
* **Trade-off.** Every renderer of the profile learns the whole list, so a
  compromised renderer could read which sites the user exempted. The
  stricter design (a per-document flag at commit, plumbed to every kind of
  worker) was not worth the extra upstream surface yet.

Shields off is also the per-site escape hatch for the costs below (canvas
video pipelines, PBO readback).

## Surface covered

| API | Normal windows | MoltNet windows | Hook (upstream file: function) |
|---|---|---|---|
| `canvas.toDataURL()` | farbled | farbled | `html_canvas_element.cc`: `ToDataURLInternal` |
| `canvas.toBlob()`, `OffscreenCanvas.convertToBlob()` (windows and workers) | farbled | farbled | `canvas_async_blob_creator.cc`: constructor |
| `getImageData()` (2D and OffscreenCanvas 2D; unorm8, float16, float32 storage) | farbled | farbled | `base_rendering_context_2d.cc`: `getImageDataInternal` |
| WebGL 1/2 `readPixels()` into client memory: `RED`, `RG`, `RGB`, `RGBA` × `UNSIGNED_BYTE`, `FLOAT`, `HALF_FLOAT(_OES)` | farbled | farbled | `webgl_rendering_context_base.cc`: `ReadPixelsHelper` |
| WebGL2 `readPixels()` into a `PIXEL_PACK_BUFFER` (then `getBufferSubData`, `copyBufferSubData`, texture uploads from it), same formats | farbled (in the buffer) | farbled | `webgl2_rendering_context_base.cc`: `readPixels(..., offset)` |
| `new VideoFrame(canvas \| OffscreenCanvas \| ImageBitmap \| <img> \| SVG image)`, then `copyTo()`, `VideoEncoder`, `MediaStreamTrackGenerator`, ... | **not farbled** (per-frame cost; see below) | farbled (at creation) | `video_frame.cc`: `VideoFrame::Create` (CanvasImageSource) |
| `canvas.captureStream()` frames (`MediaRecorder`, `ImageCapture.grabFrame()`, `MediaStreamTrackProcessor`, `<video>`, WebRTC) | **not farbled** (per-frame cost; see below) | farbled (at capture) | `html_canvas_element.cc`: `NotifyListenersCanvasChanged` |
| WebGPU `queue.copyExternalImageToTexture()` from a canvas, `OffscreenCanvas`, `ImageBitmap`, `<img>`, `ImageData` | farbled (before upload) | farbled | `gpu_queue.cc`: `copyExternalImageToTexture` |
| `OffscreenCanvas.transferToImageBitmap()`, `createImageBitmap(canvas)` | covered where the bitmap is read (see below) | same | - |
| HTML-in-canvas (`drawElementImage`, WebGL `texElementImage2D`, WebGPU `copyElementImageToTexture`) | **off**: the `HTMLInCanvas` origin trial's tokens are refused (only `chrome://flags` turns it on) | same | `origin_trial_policy_impl.cc`: `IsFeatureDisabled` |
| `WEBGL_debug_renderer_info` | real | **unavailable** (null from `getExtension`, absent from `getSupportedExtensions`) | `webgl_debug_renderer_info.cc`: `Supported` |
| WebGL limits (`getParameter` `MAX_*`, ranges, `MAX_VIEWPORT_DIMS`, ...), `getShaderPrecisionFormat`, `getInternalformatParameter(SAMPLES)` | real | **fixed baselines**, never above the real value ([details](#webgl-and-webgpu-in-moltnet-windows)) | `webgl_rendering_context_base.cc`, `webgl2_rendering_context_base.cc` |
| WebGL extensions (`getSupportedExtensions`, `getExtension`) | real | **fixed allow-list of 23**, intersected with the GPU's | `webgl_rendering_context_base.cc`: `ExtensionSupportedAndAllowed` |
| WebGPU `navigator.gpu.requestAdapter()` | real | **null** (no adapter) | `gpu.cc`: `RequestAdapterImpl` |
| Installed fonts: CSS `font-family`, canvas text, `document.fonts.check()`, `local()` | per-OS allow-list + 1 in 16 of the others, per site | **per-OS allow-list** ([details](#fonts)) | `font_fallback_list.cc`, `font_selector.{h,cc}`, `css_font_selector_base.{h,cc}`, `local_font_face_source.cc` |
| `queryLocalFonts()` (after its permission prompt) | filtered like CSS | filtered like CSS | `font_access.cc`: `DidGetEnumerationResponse` |
| `AudioBuffer.getChannelData()`, `copyFromChannel()` (incl. `OfflineAudioContext` results and `ScriptProcessorNode`'s `inputBuffer`/`outputBuffer`) | farbled | farbled | `audio_buffer.cc` (binding entry points only) |
| `AudioWorkletProcessor.process()` `inputs` | farbled | farbled | `audio_worklet_processor.cc`: `CopyPortToArrayBuffers` |
| `AnalyserNode.get{Float,Byte}{Frequency,TimeDomain}Data()` | farbled | farbled | `analyser_node.cc` |
| `navigator.hardwareConcurrency` (window and workers) | per site, in [2, real] | **4** | `navigator_base.cc`: `hardwareConcurrency` |
| `screen.width/height/availWidth/availHeight` | real | viewport in DIPs rounded **down** to 200x100 steps, max 1400x900 | `screen.cc`: `GetRect` |
| `screen.availLeft/availTop` | real | **0** | `screen.cc`: `GetRect` |
| `screen.colorDepth/pixelDepth`, `screen.isExtended` | real | **24**, **false** | `screen.cc` |
| `outerWidth/outerHeight` | real | = `innerWidth/innerHeight` | `local_dom_window.cc` |
| `screenX/screenY/screenLeft/screenTop` | real | **0** | `local_dom_window.cc` |
| `MouseEvent`/`PointerEvent`/`WheelEvent`/`DragEvent` `.screenX/Y` | real | relative to the viewport (as if the window sat at 0,0 with no browser chrome) | `mouse_event.cc`: `SetCoordinatesFromWebPointerProperties` |
| Page size (`innerWidth/innerHeight`, layout, width/height media queries) | real | **letterboxed**: the page area itself is rounded down to the screen steps above and centred ([details](#letterboxing-moltnet)) | browser: `contents_web_view.cc`: `GetInsets` |
| `device-width/device-height` media queries | real | = the uniform screen | `media_values.cc` |
| `devicePixelRatio`, `resolution` media queries, `srcset`/`<picture>`/`image-set()` selection | real | **1** | `local_dom_window.cc`, `media_values.cc`, `html_image_element.cc`, `style_resolver_state.cc` |
| Page zoom | regular profile's | **100%**, not inherited from the regular profile | `off_the_record_profile_impl.cc`: `Init` |
| `Touch.screenX/Y`, touch contact (`radiusX/Y`, `rotationAngle`, `force`; touch `PointerEvent` `width/height/pressure/tilt/twist`) | real | like `MouseEvent`; contact **hidden** ([details](#touches)) | `touch_event_manager.cc`, `pointer_event_factory.cc` |
| `getScreenDetails()`, `window-management` permission state | real | **denied** (no prompt) | `window_screen_details.cc`, `permission_status_listener.cc` |
| `screen` and orientation `change` events | real | **never fired** | `screen.cc`, `screen_orientation_controller.cc` |
| `screen.orientation.type/angle` | real | from the uniform screen, angle **0** | `screen_orientation.cc` |
| Date/time/number form-control formats | UI language and region | **en-US** | `platform_locale.cc`: `DefaultLocale` |
| Time zone (`Date`, `Intl`) | real | **Etc/UTC** (renderer-wide `TimeZoneController` override) | glue: `Configure` |
| `Intl` / ICU default locale | real | **en-US** (`LocaleController` override) | glue: `Configure` |
| `Accept-Language`, `navigator.language(s)` | real | **en-US** (MoltNet profile's `intl.accept_languages`) | `molt_net_routing.cc` (MoltBrowser file) |

"Farbled" in normal windows means: unless Shields are off for the site.

The screen "viewport" is the outermost main frame's visible viewport, so every
frame of a page (cross-process iframes included) reports the same screen.

**Where the hooks sit, and why.** Readback (`getImageData`, `readPixels`,
`toDataURL`, `getChannelData`) is farbled at the sink, as in v1. Pixels that
leave a canvas towards a *video or GPU pipeline* are farbled at the source
instead (`VideoFrame` creation, `captureStream`, `copyExternalImageToTexture`):
those pipelines have many sinks that hand bits back to script (`copyTo`,
`VideoEncoder` chunks, `MediaRecorder` blobs, `MediaStreamTrackProcessor`,
WebRTC to the page's own server, WebGPU buffers), and a frame that is
already farbled covers all of them with one hook. `<video>` and `VideoFrame`
sources are not farbled there (they are camera/decoded video, not device
rendering); a `<video>` playing a canvas `captureStream` carries
already-farbled frames.

**ImageBitmap / `transferToImageBitmap`.** An `ImageBitmap` is not a
readback: script can only get its pixels by drawing it into a 2D canvas
(`getImageData`/`toDataURL`/`toBlob`), uploading it to WebGL (`readPixels`),
wrapping it in a `VideoFrame`, or uploading it to WebGPU. All four are
hooked, so `OffscreenCanvas.transferToImageBitmap()` drawn into a farbled
canvas, or passed anywhere else, is covered by the readback at the end. The
noise is idempotent, so a bitmap that went through two hooks is not noised
twice. (`ImageBitmapRenderingContext.transferFromImageBitmap` shows the
bitmap in a canvas, whose readbacks are hooked.)

## The noise

All noise lives in `third_party/blink/renderer/core/molt_fingerprinting/farbling.{h,cc}`
(depends only on //base and //crypto) and is a pure function of the per-site
key and the **value** being read: never of its position, the size of the
read, or the rest of the buffer.

* **Canvas / WebGL pixels** (unpremultiplied RGBA; BGRA, float16 and float32
  canvas data are mapped onto the same 8-bit decision): fully transparent
  pixels are never touched (blank-canvas checks such as signature pads keep
  working). For each colour, one RGB channel is picked from its alpha; that
  channel's 256 values are grouped into pairs whose alignment ({2k,2k+1} or
  {2k+1,2k+2}) depends on the key and the other channels, and one pair in
  eight is collapsed onto one of its two values. Net effect: about 1 colour
  in 16 (6.3% measured) moves by exactly 1 in one channel; everything else is
  untouched; flat areas stay flat.
  * Same answer for `toDataURL`, `toBlob`, and every `getImageData`
    sub-rectangle of the same canvas, and for the same drawing repeated
    anywhere on the canvas.
  * Idempotent: farbling farbled data changes nothing, so
    `getImageData` -> `putImageData` -> `getImageData` loops never drift.
  * Some moves cross a `>> 1` boundary (3 -> 4), so dropping the lowest bit
    doesn't undo it.
  * The canvas's own backing store and cached snapshot are never modified;
    `toDataURL`/`toBlob` encode a farbled copy.
* **WebGL narrow reads** (`RED`, `RG`, `RGB` with `UNSIGNED_BYTE`): the
  missing components take their GL defaults for the decision (G = B = 0,
  A = 255), so a `RED` read returns exactly the R channel an `RGBA` read of
  the same data returns. (With alpha fixed at 255 the key picks one channel
  for the whole read; a `RED`/`RG` read is only noised when that channel is
  present, exactly like the R/G channels of the matching `RGBA` read.)
* **Exported frames** (`VideoFrame`, `captureStream`, WebGPU uploads): a
  raster copy with the source's alpha type. 8-bit premultiplied images (the
  normal canvas case) are farbled as stored, with the same decisions on the
  premultiplied tuple, except that a channel is never moved above its alpha
  (still idempotent); unpremultiplied and opaque images get the canvas noise
  above; float premultiplied images are farbled unpremultiplied and
  premultiplied again. Transparent canvases keep producing premultiplied
  frames, so nothing changes for them besides the noise.
* **WebGL `FLOAT` reads**: the audio noise below (the low 8 of 23 mantissa
  bits, relative change < 2^-15) instead of 8-bit snapping, which would
  corrupt GPGPU results (WebGL compute libraries read float results back
  this way).
* **WebGL `HALF_FLOAT` reads**: the pair collapse on the 15-bit magnitude:
  about 1 value in 16 moves by exactly one ulp; zero, infinities and NaN
  never move, and nothing moves to zero or to infinity. (Replacing low
  mantissa bits, as for float32, would need several of the 10 bits to hide
  anything and would visibly distort half-float results.)
* **Audio samples** (`AudioBuffer`, `AudioWorklet` inputs): the low 8 bits of
  the 23-bit mantissa are replaced by key-derived bits of the rest of the
  sample: a relative change below 2^-15 (about -90 dB, inaudible), far larger
  than the 1-2 ulp differences between machines that audio fingerprints rely
  on. Zero, subnormal, infinite and NaN samples are left alone (silence stays
  silent). Idempotent, so repeated `getChannelData()` calls never compound.
  `getChannelData()` returns the live array, so the noise is applied in
  place; the common "get the array, then write samples" pattern leaves the
  page's own samples untouched until its next `getChannelData()` call.
  `AudioWorklet` inputs are farbled in the copies handed to script; the
  graph's own buses (and what you hear) are untouched.
* **AnalyserNode byte data**: the same pair-collapse on byte values, never
  touching 0 (frequency data) or 128 (silence in time-domain data).
* **hardwareConcurrency**: `2 + PRF(key) mod (real - 1)`, i.e. in [2, real].
  MoltNet uses 4: one constant for everybody is the point, and 4 is at or
  below the logical core count of essentially every machine MoltBrowser runs
  on, so pages sizing worker pools from it don't oversubscribe weak machines,
  while 2 would needlessly halve throughput on most.

**Cost** (measured with a scratch benchmark of `farbling.cc`, `-O2`, median
of 7 runs, on the 2.1 GHz VM these changes were developed on, where a warm
8 MB `memcpy` takes about 0.7 ms; expect a fraction of this on a current
Mac):

| Path | Content | Time |
|---|---|---|
| canvas readback, 1920x1080 RGBA8 | flat / text-like / every pixel distinct | ~3 / ~10 / ~35-50 ms |
| exported frame (premultiplied), 640x360 | canvas-like / camera-like | 0.8 / 5.2 ms |
| exported frame, 1280x720 | canvas-like / camera-like | 3.3 / 16 ms |
| exported frame, 1920x1080 | canvas-like / camera-like | 7.8 / 42 ms |
| `readPixels` 1024x1024 | `RGB`/`UNSIGNED_BYTE`, `RGBA`/`FLOAT`, `RGBA`/`HALF_FLOAT` | 18, 17, 60 ms |
| `AudioWorklet` render quantum | 128 frames, stereo | 0.8 µs |
| 10 s of 44.1 kHz audio | `getChannelData` | ~1.4 ms |

On top of the CPU work: exported frames from accelerated canvases need a
synchronous GPU readback and become CPU-backed (`captureStream` loses its
one-copy GPU path while farbling), and every farbled `readPixels` into a
`PIXEL_PACK_BUFFER` costs one synchronous GPU-process round trip (the
mapping), so the async-readback idiom loses its asynchrony. Pages that
stream a canvas as video (canvas recording, effects pipelines in video
calls that build frames with `new VideoFrame(canvas)` or `captureStream()`)
would pay the export cost on every frame: at 720p on this VM that is about
half a core at 30 fps. **So the two per-frame hooks (`VideoFrame` from a
canvas, `captureStream`) only run in MoltNet windows** (`FarblesVideoExports()`
in `molt_fingerprinting.h`), and normal windows keep the one-copy GPU capture
path. The cost of that choice: in normal windows a script can still read
unfarbled canvas pixels by wrapping the canvas in a `VideoFrame` and calling
`copyTo()`, or through a `captureStream()` track processor. Planned fix:
farble at those readback points (`VideoFrame.copyTo`, `MediaStreamTrackProcessor`
frames) instead of on every exported frame, then turn them on everywhere.
WebGPU uploads from a canvas stay farbled in both (they are per upload, not
per frame; camera `<video>`/`VideoFrame` sources are not farbled). Nothing is
copied or computed when protection is off for the context.

## WebGL and WebGPU in MoltNet windows

WebGL's pixels are farbled everywhere and MoltNet hides its unmasked GPU
strings, but WebGL also describes the GPU through dozens of limits, an
extension list and shader precision formats, and WebGPU through
`GPUAdapter.info`, limits and features. In MoltNet windows (kUniform) all of
that is now the same for every user. Normal and Incognito windows are
unchanged (see [Scope](#scope-moltnet-only) below).

### WebGL 1 and 2

* **Limits.** `getParameter` reports a fixed baseline for every
  implementation-dependent limit, as `min(real, baseline)`: a page is never
  told about a capability the GPU lacks, and every GPU that meets the
  baseline reports exactly the baseline. Where the larger number is the
  weaker claim (`UNIFORM_BUFFER_OFFSET_ALIGNMENT`, `MIN_PROGRAM_TEXEL_OFFSET`,
  the low end of the point/line ranges) it is `max(real, baseline)`. State
  (`VIEWPORT`, `DEPTH_RANGE`, `SAMPLES`, blend state, ...) is never changed.
* **Extensions.** `getSupportedExtensions()` and `getExtension()` see a fixed
  allow-list of 23 common extensions, intersected with what the GPU
  supports (both go through `ExtensionSupportedAndAllowed`, so they always
  agree).
* **Shader precision.** `getShaderPrecisionFormat` reports fixed formats,
  clamped component-wise to the real ones.
* **Sample counts.** WebGL 2's `getInternalformatParameter(RENDERBUFFER, f,
  SAMPLES)` lists no count above the reported `MAX_SAMPLES` (4).

**How the baselines were picked.** Every baseline is at least the WebGL 2
(OpenGL ES 3.0) minimum, so a clamped report is still a valid one, and at
most what every desktop GPU that runs WebGL 2 in current Chromium reports
through ANGLE (D3D11 feature level 10_0 and up, every Metal Mac, current
Mesa and NVIDIA drivers), so that nearly every MoltNet user reports the
baseline itself. Within that range: where pages size their work from a limit
(textures, uniforms, varyings, texture units, attributes, draw buffers) the
baseline is the realistic desktop floor, because the spec minimums are low
enough to push engines onto slower paths or smaller textures; very high
values (16384-pixel textures, 4096 uniform vectors) would single out older
and mobile GPUs. Where pages don't (performance hints, LOD bias, server wait
timeout) the baseline is the spec minimum or a deliberately low value. The
coverage figures below are from web3dsurvey.com's WebGL 2 data (October
2026, all browsers and platforms; desktop-only coverage is higher).

| Parameter(s) | Baseline | WebGL 2 minimum | Why |
|---|---|---|---|
| `MAX_TEXTURE_SIZE`, `MAX_CUBE_MAP_TEXTURE_SIZE`, `MAX_RENDERBUFFER_SIZE` | 8192 | 2048 | ~95% of devices and every desktop GPU have 8192 (D3D10-class GPUs stop there); 16384 only ~80%. 4096 would halve the largest texture |
| `MAX_VIEWPORT_DIMS` | 8192 x 8192 | the renderbuffer size | consistent with the renderbuffer size, as ES 3.0 requires |
| `MAX_VERTEX_ATTRIBS` | 16 | 16 | ANGLE's cap everywhere |
| `MAX_VERTEX_UNIFORM_VECTORS`, `MAX_FRAGMENT_UNIFORM_VECTORS` | 1024 | 256, 224 (WebGL 1: 128, 16) | Metal reports 1024, the other desktop backends at least that (~95% of all devices have 512 or more); lower values cut e.g. the skinning bones engines put in uniforms |
| `MAX_VERTEX_UNIFORM_COMPONENTS`, `MAX_FRAGMENT_UNIFORM_COMPONENTS` | 4096 | 1024, 896 | 4 x the vectors |
| `MAX_VARYING_VECTORS` | 30 | 15 | ~95% of devices have at least 30, about a third more; 15 is tight for big material shaders |
| `MAX_VARYING_COMPONENTS`, `MAX_VERTEX_OUTPUT_COMPONENTS`, `MAX_FRAGMENT_INPUT_COMPONENTS` | 120 | 60, 64, 60 | 4 x the varyings |
| `MAX_TEXTURE_IMAGE_UNITS`, `MAX_VERTEX_TEXTURE_IMAGE_UNITS` | 16 | 16 | 99.95% have 16 or more, only ~20% have 24 |
| `MAX_COMBINED_TEXTURE_IMAGE_UNITS` | 32 | 32 | the sum of the two |
| `MAX_DRAW_BUFFERS`, `MAX_COLOR_ATTACHMENTS` (also `WEBGL_draw_buffers`) | 8 | 4 | every desktop GPU (4 on some mobile ones) |
| `SUBPIXEL_BITS` | 4 | 4 | the minimum; drivers report 4 or 8 |
| `ALIASED_POINT_SIZE_RANGE` | [1, 255] | [1, 1] | about Intel's maximum, the lowest on desktop |
| `ALIASED_LINE_WIDTH_RANGE` | [1, 1] | [1, 1] | D3D11, Metal and Vulkan only draw 1-pixel lines |
| `MAX_TEXTURE_MAX_ANISOTROPY_EXT` | 16 | 2 | universal on desktop |
| `MAX_3D_TEXTURE_SIZE`, `MAX_ARRAY_TEXTURE_LAYERS` | 2048 | 256 | D3D11's and Metal's value; ~98.5% or more of devices have it |
| `MAX_SAMPLES` | 4 | 4 | the minimum; higher counts (8, 16, 32) vary between GPUs |
| `MAX_VERTEX_UNIFORM_BLOCKS`, `MAX_FRAGMENT_UNIFORM_BLOCKS` | 12 | 12 | the minimums |
| `MAX_COMBINED_UNIFORM_BLOCKS`, `MAX_UNIFORM_BUFFER_BINDINGS` | 24 | 24 | the minimums |
| `MAX_UNIFORM_BLOCK_SIZE` | 16384 | 16384 | the minimum; about a fifth of Chromium's WebGL 2 users have less than 65536 |
| `MAX_COMBINED_{VERTEX,FRAGMENT}_UNIFORM_COMPONENTS` | 53248 | (formula) | ES 3.0's formula over the values above |
| `UNIFORM_BUFFER_OFFSET_ALIGNMENT` | 256 (`max`) | at most 256 | every real alignment divides 256 |
| `MIN_PROGRAM_TEXEL_OFFSET`, `MAX_PROGRAM_TEXEL_OFFSET` | -8 (`max`), 7 | -8, 7 | the minimums |
| `MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS`, `_SEPARATE_ATTRIBS`, `_SEPARATE_COMPONENTS` | 64, 4, 4 | 64, 4, 4 | the minimums |
| `MAX_ELEMENT_INDEX` | 2^32 - 1 | 2^24 - 1 | 32-bit indices on every desktop GPU |
| `MAX_ELEMENTS_VERTICES`, `MAX_ELEMENTS_INDICES` | 2048 | none | performance hints nothing enforces |
| `MAX_TEXTURE_LOD_BIAS` | 2.0 | 2.0 | only bounds a shader's LOD bias |
| `MAX_SERVER_WAIT_TIMEOUT` | 0 | 0 | WebGL 2 only allows `TIMEOUT_IGNORED` |

Extension parameters (`MAX_VIEWS_OVR`, `MAX_DUAL_SOURCE_DRAW_BUFFERS_EXT`,
`MAX_CLIP_DISTANCES_WEBGL`, pixel-local-storage planes, interpolation
offsets) are unreachable: their extensions aren't on the allow-list.

**Extensions allowed** (when supported): `ANGLE_instanced_arrays`,
`EXT_blend_minmax`, `EXT_color_buffer_float`, `EXT_color_buffer_half_float`,
`EXT_frag_depth`, `EXT_sRGB`, `EXT_shader_texture_lod`,
`EXT_texture_filter_anisotropic`, `KHR_parallel_shader_compile`,
`OES_element_index_uint`, `OES_standard_derivatives`, `OES_texture_float`,
`OES_texture_float_linear`, `OES_texture_half_float`,
`OES_texture_half_float_linear`, `OES_vertex_array_object`,
`WEBGL_color_buffer_float`, `WEBGL_compressed_texture_s3tc`,
`WEBGL_compressed_texture_s3tc_srgb`, `WEBGL_depth_texture`,
`WEBGL_draw_buffers`, `WEBGL_lose_context`, `WEBGL_multi_draw` (the ones
that are core in WebGL 2 only show up in WebGL 1). Everything else is
hidden, notably:

* `WEBGL_debug_renderer_info` (still also hidden by its own hook) and
  `WEBGL_debug_shaders`, whose `getTranslatedShaderSource()` shows the
  backend's shader language and the driver-bug workarounds ANGLE applies
  for the specific GPU;
* `EXT_disjoint_timer_query(_webgl2)`: GPU timers;
* extensions that differ between GPUs of one platform: `EXT_float_blend`
  (Apple GPUs can't blend 32-bit float targets; ~88% of macOS devices have
  it) and the BPTC, RGTC, ASTC, ETC/ETC1 and PVRTC texture formats (ASTC
  alone would tell Apple-silicon Macs from Intel ones);
* the newer and rarely used ones (`EXT_clip_control`, `EXT_depth_clamp`,
  `EXT_polygon_offset_clamp`, `EXT_texture_mirror_clamp_to_edge`,
  `EXT_texture_norm16`, `EXT_render_snorm`, `EXT_conservative_depth`,
  `OES_fbo_render_mipmap`, `OES_draw_buffers_indexed`,
  `OES_sample_variables`, `OES_shader_multisample_interpolation`,
  `OVR_multiview2`, `NV_shader_noperspective_interpolation`, and the
  `WEBGL_` blend-func-extended, clip-cull-distance, polygon-mode,
  provoking-vertex, render-shared-exponent, stencil-texturing,
  multi-draw-instanced-base-vertex and draft ones), whose support varies by
  backend and which few sites use.

Hidden extensions behave exactly as on a GPU without them: `getExtension`
returns null, their enums are invalid, their shader `#extension`s don't
compile, and enabling `EXT_color_buffer_float` or `OES_texture_float` no
longer implicitly enables `EXT_float_blend`.

**Shader precision formats** (`rangeMin, rangeMax, precision`, both shader
types): `highp` float 127, 127, 23 and int 31, 30, 0, as on every WebGL 2 GPU;
`mediump` and `lowp` float 15, 15, 10 and int 15, 14, 0, the half floats and
16-bit integers that GPUs with native low precision (mobile, Apple) run them
at. Desktop GPUs run every precision at 32 bits and are clamped down to the
same report; a page that checks `HIGH_FLOAT` (as three.js does) is
unaffected, one that looks at `MEDIUM_FLOAT` may pick `highp` instead,
which works everywhere.

**Checked and left alone.** `getContextAttributes()` echoes the request
except `antialias` (false where Chromium can't or won't use MSAA on the
GPU) and `desynchronized` (per platform); reporting an antialias the GPU
doesn't do would make pages skip their own anti-aliasing, so it stays
real. `VERSION`, `SHADING_LANGUAGE_VERSION`, `VENDOR` and `RENDERER` are
fixed Chromium strings; `RED/GREEN/BLUE/ALPHA/DEPTH/STENCIL_BITS`,
`SAMPLES` and `IMPLEMENTATION_COLOR_READ_FORMAT/TYPE` describe the bound
framebuffer, which ANGLE derives from the requested formats rather than the
hardware;
`COMPRESSED_TEXTURE_FORMATS` only lists formats of enabled (so allowed)
extensions; the extension order is the fixed registration order. Whether
`getContext('webgl2')` succeeds at all (blocklisted GPUs) and
`failIfMajorPerformanceCaveat` remain hardware signals. WebGL on WebGPU
(`WebGLOnWebGPU`) is an experiment behind a Blink runtime flag that is off
by default; it has its own implementation and is not covered.

**Not enforced.** The reported values are what the page is told, not what
it gets: the GPU's real limits still apply, so a page that uses more than
the reported limit (a 16384-pixel texture, more uniforms, 8x MSAA) keeps
working, exactly as before. That also means a script that probes instead of
asking still finds the real limits: allocating textures until one fails,
compiling shaders with more uniforms or varyings, `drawBuffers` with more
buffers, `DRAW_BUFFERi` or `bindBufferBase` indices past the reported
count, the sample count a `renderbufferStorageMultisample(3, ...)` is
rounded up to, `drawingBufferWidth` of a canvas wider than 8192, or
measuring `mediump` precision in a shader. Enforcing the baselines would
mean new validation in dozens of entry points (and different errors from
upstream's), so it isn't done: like the canvas noise, this stops scripts
that read the values (what fingerprinting libraries do), not ones written
against MoltBrowser.

### WebGPU: no adapter

In MoltNet windows `navigator.gpu.requestAdapter()` resolves to null, the
spec's answer when no adapter is available, before any GPU-process context
is created; sites already handle it by falling back to WebGL (three.js'
`WebGPURenderer`, for one, switches to its WebGL 2 backend). Tor Browser
has no WebGPU either. Making WebGPU uniform instead would mean fixing
`GPUAdapter.info`, every limit, the feature list and the WGSL behaviour of
each backend, and WebGPU's rendering and compute results would still
differ by GPU, with no readback noise to cover them (see the gaps below).

* The rest of `navigator.gpu` stays: `getPreferredCanvasFormat()` is fixed
  per platform at compile time (`rgba8unorm` on Linux and Android,
  `bgra8unorm` elsewhere) and `wgslLanguageFeatures` comes from the Dawn
  build and flags, not the GPU. Hiding `navigator.gpu` itself would be
  cleaner for feature detection, but at 152 the interface is no longer
  behind a runtime flag, so it would need an IDL change; a null adapter is
  the one-hook way.
* Workers (dedicated, shared, service) go through the same
  `GPU::RequestAdapterImpl` and are covered too.
* DevTools' console for the page says "WebGPU is not available in MoltNet
  windows" (the page itself can't read it).

### Which contexts

The same contexts as farbling, decided per call from the WebGL context's
(or `navigator.gpu`'s) execution context: documents, iframes (by their
top-level site), workers including `OffscreenCanvas` WebGL in workers.
Pages whose top-level site isn't http(s) (`chrome://` and other internal or
local pages) keep the real GPU. MoltNet ignores Shields off, as it does for
farbling. A WebGL call without an execution context fails closed (gets the
baselines).

### Scope: MoltNet only

None of this applies in normal and Incognito windows. Their goal is per-site
unlinkability without breaking sites: fixed limits can't vary per site in a
useful way (random lower limits would break apps and the real ones show
through probing anyway), and clamping them to one baseline only makes a user
look like the other MoltBrowser users, which normal windows don't otherwise
attempt (real time zone, screen, language and unmasked GPU strings). It
would cost every WebGL app the limits, extensions and WebGPU above for no
unlinkability gain.

### What it costs MoltNet users

* Pages that honour the limits use at most 8192-pixel textures (image
  viewers and map engines tile more) and 4x MSAA.
* No 32-bit float blending (`EXT_float_blend`): GPGPU code that accumulates
  into float32 targets with blending fails; half-float blending works.
* KTX2/Basis texture transcoders choose S3TC (BC1/BC3) instead of BC7 or
  ASTC: a little more memory or a little less quality.
* WebGPU-only pages show their "WebGPU not supported" path.

The baselines target desktop GPUs, where MoltNet runs. If MoltNet ever
comes to Android, the texture size, uniform-vector and draw-buffer
baselines (and S3TC, which mobile GPUs lack) need a mobile-specific look.

### Code

| Where | What |
|---|---|
| `core/molt_fingerprinting/webgl_uniform.{h,cc}` | the baselines, allow-list and precision formats, std + //base only (+ `webgl_uniform_unittest.cc`, in `:unit_tests`) |
| `core/molt_fingerprinting/gpu_uniformity.{h,cc}` | when to apply them (kUniform, web context), the entry points the hooks call (+ `gpu_uniformity_test.cc`) |

Upstream hooks (each marked `MoltBrowser:`):
`webgl_rendering_context_base.cc` (`ExtensionSupportedAndAllowed`,
`GetIntParameter`, `GetFloatParameter`, `GetWebGLFloatArrayParameter`,
`GetWebGLIntArrayParameter`, the `WEBGL_draw_buffers` cases of
`getParameter`, `getShaderPrecisionFormat`),
`webgl2_rendering_context_base.cc` (`GetInt64Parameter`,
`getInternalformatParameter`) and `webgpu/gpu.cc` (`RequestAdapterImpl`).

**Verified without a Chromium checkout:** `webgl_uniform_unittest.cc` (14
tests: every baseline against the ES 3.0 minimums, clamp directions, the
tables' consistency, precision formats, sample counts, the allow-list)
passes standalone with g++ (ASan/UBSan) and clang (UBSan trap,
`-Wunsafe-buffer-usage -Wshorten-64-to-32 -fno-exceptions`); every GL enum
value in `webgl_uniform.h` matches the Khronos headers at the pin (the
static_asserts of `gpu_uniformity.cc`, compiled against them);
`gpu_uniformity.cc` type-checks with the same strict flags against
stand-ins of the pinned Blink signatures, and a 9-test harness runs the
real glue in all three modes, including real kUniform (web, opaque and
non-web top-level sites, the seed missing, Shields off ignored, state
untouched); `gpu_uniformity_test.cc` itself builds and passes against
those stand-ins (plus `DummyPageHolder` and friends); the hook expressions
type-check with their upstream argument types. **Needs the build
machine:** the real Blink build of the hooks, `gpu_uniformity_test.cc` in
`blink_unittests` against the real `DummyPageHolder`, and a MoltNet window
running WebGL report pages (e.g. webglreport.com, three.js and Babylon.js
examples, a KTX2 sample) and a WebGPU sample (null adapter, WebGL fallback)
on a Mac, a Windows and a Linux machine, comparing their reports.

## Letterboxing (MoltNet)

MoltNet windows used to report a rounded screen size while the page itself
kept the window's true content area: `innerWidth`/`innerHeight`, the CSS
`width`/`height` media queries, `vw`/`vh` and layout showed a size that
depends on the window size, the toolbars (bookmarks bar, infobars), the OS,
the DPI and so on, one of the strongest MoltNet fingerprints left. Now the
browser rounds the page's area itself, Tor Browser style: the tab's content
area is rounded **down** to 200 x 100 DIP steps, capped at 1400 x 900, and
the page is centred in it with margins around. A page's size is then one of
63 (7 widths x 9 heights), the same for everybody whose window rounds the
same way.

### What a MoltNet page sees

| Value | MoltNet |
|---|---|
| `innerWidth`/`innerHeight`, CSS `width`/`height`/`aspect-ratio` media queries, `vw`/`vh`, layout | the letterboxed size: a multiple of 200 x 100, at most 1400 x 900 (at 100% zoom) |
| `screen.width/height/availWidth/availHeight`, `outerWidth/outerHeight` | the same size (`screen.*` now counts the page's scrollbars, as `innerWidth` does) |
| CSS `device-width`, `device-height`, `device-aspect-ratio` | the same size |
| `devicePixelRatio`, the `resolution` / `-webkit-device-pixel-ratio` media queries, the density `srcset` / `<picture>` / `image-set()` candidates are picked for | **1** (see [Device pixel ratio](#device-pixel-ratio)) |
| page zoom | starts at 100% (see [Zoom](#zoom)) |

Normal and Incognito windows are unchanged (their goal is per-site
unlinkability; a rounded window would only make users look like other
MoltBrowser users). The kill switch turns all of this off with the rest of
the protection.

**The browser side.** The active tab's page is shown by `ContentsWebView`, a
`views::WebView` that lays the page's native view out in its
`GetContentsBounds()`. In MoltNet profiles `ContentsWebView::GetInsets()`
returns the letterbox margins (`molt_letterbox_view.cc`), so the page gets
the rounded, centred rectangle and is resized only when the rounded size
changes (a few times while dragging a window edge, not on every pixel).
The view's solid-colour layer, under the page, shows in the margins.
Everything else in the content area (DevTools, overlays, dialogs, the status
bubble) still uses the view's full bounds, so none of it moves. The geometry
is a constexpr function (`molt_letterbox.h`); the steps and caps are Blink's
(`farbling.h`), duplicated because browser code can't include renderer
headers, and `molt_letterbox_unittest.cc` checks both the constants and that
Blink's rounding of every letterboxed size is that size again.

**The renderer side** (`core/molt_fingerprinting/letterbox.{h,cc}`). The
uniform screen is now computed from the main frame's size *with* its
scrollbars (before: without, so a page with a classic scrollbar got a screen
one step narrower than its `innerWidth`), converted to DIPs with
`innerWidth`'s own rounding (`AdjustForAbsoluteZoom::AdjustInt`), then
bucketed. On a letterboxed page `screen.width` and `innerWidth` are
therefore the same number; a page the browser couldn't letterbox (below)
still gets a bucketed screen. `device-width`/`device-height` come from the
same screen (hooked in `MediaValues`, so `matchMedia`, `<source media>` and
the preload scanner agree); Blink already re-evaluates device media queries
when the page's size changes.

### Margins

The margins take the **toolbar colour** (`kColorToolbar`): the surface the
page sits on, the colour split view shows between and around its panes. It
follows light and dark mode and the theme (and is forced opaque). In tab
fullscreen they take Chrome's own colour around letterboxed fullscreen
content (`kColorWebContentsBackgroundLetterboxing`, near black), like the
bars around a film. The page never sees the margins or their colour.

### Browser UI around the page

* **DevTools docked**: takes its space from the content area first; the page
  is letterboxed in what is left. Docking DevTools or dragging its splitter
  moves the page from one bucket to another, never to an unrounded size.
  DevTools itself isn't letterboxed. Its device emulation overrides the
  page's size (a developer tool): the screen is then the emulated size,
  bucketed.
* **Side panel** (AI chat, agent, reading list, ...): sits beside the
  content area, so opening it narrows the area and the page drops to a
  smaller bucket.
* **Find bar**: a separate widget floating over the content area; it doesn't
  resize the page and may sit over a margin.
* **Infobars and the bookmarks bar**: above the content area. They change
  the page's height only when they push the area below a multiple of 100;
  usually the vertical margin absorbs them.
* **Tab-modal dialogs, permission prompts, the status bubble**: overlays,
  unchanged (the status bubble sits in the content area's corner, over the
  margin).
* **Split view**: each pane is letterboxed on its own.
* **Popups** (`window.open` with a size, `resizeTo`): letterboxed like any
  window, so a requested size larger than the screen no longer shows the
  screen's real size either.

### Fullscreen

Tor Browser lifts letterboxing in fullscreen (by what Mullvad Browser users,
on the same code, report), so a page that gets fullscreen learns the
monitor's real size. **MoltBrowser keeps the letterbox in fullscreen**,
browser fullscreen and tab fullscreen (`requestFullscreen()`, video players)
alike: a page only needs one click from the user to ask for fullscreen, and
an exact monitor size (1512 x 982 is a 14-inch MacBook Pro, 1728 x 1117 the
16-inch) is a strong identifier. The cost is on monitors larger than
1400 x 900 DIPs: fullscreen video plays in a 1400 x 900 box with dark
margins (about 73% of the width of a 1920-DIP screen). A page that is being
tab-captured while fullscreen is sized to the capture by Chrome's own
capture letterboxing; that size comes from the capturing site, not the
screen.

### Zoom

Page zoom changes CSS pixels: at 125%, a 1400-DIP page is 1120 CSS pixels
wide. **MoltNet resets zoom but doesn't lock it.**

* MoltNet's profile used to copy, and keep following, the regular profile's
  default zoom and per-site zoom levels (Chromium does that for every
  off-the-record profile). A site zoomed in a regular window opened zoomed
  in MoltNet, which both broke the letterbox and linked the two profiles.
  Now MoltNet pages always start at 100% (`OffTheRecordProfileImpl::Init`
  skips `TrackZoomLevelsFromParent` for MoltNet).
* The user can still zoom (an accessibility need; Tor Browser doesn't lock
  zoom either). A zoom chosen in a MoltNet window applies to that site for
  the MoltNet session and is forgotten at New Identity. While a page is
  zoomed its `innerWidth`/`innerHeight` show the zoom factor (the screen
  doesn't change); `devicePixelRatio` stays 1, so it doesn't.
* Not done: letterboxing the CSS size instead of the DIP size (so zoom would
  stop showing), which needs the browser to re-letterbox on zoom changes.

### Device pixel ratio

**MoltNet pages report a device pixel ratio of 1**, Tor Browser's choice
too (in 2024 it reverted a change to 2). The real value splits users into
1x, 1.25x, 1.5x, 1.75x, 2x, ... groups, which differ by OS and machine
(every Retina Mac is 2x); one value for everybody is the point of MoltNet,
and 1 is the one that matches the uniform screen as a plain standard-density
screen and costs nothing extra over Tor (2 would make every 1x user download
images four times as large).

* Covered: `window.devicePixelRatio`; the `resolution` and
  `-webkit-device-pixel-ratio` media queries (`matchMedia`, `<source
  media>`, the preload scanner); the density `srcset` and `<picture>`
  candidates are picked for, on the main path and in the preload scanner,
  and `image-set()`, so the images a page fetches don't tell its server the
  real ratio either. Page zoom doesn't change it.
* Only what pages read changes: Blink still renders at the real scale, so
  text, borders, SVG and video stay sharp.
* Cost on high-density screens: pages that size canvases by
  `devicePixelRatio` draw them at 1x (blurry charts, maps and games), and
  `srcset`/`image-set()` images are the 1x ones (softer, but smaller over
  Tor).
* Internal pages (`chrome://` and the like) keep the real ratio.
* Not covered, so a script that looks for it still finds the real ratio:
  layout snapping to device pixels (thin borders, for example, can be
  measured), `ResizeObserver`'s `devicePixelContentBoxSize`, the DPR client
  hints (`Sec-CH-DPR`, sent only to sites that ask with `Accept-CH`) and the
  favicon size the browser fetches. As with the WebGL baselines, this stops
  scripts that read the value, not ones written against MoltBrowser.

### Known gaps (letterboxing)

* A content area narrower than 200 DIPs or shorter than 100 DIPs is not
  letterboxed (there is no smaller step); the screen still reports one step.
* Cross-process iframes only know the main frame's size *without* its
  scrollbars, so with classic (Windows, Linux) scrollbars a scrolling main
  frame's cross-site iframes can report a screen one step narrower than the
  main frame's. Device media queries in an iframe whose own size doesn't
  change are re-evaluated at its next media-value change.
* At a device scale factor of 1.2, Chromium's float rounding gives the page
  one device pixel more than the browser laid out at some sizes, so
  `innerWidth` is one more than the letterbox (1401 for 1400); the screen
  still says 1400.
* Zoom shows while a page is zoomed (above).
* A maximised window on a large monitor shows the page at 1400 x 900 with
  wide margins: the cap is what makes everyone with a large window
  identical. New MoltNet windows still open at the regular window size
  (follow-up: open them at a 1400 x 900 page).
* During the side panel's flyover animation the page's clip rectangle is
  computed for the view without its margins, so the page can be clipped a
  margin's width off for the length of the animation (cosmetic).

### Code

| Where | What |
|---|---|
| `chrome/browser/molt_ai/fingerprinting/molt_letterbox.h` | the geometry, constexpr and std-only (+ `molt_letterbox_unittest.cc` in `molt_ai_unittests`, against Blink's constants and rounding) |
| `chrome/browser/molt_ai/fingerprinting/molt_letterbox_view.{h,cc}` | which tabs, the insets, the margin colour (desktop only, `:letterbox_view`) |
| `core/molt_fingerprinting/letterbox.{h,cc}` | the screen size with scrollbars, the device pixel ratio (+ `letterbox_test.cc`) |

Upstream hooks (each marked `MoltBrowser:`): browser
`chrome/browser/ui/views/frame/contents_web_view.{h,cc}` (`GetInsets`,
`UpdateBackgroundColor`, `DidToggleFullscreenModeForTab`) and
`chrome/browser/profiles/off_the_record_profile_impl.cc` (`Init`); Blink
`css/media_values.cc` (`CalculateDeviceWidth`, `CalculateDeviceHeight`,
`CalculateDevicePixelRatio`), `frame/local_dom_window.cc`
(`devicePixelRatio`), `html/html_image_element.cc`
(`FindBestFitImageFromPictureParent`, `SelectSourceURL`) and
`css/resolver/style_resolver_state.cc` (the constructor's image density).
`UniformScreenRect()` (`molt_fingerprinting.cc`) now takes its size from
`letterbox.cc`.

**Verified without a Chromium checkout:** `molt_letterbox_unittest.cc`
(4 tests and static_asserts) passes standalone against the real
`farbling.cc` with g++ (ASan/UBSan) and clang (UBSan trap,
`-Wunsafe-buffer-usage -Wshorten-64-to-32 -fno-exceptions`);
`letterbox.cc` and `molt_fingerprinting.cc` type-check with the strict
flags against stand-ins of the pinned Blink signatures, and a harness runs
the real glue in real kUniform mode: for 13 device scale factors and every
content area up to 4000 DIPs, the renderer's size (`ceil(DIP x scale)`, as
content computes it) gives a screen equal to `innerWidth`/`innerHeight` and
to the browser's letterbox size (with the 1.2 drift above); cross-process
and same-process frames; the density for web, opaque, `chrome://`,
extension and `file://` top-level frames. Seven mutants of `letterbox.cc`
(scrollbars left out, the old flooring, no web check, ...) all fail it. The earlier glue
harness passes with its screen test now expecting scrollbars to count.
`molt_letterbox_view.cc` type-checks against stand-ins of the pinned
views/content/color signatures and runs, with the `ContentsWebView` hook
expressions, under ASan/UBSan (insets, margin colours, fullscreen, no page,
kill switch); the Blink hook expressions type-check with their upstream
argument types. **Needs the build machine:** compiling the hooks,
`letterbox_test.cc` (`MoltLetterboxTest.*`) in `blink_unittests`, the
unit test in `molt_ai_unittests`, and in a MoltNet window on a Mac, a
Windows and a Linux machine (1x and 2x): resizing the window (the page
steps, `screen.width == innerWidth` throughout, e.g. with a page printing
both on `resize`), DevTools docked on each side, the side panel, an
infobar, split view, browser and tab fullscreen, light and dark mode, a
regular-profile zoom not carried over, and `devicePixelRatio`, `srcset` and
`image-set()` on a 2x screen.

## Fonts

The set of installed font families is one of the strongest fingerprinting
signals: a page that draws text in `font-family: "X", monospace` and
compares widths, declares `@font-face { src: local("X") }`, calls
`document.fonts.check()`, measures or draws text on a canvas, or calls
`queryLocalFonts()` can test hundreds of families in a few milliseconds.
So protected contexts only see some installed ("system") families:

| Context | Installed families it can name |
|---|---|
| MoltNet window | the platform's **allow-list** only: the same for every MoltNet user on that OS |
| Normal / Incognito window, web site | the allow-list plus the site's **sample**: each other family name is in it with probability 1/16, decided by `PRF(site key, name)`, so stable within a site and session, unrelated across sites and sessions |
| Normal / Incognito, a context with a random key (opaque top-level site, SVG image document, third-party shared worker, a context that started before the seed) | the allow-list only (a per-context random sample could be summed over many such contexts) |
| Shields off (normal / Incognito), non-http(s) top-level site, protection off | every font, exactly as upstream |

Never affected: generic families (`serif`, `sans-serif`, `monospace`,
`cursive`, `fantasy`, `system-ui`, `math`, `-webkit-body`) and the fonts the
user chose for them in settings, web fonts (`src: url()`, `FontFace` from
data), and the per-character system fallback.

**How a lookup is decided** (`molt_fingerprinting/fonts.{h,cc}`, pure logic;
`system_fonts.{h,cc}`, Blink glue). Names are compared after lower-casing
ASCII letters and dropping ASCII whitespace (fontconfig ignores blanks too);
other code points are compared as they are, in Latin-1, UTF-8 or UTF-16
without copying. A family lookup is allowed only if **both** the requested
name and the family the platform resolved it to (`SkTypeface::getFamilyName`,
cached per typeface) are visible:

* the resolved family stops aliases, localized, full and PostScript names,
  fontconfig rules and Windows suffix matching ("Segoe UI Semibold" ->
  Segoe UI) from reaching a hidden family;
* the requested name keeps platform aliasing from telling machines apart.
  On Linux, `Arial` is the real Arial on a machine with msttcorefonts and
  Liberation Sans (fontconfig's metric alias) elsewhere; Blink itself
  substitutes Arial for a missing Helvetica, Courier New for Courier, and so
  on. Such names are listed as aliases and protected contexts look up the
  listed family instead (Liberation Sans, Arial, ...), so every machine gets
  the same answer. In a normal window an alias whose name is in the site's
  sample is looked up as is.
* A hidden name is not looked up at all, so an installed hidden font and a
  missing font are indistinguishable, in result and in timing.

`system-ui` is never restricted, also when Blink uses it as a family name
(quoted, or macOS's `BlinkMacSystemFont`): it is the platform UI font, which
the generic `system-ui` already shows.

**The allow-lists** (`font_allowlists.cc`) hold, per OS, the families every
supported version installs by default; a font missing on some supported
version would split users, so the lists err on the small side (a missing
entry only costs compatibility: the page falls back to its next family).

* Windows 10 (22H2) and 11: the intersection of Microsoft's published
  Windows 10 and Windows 11 font lists, without the "Features on Demand"
  supplemental fonts (Meiryo, MS Mincho, SimHei, Gulim, Arial Nova, ...;
  installed with some display languages only) and without the Windows 11
  additions (Segoe UI Variable, Segoe Fluent Icons, Cascadia Code / Mono).
  Also the legacy per-weight family names DirectWrite reports (Segoe UI
  Semibold, Bahnschrift Light, ...) and the localized names of the default
  CJK fonts (MS Gothic, Yu Gothic, SimSun, Microsoft YaHei / JhengHei,
  MingLiU-ExtB, Malgun Gothic). Aliases: Helvetica, Times, Courier,
  MS Sans Serif, MS Serif (Blink's own substitutions).
* macOS 13, 14 and 15: the families installed in `/System/Library/Fonts`
  and `/System/Library/Fonts/Supplemental` per Apple's published font lists
  for those releases, not the ones macOS downloads on demand; plus a few
  PostScript names sites use (`HelveticaNeue-Light`, `HiraKakuProN-W3`, ...)
  and localized names of the CJK families. The long tail of rarely named
  script fonts (Noto Sans Adlam, Noto Sans Bamum, ...) is left out.
* Linux: only what mainstream desktop distributions all install: DejaVu
  (Sans, Serif, Sans Mono, Condensed, Light), Liberation 2 (Sans, Serif,
  Mono) and Noto (Sans, Serif, Sans Mono, Color Emoji). Aliases: Arial,
  Helvetica, Times New Roman, Times, Courier New, Courier -> Liberation.
* Android: the families and aliases of AOSP's `fonts.xml` (OEM additions
  are not listed; users can't install system fonts).

The lists were built from the vendors' published lists and default installs
(no other browser's list or code was used); the Windows and macOS lists
still need checking against fresh installs of each supported version
(Follow-up 6).

**Sample rate.** A site sees each non-listed installed family with
probability p. Compatibility needs a large p, but the sample is a
per-site *view* of a stable fact: two sites can link a visitor through any
rare family both happen to see, with probability p^2 per family:

| p | extra families a site sees, of 10 / 30 | 2 sites share one, of 10 / 30 |
|---|---|---|
| 1/4 | 2.5 / 7.5 | 48% / 86% |
| 1/8 | 1.25 / 3.75 | 15% / 38% |
| **1/16** | 0.6 / 1.9 | **3.8% / 11%** |
| 1/32 | 0.3 / 0.9 | 1.0% / 2.9% |

At no rate that keeps linking rare can a site rely on a font the visitor
installed, so the sample buys little compatibility; it mostly makes the
visible set vary per site. MoltBrowser uses **1/16** (`kExtraFontOneIn`):
cross-site overlap stays small for typical users (Office or Adobe users,
with dozens of extra but common families, share about one between two
sites, which says little), and Shields off is the escape hatch when a site
needs the user's own fonts. The sample is redrawn every session.

**Hooks** (one-to-five-line, each marked `MoltBrowser:`):

| API | Hook (upstream file: function) |
|---|---|
| CSS `font-family`, canvas `ctx.font` (`measureText`, `fillText`), OffscreenCanvas and `FontFace` in workers, SVG text | `font_fallback_list.cc`: `GetFontData` asks the selector (`font_selector.{h,cc}`: new virtual `FontSelector::GetSystemFontData`, default = the old `FontCache` lookup), overridden in `css_font_selector_base.{h,cc}` for documents and workers |
| `document.fonts.check()`, `self.fonts.check()` | `css_font_selector_base.cc`: `IsPlatformFamilyMatchAvailable` |
| `@font-face` / `FontFace` `src: local()` (and `FontFace.load()`'s status) | `local_font_face_source.cc`: `IsLocalFontAvailable`, `CreateFontData` (unique names match exactly on every platform, so the face's family alone decides) |
| `queryLocalFonts()` | `font_access.cc`: `DidGetEnumerationResponse` |

**Local Font Access.** `queryLocalFonts()` still needs the permission
prompt; after it, it returns only the families CSS can use in the same
context: the allow-list in MoltNet (the same for every MoltNet user on the
OS), the allow-list plus the site's sample in normal windows, everything
with Shields off. Design tools that need all of the user's fonts work with
Shields off for their site.

**Generic families at 152.** Blink parses `serif`, `sans-serif`, `cursive`,
`fantasy`, `monospace`, `-webkit-body`, `system-ui` and `math` as generic
families; `CSSFontSelector` maps them through the user's font settings
(`FontSelector::FamilyNameFromSettings`) and looks the result up directly,
not through the hook, so they keep working. `system-ui` resolves to the
platform UI font in `FontCache`. `ui-serif`, `ui-monospace`, `emoji`,
`fangsong` and the like are not generic families in Blink 152; they are
plain family names that don't resolve (and are treated as such). Text
without `font-family` uses the standard-font setting the same way. If the
user points a default-font setting at a font outside the allow-list, that
font is still used for that generic and for unstyled text: the user's
choice wins, and pages can't name it, but see the gaps below.

**Cost.** The visibility of a context is computed once per document or
worker (string-free afterwards); a decision hashes the name once (linear,
no allocation), binary-searches the list and, after a successful lookup,
reads the typeface's family from a 256-entry cache. Toggling Shields applies
from the next document (MoltShield reloads the page).

**Known gaps (fonts)**

* **Per-character system fallback** (emoji, CJK, rare scripts): a page that
  renders characters no listed font has gets them from whatever installed
  font covers them, and can measure that. Out of scope.
* **Text metrics and rendering**: `measureText()` values and glyph pixels
  are not noised beyond the canvas noise; hinting, anti-aliasing and font
  versions differ between machines.
* **Font settings**: the fonts the user chose for the generic families
  (and the minimum font size) stay visible through generic families and
  unstyled text; a page that ships candidate fonts as web fonts can compare
  metrics and identify them. On Linux, `system-ui` shows the desktop's UI
  font, and Chromium's default `sans-serif` setting ("Arial") resolves to
  the real Arial or to Liberation Sans depending on the machine (the same
  metrics, different glyphs). Some default settings (per-script CJK
  fonts, `math`) are lists from which Chromium takes the first installed
  family, so those generics also tell which candidate is installed.
  MoltNet should eventually pin these settings.
* Users whose machine lacks an allow-listed font (removed by an
  administrator, minimal Linux installs) remain distinguishable by that.
* In normal windows, a sampled name that resolves to a hidden family costs a
  platform lookup that a page might time.
* Code paths that build a `Font` without a font selector use `FontCache`
  directly and aren't filtered; none is known to be reachable from web
  content (frameless documents refuse canvas fonts).

**Verification.** Standalone (no Chromium checkout): `fonts_unittest.cc`
(13 tests: normalization in all three encodings, malformed UTF-8/UTF-16,
list integrity, the sample's rate, stability and cross-site independence,
uniform mode) passes with g++ (ASan/UBSan) and clang (UBSan trap,
`-Wunsafe-buffer-usage -Wshorten-64-to-32 -Wimplicit-int-conversion
-Wexit-time-destructors -Wglobal-constructors -fno-exceptions`), together
with `farbling_unittest.cc`; `system_fonts.cc` and `molt_fingerprinting.cc`
type-check with the strict clang flags against stand-ins of the pinned
Blink/Skia signatures, and a 12-test harness runs the real glue against a
fake `FontCache` (per-mode visibility, aliases on two simulated machines,
name tricks, `check()`, `local()`, `queryLocalFonts()` filtering, Shields
off, random keys, workers, caching); the upstream hook statements
type-check against stand-ins of their classes. Needs the build machine:
compiling Blink with the hooks, `blink_unittests` (`MoltSystemFontsTest.*`,
`MoltFontNameTest.*`, `MoltFontListTest.*`, `MoltFontVisibilityTest.*`),
and in a running browser on each OS: font-probing pages and suites
(FingerprintJS, CreepJS font tests) in normal, Incognito and MoltNet
windows, with Shields on and off; East Asian sites; design tools with
`queryLocalFonts()`.

## MoltNet: touches, screens, orientation and form controls

Four more things that told MoltNet users apart: where a touch lands on
the physical screen and how big and firm it is, the user's monitors, the
screen's orientation, and the language and regional settings that form
controls follow. Normal and Incognito windows are unchanged: none of these
values can vary per site in a useful way, and fixing them would only make
a user look like other MoltBrowser users.

| API | MoltNet windows | Hook (upstream file: function) |
|---|---|---|
| `Touch.screenX/Y` | MouseEvent's value: the point relative to the viewport | `touch_event_manager.cc`: `CreateDomTouch` |
| `Touch.radiusX/Y`, `rotationAngle`, `force` | **0** (the spec's "unknown") | same |
| `PointerEvent` with `pointerType` "touch": `width`/`height`, `pressure`, `tiltX/Y`, `twist`, `tangentialPressure` | **1**/**1**, **0.5** while down (else 0), **0** | `pointer_event_factory.cc`: `UpdateCommonPointerEventInit` |
| `window.getScreenDetails()` | rejects: `NotAllowedError: Permission denied.`, no prompt | `window_screen_details.cc`: `GetScreenDetails` |
| `navigator.permissions.query({name: 'window-management'})` | **"denied"**, never changes | `permission_status_listener.cc`: `state`, `OnPermissionStatusChange` |
| `window.screen` "change" event | never fires | `screen.cc`: `AreWebExposedScreenPropertiesEqual` |
| `screen.orientation.type`, `.angle` | the uniform screen's: **landscape-primary**, **0** (see below) | `screen_orientation.cc`: `type`, `angle` |
| `screen.orientation` "change" event | never fires | `screen_orientation_controller.cc`: `NotifyOrientationChangedInternal` |
| Date, time, month, week and number fields, numbers in validation messages | **en-US** formats on every OS | `platform_locale.{h,cc}`: `Locale::DefaultLocale`, set by `Configure` |

The values are in `moltnet_values.{h,cc}` (std only, tested standalone),
the Blink side in `moltnet_uniformity.{h,cc}`. Which contexts: what
continues an existing MoltNet treatment follows it, so `Touch.screenX/Y`
(MouseEvent's), the orientation (the uniform screen's) and the missing
screen events apply to the whole MoltNet process like the screen values
themselves, and the form locale is per process like the time zone and the
Intl locale. The new treatments (touch contact, `getScreenDetails()`, the
window-management permission) apply to web content only, like the WebGL
baselines: pages whose top-level site isn't http(s) keep the real values.
MoltNet ignores Shields off, as elsewhere.

### Touches

At 152 `TouchEventManager::CreateDomTouch` copies the touch point's
position on the physical screen into the `Touch`, so `screenX - clientX`
gave the window position plus the browser chrome, which mouse events in
MoltNet no longer show. Touch pointer events already went through
MouseEvent's hook (`UpdateCommonPointerEventInit` calls
`MouseEvent::SetCoordinatesFromWebPointerProperties`). The hook now passes
the touch point through the same `UniformScreenPoint()`, with the same
root-frame position, so a `Touch` and the `PointerEvent` of the same
finger report the same `screenX/Y`, relative to the viewport.

The contact is hardware-revealing too: whether and how a digitizer reports
the contact ellipse and pressure (many report nothing, others a fixed size,
others real values at their own resolution) differs between touchscreens,
and the contact size of a real finger is the user's. The same numbers
reach pages twice: `Touch.radiusX` is half of `PointerEvent.width`, and
`Touch.force` feeds `PointerEvent.pressure`. So both are fixed for touch
pointers: the `Touch` gets the Touch Events spec's values for "unknown"
(0), the `PointerEvent` the Pointer Events spec's values for hardware
without contact geometry or pressure (1 x 1, 0.5 while down, 0 otherwise;
no tilt, twist or barrel pressure). Pens and mice are untouched: pen
pressure and tilt are what drawing apps need. Cost: pages that use touch
size or pressure (some drawing apps and games) see none in MoltNet.

### Screens (Window Management API)

`screen.isExtended` is already false for the whole MoltNet process
(`Screen::isExtended` returns before the permissions-policy check). For
`window.getScreenDetails()` there were two options: report one screen
equal to the uniform screen, or reject as if the permission were denied.
MoltNet rejects, because at 152 that is one hook that cannot leak:

* `getScreenDetails()` is the only way to a `ScreenDetails` or
  `ScreenDetailed` object, so rejecting there closes every attribute they
  have (`left/top`, `isPrimary`, `isInternal`, `label`,
  `devicePixelRatio`, `hdrHeadroom` and the `CanvasHDR` colour primaries),
  their three change events, and whatever a later Chromium adds. A fake
  screen needs a hook per attribute and event, and has to stay consistent
  with what the browser still does with the real screens once the
  permission is granted (window placement on other screens, fullscreen on
  a chosen screen, fullscreen popups); the user would also still be asked
  for the permission.
* Denial is a state every site already handles: users deny it, and
  Firefox and Safari don't have the API at all. Together with
  `isExtended = false` a MoltNet window looks like a one-screen user who
  said no.

The rejection is the one a real denial gives (`NotAllowedError`,
"Permission denied."), returned without asking the browser: no prompt and
no permission state is consulted.

**The permission query did leak.** MoltNet's profile is an off-the-record
profile of the regular one, and `WINDOW_MANAGEMENT` is registered with
`INHERIT_IF_LESS_PERMISSIVE`: a block set in the regular profile (for the
site or as the default) shows in MoltNet, so `navigator.permissions.query`
said "denied" for users who once blocked window management and "prompt"
for everyone else, the same way after every New Identity. In MoltNet web
content the `PermissionStatus` for window-management now always says
"denied" (matching `getScreenDetails()`), and updates from the browser are
dropped, so it never fires "change". Other permissions inherit blocks the
same way; see the gaps.

**Screen change events.** `window.screen` fired "change" whenever the real
screen changed (the window moved to another monitor, the resolution or
scale changed, the monitor was rotated), although no value the page sees
in MoltNet changed: on its own that reveals a second monitor. In MoltNet
`Screen::AreWebExposedScreenPropertiesEqual` now always says "equal", so
no such event fires. (If `devicePixelRatio` stays real in MoltNet, a scale
change keeps firing `matchMedia('(resolution: ...)')` listeners, but no
longer `screen`'s "change".)

Cost: multi-screen web apps (presenter views, dashboards that place
windows) work on one screen only in MoltNet windows.

### Screen orientation

At 152, `screen.orientation.type` and `.angle` are what the browser sends
in `ScreenInfo` for the real display (on desktop
`DisplayUtil::GetOrientationTypeForDesktop`: portrait when the display is
at least as tall as wide, primary or secondary by panel rotation; the angle
is the panel rotation), cached by `ScreenOrientationController::
UpdateOrientation`, with a "change" event when they change. So a MoltNet
page saw, next to its letterboxed landscape screen, a pivoted monitor as
`portrait-secondary` at 270 degrees.

In MoltNet both are now computed when read, from the uniform screen
(`UniformScreenRect`), with the browser's desktop rule and no rotation:
angle 0, and **landscape-primary** whenever the rounded screen is wider
than tall. That holds for every window at least 200 DIPs wider than tall,
and always at the largest step (1400 x 900); but the steps round width and
height separately, so a narrow window can round to a square or tall
screen (a window snapped to half of a 1366 x 768 laptop gives 600 x 600)
and then reports **portrait-primary**, which is what a real display of
that size reports and keeps `screen.orientation` consistent with
`screen.width/height`. (Should letterboxing make the uniform screen
always landscape, the orientation follows by itself.) No orientation
"change" events fire in MoltNet.

Checked and left alone: the CSS `orientation` media query is evaluated
from the viewport (`OrientationMediaFeatureEval` compares the viewport's
width and height; a square viewport is portrait), not from the screen, so
it says what `innerWidth/innerHeight` already say (the letterboxing
work); forcing it would break responsive layouts. `window.orientation` and
`orientationchange` are behind the Android-only `OrientationEvent` flag.
`screen.orientation.lock()` fails the same way for everyone on desktop (no
orientation delegate).

### Form controls in en-US

**Where they come from at 152.**

* *Formats* (field order and separators of date, time, month, week and
  datetime-local inputs; month and weekday names; AM/PM; digits, decimal
  and group separators; the first day of the week): form controls ask
  `Element::GetLocale()`, i.e. `Document::GetCachedLocale()`, which
  returns `Locale::DefaultLocale()` (the `lang` attribute is only used
  with `LangAttributeAwareFormControlUI`, off on desktop).
  `DefaultLocale()` is `Locale::Create(DefaultLanguage())`, and
  `DefaultLanguage()` is `Platform::DefaultLocale()`, the renderer's
  `--lang` switch: the browser's UI language. `Locale::Create` is
  `LocaleIcu` on Linux; `LocaleMac` on macOS, which uses `[NSLocale
  currentLocale]` (the user's region and custom formats) whenever the
  language matches; and `LocaleWin` on Windows, which uses
  `LOCALE_USER_DEFAULT` with the user's overrides whenever the language
  matches. A page can't read the formats directly, but it can measure the
  inputs, and number formatting shows in validation messages.
* *Strings* (`validationMessage`, "Choose File" / "No file chosen", the
  default "Details" summary, the default submit/reset labels, date-field
  placeholders such as "mm/dd/yyyy"): `Locale::QueryString` ignores the
  `Locale` and calls `Platform::QueryLocalizedString`, which ends in
  `ChromeContentClient::GetLocalizedString` and the renderer's resource
  bundle: the locale pak named by `--lang` (loaded in
  `ChromeMainDelegate::PreSandboxStartup`) or, on Linux, the one the zygote
  loaded for the environment's locale before any profile existed.
* `LocaleController`, through which MoltNet already forces en-US (and
  DevTools emulates locales), only sets ICU's default locale and tells V8:
  `Intl` and `Date` follow it, Blink's form `Locale` never reads it. So it
  covers neither.

**What MoltNet does.** `Configure(kUniform)` installs a fixed en-US
`Locale` as `Locale::DefaultLocale()` for the whole process, through a new
`Locale::SetDefaultLocaleOverride()` (two small additions to
`platform_locale.{h,cc}`). Its data (`moltnet_values.h`) is exactly what
`LocaleIcu` computes for en-US at the pinned ICU (checked against ICU):
"M/d/yy", "h:mm:ss a" and "h:mm a" with the narrow no-break space of CLDR
42 and later, "MMMM yyyy", English month names, narrow weekday names, AM
and PM, Sunday first, ASCII digits, "." and ",". So a date input shows
`03/05/2024` and a time input `1:04:09 PM` for every MoltNet user, on
macOS and Windows as on Linux, whatever the UI language and regional
settings. The date and colour picker popups are separate pages a site
can't read, and keep the user's language and formats.

**Not done: the strings** (still in the browser's UI language). Loading
the en-US pak in MoltNet renderers is a larger change with user-visible
costs, so it is left as a design:

1. *Recommended: override only the strings pages can observe.* In MoltNet
   renderers, `ChromeContentRendererClient::RenderThreadStarted` (where the
   mode is configured) calls `ui::ResourceBundle::
   OverrideLocaleStringResource(id, text)` for the page-observable ids
   (the `IDS_FORM_*` strings of validation messages, file inputs, buttons
   and date/time fields, `IDS_DETAILS_WITHOUT_SUMMARY_LABEL`, ...), with
   their en-US text taken from `blink_strings.grd` at build time (a small
   generated table, with a test that compares it to the `.grd`). The same
   on every OS, no zygote work, and the browser UI, error pages and
   accessibility names stay in the user's language. Needs an audit of the
   `Locale::QueryString` / `QueryLocalizedString` callers to list the ids.
2. *`--lang=en-US` for MoltNet renderers.* MoltBrowser's
   `AppendRendererSwitches` could add it, but at 152
   `RenderProcessHostImpl::AppendRendererCommandLine` appends `--lang`
   after the embedder's switches, so content needs a one-line change to
   keep an embedder-set `--lang`. On macOS and Windows renderers then load
   en-US.pak before the sandbox, and `DefaultLanguage()` becomes en-US on
   every thread, which also fixes the default text locale (below). On
   Linux renderers are forked from the zygote, which loaded its pak before
   any profile existed and can't open another after the fork: it would
   need to preload en-US.pak and switch to it after forking a MoltNet
   renderer, for which `ResourceBundle` has no API. And every
   renderer-generated text in MoltNet tabs turns English: network error
   pages, media-control labels and screen-reader names, the pickers, parts
   of `chrome://` pages. (Tor Browser asks the user before requesting
   English pages.)

`DefaultLanguage()` also names the default text locale
(`LayoutLocale::GetDefault()`), used for text without a `lang` attribute,
for instance to pick the Han glyph variants of CJK text when the
Accept-Language list (en-US in MoltNet) doesn't decide
(`LayoutLocale::LocaleForHan`): a Japanese or Chinese UI language still
shows there. Option 2 fixes it; with option 1 it needs its own override of
`DefaultLanguage()` in MoltNet (`platform/language.cc`, all threads).

### Verification

Standalone (no Chromium checkout): `moltnet_values_unittest.cc` (5 tests:
the orientation rule over the letterbox steps, the touch values, the en-US
data and its consistency) passes with g++ (ASan/UBSan) and clang (UBSan
trap, `-fno-exceptions -Wunsafe-buffer-usage -Wshorten-64-to-32
-Wimplicit-int-conversion -Wexit-time-destructors -Wglobal-constructors`),
with `farbling_unittest.cc` and `fonts_unittest.cc` still passing; the
en-US data matches what ICU's own `LocaleIcu` calls return for en-US.
`moltnet_uniformity.cc` and `molt_fingerprinting.cc` type-check with the
same strict clang flags against stand-ins of the pinned Blink signatures
(the `Locale` interface copied from the pinned `platform_locale.h`), and an
8-test harness runs the real glue in all three modes, including real
kUniform: `Touch.screenX/Y` equal to MouseEvent's, the contact, pens and
mice left alone, Shields off ignored, non-web pages, `getScreenDetails()`
and the permission, the orientation of real uniform screens (rotated
monitor, snapped window, early and detached frames), `Configure` pinning
the locale; 8 mutations of the glue all fail it. The hook statements type-check with their upstream argument
types. **Needs the build machine:** compiling Blink with the hooks,
`blink_unittests` (`MoltNetUniformityTest.*`, `MoltNetValuesTest.*`), and in
a MoltNet window on a touchscreen laptop (Windows and Linux) and on macOS
and Windows machines with non-English UI and regional settings: touch
event test pages, a multi-screen setup (`getScreenDetails()`,
`permissions.query`, moving the window between monitors), a pivoted
monitor, and date/time/number inputs and their validation messages.

## Kill switch

`chrome://flags/#molt-fingerprinting-protection` (feature
`MoltFingerprintingProtection`, enabled by default). When disabled, new
renderers get no mode switch and no seed (Blink runs unmodified) and MoltNet
no longer forces en-US; relaunch to apply it to every renderer. It is
independent of `--disable-molt-shield` on purpose: that switch is an
ad-blocking escape hatch and must not silently drop MoltNet uniformity. The
per-site escape hatch is [Shields off](#shields-off) (normal and Incognito
windows only).

## Known gaps and attacks

Honest list; anything not in the table above is not covered.

**Readback paths not covered**

* **WebGL2 transform feedback.** A vertex shader can `texelFetch` a rendered
  texture and write the values into a transform-feedback buffer, read back
  with `getBufferSubData`; buffer contents have no format, so they can't be
  farbled without breaking every legitimate transform-feedback readback.
  Same for any other shader-side copy into a buffer.
* **WebGL `readPixels` formats not farbled:** integer (`RGBA_INTEGER` etc.,
  which a shader can fill with `floatBitsToUint` of any computation),
  packed 16-bit (`UNSIGNED_SHORT_5_6_5`, `4_4_4_4`, `5_5_5_1`),
  `UNSIGNED_INT_2_10_10_10_REV`, `10F_11F_11F_REV`, `5_9_9_9_REV`,
  `EXT_texture_norm16` `UNSIGNED_SHORT`, `BYTE`/`SHORT`, `ALPHA` and
  `LUMINANCE*`. Their noise would have no natural "invisible" magnitude, or
  be below their quantisation; a page that renders into such a framebuffer
  reads it unfarbled.
* **WebGPU rendering readback** (`copyTextureToBuffer` + `mapAsync`, storage
  buffers): not farbled. Any shader can move texels into a storage buffer
  in a few lines of WGSL, so a copy-level hook would be bypassed trivially,
  while buffers that are mapped for reading mostly hold compute results
  (e.g. ML inference) that noise would corrupt. What *enters* WebGPU from
  canvases and images is farbled (`copyExternalImageToTexture`), so 2D
  canvas and WebGL fingerprints can't be laundered through WebGPU. In normal
  windows WebGPU's own rendering differences, `GPUAdapter.info`, limits and
  features, and timing are not addressed; MoltNet windows have no WebGPU
  adapter at all ([WebGL and WebGPU in MoltNet
  windows](#webgl-and-webgpu-in-moltnet-windows)).
* `copyElementImageToTexture` / canvas `drawElement` (HTML-in-canvas) are
  behind the `CanvasDrawElement` runtime flag (status "test" at the pin) and
  not hooked.
* **Encoder identity.** `VideoEncoder`/`MediaRecorder` output of a farbled
  frame no longer reveals the canvas pixels, but the encoder implementation
  itself (hardware encoders) is a device signal; not addressed.
* **Audio routed out of Web Audio**: `MediaStreamAudioDestinationNode` ->
  `MediaStreamTrackProcessor` (`AudioData.copyTo`), `MediaRecorder`,
  `AudioEncoder`. `AudioParam` automation values computed by the browser are
  not farbled either.
* Float precision games: a page reading a half-float framebuffer as `FLOAT`
  can round the values back to half precision (`Float16Array`) and drop the
  float32 noise; more generally see *coarse quantisation* below.
* The rest of the WebGL surface in normal windows: limits (`MAX_*`),
  extension lists, shader precision formats. MoltNet windows report fixed
  baselines for all three ([WebGL and WebGPU in MoltNet
  windows](#webgl-and-webgpu-in-moltnet-windows)), but they aren't
  enforced, so a script that probes (allocating ever larger textures,
  compiling ever bigger shaders) still finds the real limits; and the
  results of WebGL rendering and of shader arithmetic still differ by GPU
  under the readback noise.
* Text metrics: `measureText()`/`TextMetrics`, `isPointInPath`. Installed
  fonts are limited (see [Fonts](#fonts)), but per-character fallback
  fonts, glyph rendering and the user's font settings still show.

**Context coverage**

* Worklets other than threaded ones created by a document (e.g. shared
  storage worklets) get no key and are not farbled; they expose none of the
  farbled APIs.

**MoltNet uniformity not done**

* Letterboxing ([Letterboxing](#letterboxing-moltnet)): a zoomed page's
  `innerWidth`/`innerHeight` show the zoom; the real device pixel ratio
  shows through device-pixel snapping, `devicePixelContentBoxSize` and DPR
  client hints; cross-process iframes can report a screen one step
  narrower than a main frame with a classic scrollbar.
* Renderer-side UI strings (`validationMessage`, the file input's labels,
  the default `<details>` summary and button labels, date-field
  placeholders) follow the browser's UI language, not en-US; the
  form-control formats are en-US ([design for the
  strings](#form-controls-in-en-us)).
* Text without a `lang` attribute uses the browser's UI language as its
  default locale (`LayoutLocale::GetDefault()`), which shows in the Han
  glyph variants of CJK text, among others (same section).
* A touchscreen itself shows: `navigator.maxTouchPoints` (and its value),
  the `pointer` / `any-pointer` / `hover` media queries and touch-event
  support; pen pressure, tilt and twist stay real (touch contact and
  coordinates are uniform, see [Touches](#touches)).
* Permission states MoltNet's profile inherits from the regular profile
  (a block, for a site or as the default) show through
  `navigator.permissions.query` for every permission except
  window-management, the same after every New Identity.
* DevTools' timezone/locale emulation can't be used in MoltNet windows (the
  overrides are held for the life of the renderer).

**Known attacks on the noise:**

* *Detection*: drawing known colours and reading them back reveals that
  noise is present. That is one bit shared by every MoltBrowser user, not an
  identifier.
* *Oracle*: a page can `putImageData` chosen colours, read them back, learn
  its own site's colour mapping and invert it for its fingerprint canvas. The
  per-site key keeps this per site, but a page determined to see the true
  pixels on its own site can, and a fingerprinting script written for
  MoltBrowser could then link sites. This is the price of value-only noise
  (same answer for every read). Designs that seed the noise from the whole
  canvas's content, as Brave does, make the oracle much harder but give
  different answers for sub-rectangle reads and drift on read-modify-write
  loops. Revisit if scripts start doing this.
* *Channel vote*: an opaque pixel only ever moves in one channel, picked by
  its alpha (so one fixed channel for all opaque pixels of a site). A page
  that draws its fingerprint in grey (R = G = B), or renders it three times
  in different channels, can take the majority of the three channels and
  recover the true value of every pixel. Found while reviewing v1 for
  v1.1. Not worth fixing on its own (decided Oct 2026): it is one case of
  *redundancy voting*, which defeats every sparse value-only noise. A page
  that renders the same text in grey levels 255, 254 and 253 gets three
  different anti-aliasing values per pixel, which the noise moves
  independently, so the majority of the three, scaled back, is again the
  true coverage. Moving all three channels together (the obvious fix)
  stops the channel vote but not this one, and changes every canvas
  fingerprint. The same holds for Brave's and Firefox's canvas noise: it
  defeats scripts that hash the exact pixels (what fingerprinting
  libraries do), not scripts written against the noise.
* *Coarse quantisation*: the noise is at most 1 per channel (one ulp for
  half floats, < 2^-15 relative for float32 and audio), so a script that
  compares results with a robust/perceptual hash or rounds them first can
  ignore it. Audio: masking the low mantissa bits removes the noise, but
  also the device-specific signal it hides.

**Other limitations**

* The session seed is in the memory of every renderer of the profile; a
  local attacker who can read process memory (a debugger, a compromised
  renderer) can compute the per-site noise. It is no longer on the command
  line.
* Every renderer of a profile receives the whole Shields-off list (see
  [Shields off](#shields-off)).
* `getChannelData()` writes the noise into a buffer that an
  `AudioBufferSourceNode` on the audio thread may be playing at the same
  time; this is the same benign race as a page writing to that array itself.
* Compatibility: about 1 colour in 16 of exported/read pixels and the low
  mantissa bits of read audio and float32 WebGL reads change. Invisible and
  inaudible, but tools that rely on exact values (pixel-art palettes,
  exact-colour tests, bit-exact audio analysis, colour-coded picking IDs
  read with `readPixels`, GPGPU that encodes floats into RGBA8 bytes) will
  see it; Shields off fixes a site. The canvas-to-video costs above are the
  other compatibility risk.

## Verification status

Verified without a Chromium checkout (scratch harnesses, not in the repo):

* `farbling_unittest.cc` (26 tests: v1's 22 plus premultiplied, narrow
  `readPixels`, float32 and float16 noise) passes standalone with g++
  (ASan/UBSan/LSan) and clang (UBSan trap; `-Wunsafe-buffer-usage
  -Wshorten-64-to-32 -Wimplicit-int-conversion -fno-exceptions`); HMAC
  vectors from Python's `hmac`, and 140,000 random outputs (the 80,000 of v1
  plus 60,000 premultiplied pixels, narrow reads and halves) matched against
  an independent Python model.
* `molt_fingerprinting.cc` (Blink glue) type-checks with the same strict
  clang flags against stand-ins of the pinned Blink/Skia/V8/GL/net
  signatures, and a 21-test harness runs the real glue under ASan/UBSan:
  key resolution for windows (site, nonce, opaque, non-web, re-derivation),
  dedicated workers, first- and third-party shared/service workers,
  service-worker storage keys, worklets (handed-over keys), Shields-off
  sites (and MoltNet ignoring them), the seed arriving late or malformed,
  WebGL pack layouts for every farbled format, PIXEL_PACK buffer mapping
  (in-place result equal to a client-memory read; chunked mapping of a
  27 MiB read; no mapping when not farbled; failed maps), the snapshot and
  export pipelines (premultiplied alpha preserved, never above alpha) and
  the uniform screen math and UTC/en-US side effects.
* The chrome/{common,browser,renderer} glue (mode switch, mojo seed and
  sites, content-settings observer, `ConfigurationReceiver`) and
  `molt_shield_site_{settings,exceptions}.cc` plus its unit test type-check
  against stand-ins of the pinned content/IPC/mojo/content-settings
  signatures (the generated mojom bindings are a hand-written stand-in);
  the switch helpers' unit tests pass. The ordering argument above was
  checked in the pinned `render_process_host_impl.cc`.
* Every upstream hook was written against the file at the pin and is a few
  lines calling MoltBrowser code; `tools/chromium/verify_patches.py`:
  `patches/` reproduces `src/`.

Needs the build machine (not verified here): compiling and linking the real
Blink/Chrome targets (including the mojom), `blink_unittests`
(`MoltFarblingTest.*`, `MoltFingerprintingTest.*`, now with window-based
tests through `DummyPageHolder` and a `FakeGLES2Interface` PBO test),
`molt_ai_unittests` (`MoltShieldSiteExceptionsTest.*` against a real
`HostContentSettingsMap`), checkdeps / `gn check` for the new DEPS entries
(`gles2_interface.h` and `site_for_cookies.h` in core), and the behaviour
in a running browser: test pages for every row of the table above (incl.
`getBufferSubData` after a PBO read, `VideoFrame.copyTo`, `captureStream` +
`MediaStreamTrackProcessor`, `copyExternalImageToTexture` + `mapAsync`,
`AudioWorklet`), in a normal window, an Incognito window and a MoltNet
window, with Shields on and off, the seed absent from `ps`/`chrome://
process-internals` command lines, plus fingerprinting suites (e.g. CreepJS,
FingerprintJS) across two sites and two sessions, and the frame-rate cost
on a canvas-effects video call.

## Code map

| Where | What |
|---|---|
| `src/third_party/blink/renderer/core/molt_fingerprinting/farbling.{h,cc}` | pure math (+ `farbling_unittest.cc`) |
| `src/third_party/blink/renderer/core/molt_fingerprinting/molt_fingerprinting.{h,cc}` | Blink glue (+ `molt_fingerprinting_test.cc`) |
| `src/third_party/blink/renderer/core/molt_fingerprinting/shared_worker_key.{h,cc}` | a shared worker's storage key, handed to its thread (+ `shared_worker_key_test.cc`) |
| `src/third_party/blink/renderer/core/molt_fingerprinting/webgl_uniform.{h,cc}`, `gpu_uniformity.{h,cc}` | MoltNet WebGL baselines (pure, + `webgl_uniform_unittest.cc`) and their Blink glue, incl. WebGPU (+ `gpu_uniformity_test.cc`) |
| `src/third_party/blink/renderer/core/molt_fingerprinting/letterbox.{h,cc}`; `src/chrome/browser/molt_ai/fingerprinting/molt_letterbox.h`, `molt_letterbox_view.{h,cc}` | MoltNet screen size, pixel ratio (+ `letterbox_test.cc`); the browser-side letterbox geometry and margins (+ `molt_letterbox_unittest.cc`) |
| `src/third_party/blink/renderer/core/molt_fingerprinting/moltnet_values.{h,cc}`, `moltnet_uniformity.{h,cc}` | MoltNet touches, screen details, orientation, en-US form locale (pure, + `moltnet_values_unittest.cc`; glue, + `moltnet_uniformity_test.cc`) |
| `src/third_party/blink/renderer/core/molt_fingerprinting/font_allowlists.{h,cc}`, `fonts.{h,cc}`, `system_fonts.{h,cc}` | font allow-lists and matcher (pure, + `fonts_unittest.cc`) and their Blink glue (+ `system_fonts_test.cc`) |
| `src/third_party/blink/public/web/web_molt_fingerprinting.h` | embedder entry point (mode, seed, Shields-off sites) |
| `src/chrome/common/molt_ai/fingerprinting/` | feature, mode switch, `molt_fingerprinting.mojom` |
| `src/chrome/browser/molt_ai/fingerprinting/` | session seed, mode switch, seed + sites over mojo, chrome://flags entry |
| `src/chrome/browser/molt_ai/shield/molt_shield_site_exceptions.{h,cc}` | Shields-off sites from content settings (+ unit test) |
| `src/chrome/renderer/molt_ai/fingerprinting/` | hands mode, seed and sites to Blink |
| `src/chrome/browser/molt_ai/tor/molt_net_routing.cc` | MoltNet `Accept-Language` |

Upstream files with hooks (each marked `MoltBrowser:`; a few lines each):
`chrome/browser/chrome_content_browser_client.cc`,
`chrome/renderer/chrome_content_renderer_client.cc`,
`chrome/renderer/BUILD.gn`, `chrome/browser/about_flags.cc`,
`chrome/browser/flag-metadata.json`,
`chrome/browser/flag-never-expire-list.json`,
`third_party/blink/public/BUILD.gn`,
`third_party/blink/renderer/core/BUILD.gn`, and in Blink
`html_canvas_element.cc`, `canvas_async_blob_creator.cc`,
`base_rendering_context_2d.cc`, `webgl_rendering_context_base.cc`,
`webgl2_rendering_context_base.cc`, `webgl_debug_renderer_info.cc`,
`video_frame.cc`, `gpu_queue.cc`, `audio_buffer.cc`, `analyser_node.cc`,
`audio_worklet_processor.cc`, `threaded_worklet_messaging_proxy.cc`,
`service_worker_global_scope.cc`, `navigator_base.cc`, `screen.cc`,
`local_dom_window.cc`, `mouse_event.cc`; shared workers:
`content/renderer/worker/embedded_shared_worker_stub.cc`,
`web_shared_worker.h`, `web_shared_worker_impl.{h,cc}`; MoltNet GPU:
`gpu.cc` (WebGPU); fonts: `font_selector.{h,cc}`,
`font_fallback_list.cc`, `css_font_selector_base.{h,cc}`,
`local_font_face_source.cc`, `font_access.cc`; letterboxing:
`chrome/browser/ui/views/frame/contents_web_view.{h,cc}`,
`chrome/browser/profiles/off_the_record_profile_impl.cc`,
`media_values.cc`, `html_image_element.cc`, `style_resolver_state.cc`;
MoltNet touches, screens and forms: `touch_event_manager.cc`,
`pointer_event_factory.cc`, `window_screen_details.cc`,
`permission_status_listener.cc`, `screen_orientation.cc`,
`screen_orientation_controller.cc`, `platform_locale.{h,cc}`.

## Follow-ups

1. Build and run everything above on the build machine; add browser tests
   (canvas/WebGL/audio values stable within a site, different across sites
   and sessions, unfarbled with Shields off; MoltNet values uniform; the
   seed absent from renderer command lines).
2. Make canvas-to-video export cheap enough for video-call effects: a
   faster farbling loop, or noise applied on the GPU in the copy that
   `captureStream` / `VideoFrame` already do.
3. (Dropped: the channel vote, see known attacks. A real answer to
   scripts written against the noise is a different design, such as a
   prompt before canvas readback, not a noise revision.)
4. (Done: third-party shared workers use their top-level site's key, see
   [Shared workers](#shared-workers).)
5. (Done: real letterboxing of MoltNet windows, CSS device media queries and
   `devicePixelRatio`, see [Letterboxing](#letterboxing-moltnet).) Left:
   letterboxing the CSS size so zoom stops showing, dropping DPR client
   hints in MoltNet, opening MoltNet windows at a 1400 x 900 page, and the
   checks on the build machine listed there.
6. Fonts: the allow-lists are in (see [Fonts](#fonts)); still to do: check
   the Windows and macOS lists against fresh installs of each supported
   version, pin the generic-font settings in MoltNet, and text metrics /
   per-character fallback. The WebGL parameter surface and WebGPU in
   MoltNet are done (fixed WebGL baselines, no WebGPU adapter); left: a
   mobile look at the baselines if MoltNet comes to Android, and checking
   them against the WebGL reports of real Macs, Windows and Linux machines
   in MoltNet windows.
7. Per-document Shields-off delivery instead of a per-profile list.
8. MoltNet: renderer UI strings in en-US (design in [Form controls in
   en-US](#form-controls-in-en-us)) and the default text locale; then a
   touchscreen-less look (`maxTouchPoints`, pointer media queries) and
   permission states that don't inherit the regular profile's blocks.
   Done: touch coordinates and contact, the Window Management API, screen
   orientation, form-control formats ([MoltNet: touches, screens,
   orientation and form
   controls](#moltnet-touches-screens-orientation-and-form-controls)).
