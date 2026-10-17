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
* fingerprinting surfaces outside the list below (fonts, WebGL parameters,
  WebGPU rendering, `GPUAdapter.info`, ...).

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
(`SharedWorkerFactory`), so in principle one could run before the seed is
processed; any context that needs a key before the seed arrived gets a
random key for its whole life (fail closed: its values are unlinkable, just
not stable). The renderer exposes the interface only when its mode is
farble or uniform.

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
| First-party shared worker | its site for cookies, i.e. its storage key's top-level site (= its own site) | - |
| Third-party (partitioned) shared worker | Blink is only told the constructor's origin: **random per-worker key** (see gaps) | - |
| Worklet (e.g. `AudioWorklet`) | the creating document's key, handed over at creation (`ThreadedWorkletMessagingProxy::Initialize`) | (document's) |
| Top-level site opaque (e.g. CSP `sandbox` top-level document) | random per-context key | - |
| Top-level site not http(s) (`chrome://`, extensions, `file://`) | **not farbled** | - |

Third-party iframes therefore use the **top-level** site's key: a tracker
embedded on two sites sees two unrelated fingerprints.

## Shields off

MoltShield's per-site switch is stored as an explicit `ADS` content-settings
exception (`molt_shield_site_settings.h`). In normal and Incognito windows,
farbling is off for every top-level site the user turned Shields off for:
the page, its iframes (they use the top-level site), its dedicated workers,
worklets it creates and service/first-party shared workers of that site.
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
* Text metrics and fonts: `measureText()`/`TextMetrics`, font enumeration via
  layout, `isPointInPath`.

**Context coverage**

* Third-party (partitioned) shared workers get a random key per worker
  instance: their values are unlinkable but not stable across worker
  restarts, and differ from their pages'. Blink is only told their
  constructor's origin; fixing it means plumbing the constructor storage key
  through `WebSharedWorker::CreateAndStart` and the content stub.
* Worklets other than threaded ones created by a document (e.g. shared
  storage worklets) get no key and are not farbled; they expose none of the
  farbled APIs.

**MoltNet uniformity not done**

* No real letterboxing of the window: `innerWidth/innerHeight`, CSS
  `width/height` media queries and layout still reveal the true viewport,
  and `screen.width` can be smaller than `innerWidth`.
* CSS `device-width/device-height` media queries, `devicePixelRatio`,
  `TouchEvent` screen coordinates, the Window Management API
  (`getScreenDetails()`).
* Renderer-side UI strings (`validationMessage`, the file input's label) and
  date/time form-control formats follow the browser's UI language, not
  en-US.
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
`local_dom_window.cc`, `mouse_event.cc`.

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
4. Stable keys for third-party shared workers (plumb the constructor
   storage key).
5. Real letterboxing of MoltNet windows (round the content area itself), then
   CSS device media queries and `devicePixelRatio`.
6. Fonts and text metrics (an allow-list of system fonts per platform, as
   Tor Browser and Brave do). (The WebGL parameter surface and WebGPU in
   MoltNet are done: fixed WebGL baselines and no WebGPU adapter. Left: a
   mobile look at the baselines if MoltNet comes to Android, and checking
   them against the WebGL reports of real Macs, Windows and Linux machines
   in MoltNet windows.)
7. Per-document Shields-off delivery instead of a per-profile list.
8. MoltNet: renderer UI strings and form-control formats in en-US; touch
   coordinates; Window Management API.
