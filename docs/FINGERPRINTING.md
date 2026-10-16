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
  canvas and WebGL fingerprints can't be laundered through WebGPU; WebGPU's
  own rendering differences, `GPUAdapter.info`, limits and features, and
  timing are not addressed.
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
* The rest of the WebGL surface: limits (`MAX_*`), extension lists, shader
  precision formats.
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
  v1.1; fixing it means changing the canvas noise itself (e.g. moving all
  three channels by the same keyed decision), which changes every
  canvas fingerprint and is left for a deliberate noise revision.
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
3. The channel-vote weakness of the canvas noise (see known attacks).
4. Stable keys for third-party shared workers (plumb the constructor
   storage key).
5. Real letterboxing of MoltNet windows (round the content area itself), then
   CSS device media queries and `devicePixelRatio`.
6. Fonts and text metrics (an allow-list of system fonts per platform, as
   Tor Browser and Brave do), the WebGL parameter surface and WebGPU
   (`GPUAdapter.info`, or WebGPU off) in MoltNet.
7. Per-document Shields-off delivery instead of a per-profile list.
8. MoltNet: renderer UI strings and form-control formats in en-US; touch
   coordinates; Window Management API.
