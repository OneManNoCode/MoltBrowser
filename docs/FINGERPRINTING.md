# Fingerprinting protection (v1)

MoltBrowser's answer to audit finding **P3** ("no fingerprinting defense").
Everything happens in the engine (Blink and the browser process), not in
page scripts, so pages can't remove it and it covers workers too.

* **Normal and Incognito windows: farbling.** Device-dependent outputs
  (canvas and WebGL pixels, Web Audio samples, `hardwareConcurrency`) get
  small, deterministic noise keyed **per site and per session**. A site sees a
  stable fingerprint for the whole session (pages don't break or flicker);
  two sites see unrelated fingerprints; the next session sees new ones.
* **MoltNet (Tor) windows: uniformity + farbling.** Values that vary between
  users are replaced by one value for everybody (UTC, en-US, letterboxed
  screen metrics, 4 cores, no unmasked GPU strings), and the farbling above
  stays on, keyed by a seed that changes with every New Identity.

The design follows Brave's "farbling" and Tor Browser's uniformity
(letterboxing, UTC, en-US). No Brave or Tor Browser code is used.

## Threat model

Protects against: websites and third-party scripts that read
device-dependent values to recognise the same browser across sites (or, in
MoltNet, to tell MoltNet users apart), including scripts running in iframes
and dedicated workers.

Does not protect against (see [Known gaps and attacks](#known-gaps-and-attacks)):

* an adaptive attacker who targets MoltBrowser's noise specifically;
* local attackers: the session seed is on the renderer's command line;
* a compromised renderer;
* fingerprinting surfaces outside the v1 list below (fonts, WebGL
  parameters, WebGPU, ...).

## How it works

```
 browser process                                renderer process
 ---------------                                ----------------
 Profile (regular / Incognito / MoltNet)
   session seed: 32 random bytes, in memory
   only (BrowserContext user data)
        |
 ChromeContentBrowserClient::
   AppendExtraCommandLineSwitches  ---------->  --molt-fingerprinting-mode=farble|uniform
   (molt_fingerprinting_browser.cc)             --molt-fingerprinting-seed=<64 hex>
                                                        |
                                    ChromeContentRendererClient::RenderThreadStarted
                                    -> blink::WebMoltFingerprinting::Configure()
                                                        |
                                    Blink (core/molt_fingerprinting): per execution
                                    context, key = HMAC-SHA256(seed, top-level site)
                                    -> hooks in canvas / WebGL / audio / navigator /
                                       screen / window / mouse events
```

**Why one mode and seed per renderer process is exact.** A renderer process
only ever hosts one BrowserContext: `RenderProcessHostImpl::IsSuitableHost`
rejects any host whose BrowserContext differs from the one being navigated,
and the spare-renderer manager only hands out a spare whose BrowserContext
matches (both checked in the pinned sources). The only exception is
`--single-process`, which supports a single context anyway. A crashed
renderer is relaunched for the same context, so it gets the same seed.

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
| Shared worker, service worker | Blink doesn't know their top-level site: **random per-worker key** (see gaps) | - |
| Top-level site opaque (e.g. CSP `sandbox` top-level document) | random per-context key | - |
| Top-level site not http(s) (`chrome://`, extensions, `file://`) | **not farbled** | - |

Third-party iframes therefore use the **top-level** site's key: a tracker
embedded on two sites sees two unrelated fingerprints.

## Surface covered

| API | Normal windows | MoltNet windows | Hook (upstream file: function) |
|---|---|---|---|
| `canvas.toDataURL()` | farbled | farbled | `html_canvas_element.cc`: `ToDataURLInternal` |
| `canvas.toBlob()`, `OffscreenCanvas.convertToBlob()` (windows and workers) | farbled | farbled | `canvas_async_blob_creator.cc`: constructor |
| `getImageData()` (2D and OffscreenCanvas 2D; unorm8, float16, float32 storage) | farbled | farbled | `base_rendering_context_2d.cc`: `getImageDataInternal` |
| WebGL 1/2 `readPixels()` into client memory, `RGBA`/`UNSIGNED_BYTE` | farbled | farbled | `webgl_rendering_context_base.cc`: `ReadPixelsHelper` |
| `WEBGL_debug_renderer_info` | real | **unavailable** (null from `getExtension`, absent from `getSupportedExtensions`) | `webgl_debug_renderer_info.cc`: `Supported` |
| `AudioBuffer.getChannelData()`, `copyFromChannel()` | farbled | farbled | `audio_buffer.cc` (binding entry points only) |
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

The screen "viewport" is the outermost main frame's visible viewport, so every
frame of a page (cross-process iframes included) reports the same screen.

### Already handled by Chromium (left alone)

* User-Agent string: Chromium's UA reduction (frozen minor version and
  platform). MoltBrowser also keeps the stock "Chromium" brand in
  `Sec-CH-UA` / `navigator.userAgentData` (audit P4).
* `navigator.deviceMemory` and `Sec-CH-Device-Memory`: already rounded to a
  power of two and capped at 8 GB.
* `enumerateDevices()` IDs: salted per origin and per profile, so a new
  MoltNet profile gets new IDs.
* `Accept-Language` / `navigator.languages` in off-the-record profiles: only
  the first language is sent.

## The noise

All noise lives in `third_party/blink/renderer/core/molt_fingerprinting/farbling.{h,cc}`
(depends only on //base and //crypto) and is a pure function of the per-site
key and the **value** being read: never of its position, the size of the
read, or the rest of the buffer.

* **Canvas / WebGL pixels** (unpremultiplied RGBA; BGRA, float16 and float32
  data are mapped onto the same 8-bit decision): fully transparent pixels are
  never touched (blank-canvas checks such as signature pads keep working).
  For each colour, one RGB channel is picked from its alpha; that channel's
  256 values are grouped into pairs whose alignment ({2k,2k+1} or
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
* **Audio samples**: the low 8 bits of the 23-bit mantissa are replaced by
  key-derived bits of the rest of the sample: a relative change below 2^-15
  (about -90 dB, inaudible), far larger than the 1-2 ulp differences between
  machines that audio fingerprints rely on. Zero, subnormal, infinite and NaN
  samples are left alone (silence stays silent). Idempotent, so repeated
  `getChannelData()` calls never compound. `getChannelData()` returns the
  live array, so the noise is applied in place; the common "get the array,
  then write samples" pattern leaves the page's own samples untouched until
  its next `getChannelData()` call.
* **AnalyserNode byte data**: the same pair-collapse on byte values, never
  touching 0 (frequency data) or 128 (silence in time-domain data).
* **hardwareConcurrency**: `2 + PRF(key) mod (real - 1)`, i.e. in [2, real].
  MoltNet uses 4: one constant for everybody is the point, and 4 is at or
  below the logical core count of essentially every machine MoltBrowser runs
  on, so pages sizing worker pools from it don't oversubscribe weak machines,
  while 2 would needlessly halve throughput on most.

**Cost** (measured on the 2.1 GHz VM these changes were developed on, where
an 8 MB `memcpy` takes 5.7 ms; expect a fraction of this on a current Mac):
farbling a 1920x1080 RGBA8 read takes ~3 ms for flat content, ~10 ms for
text-like content and ~35-50 ms when every pixel is a different colour;
10 s of 44.1 kHz audio takes ~1.4 ms. Nothing is copied or computed when
protection is off for the context.

## Kill switch

`chrome://flags/#molt-fingerprinting-protection` (feature
`MoltFingerprintingProtection`, enabled by default). When disabled, new
renderers get no fingerprinting switches (Blink runs unmodified) and MoltNet
no longer forces en-US; relaunch to apply it to every renderer. It is
independent of `--disable-molt-shield` on purpose: that switch is an
ad-blocking escape hatch and must not silently drop MoltNet uniformity.

MoltShield's per-site "Shields off" does **not** turn farbling off for that
site yet (see follow-ups).

## Known gaps and attacks

Honest list; anything not in the table above is not covered.

**Readback paths not covered**

* WebCodecs `new VideoFrame(canvas)` + `copyTo()`, `canvas.captureStream()`
  frames (MediaRecorder, `ImageCapture.grabFrame()`,
  `MediaStreamTrackProcessor`).
* WebGPU: canvas readback through `copyExternalImageToTexture`/`mapAsync`,
  and `GPUAdapter.info`.
* WebGL2 `readPixels` into a `PIXEL_PACK_BUFFER` followed by
  `getBufferSubData`; `readPixels` with float or integer formats/types.
* The rest of the WebGL surface: limits (`MAX_*`), extension lists, shader
  precision formats.
* Text metrics and fonts: `measureText()`/`TextMetrics`, font enumeration via
  layout, `isPointInPath`.
* Web Audio inside an `AudioWorklet` (processors see raw samples) and audio
  routed to `MediaStreamAudioDestinationNode` -> `MediaRecorder`.

**Context coverage**

* Shared and service workers get a random key per worker instance: their
  values are unlinkable but not stable across worker restarts, and differ
  from their pages'. Fix: plumb the constructor storage key's top-level site
  into `WebSharedWorker::CreateAndStart` / the service worker's
  `GlobalScopeCreationParams` (follow-up).
* Worklets aren't keyed (they expose none of the farbled APIs except the raw
  `AudioWorklet` samples above).

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
* *Coarse quantisation*: the noise is at most 1 per channel, so a script that
  compares canvases with a robust/perceptual hash instead of exact bytes can
  ignore it. Audio: masking the low mantissa bits removes the noise, but also
  the device-specific signal it hides.

**Other limitations**

* The session seed is on the renderer's command line: it never touches disk,
  but other processes of the same user (and, on Linux/macOS, `ps` for other
  local users) can read it. With it, a local observer could compute the
  per-site noise. Outside the web-attacker threat model; a
  channel-associated `RendererConfiguration` message would avoid it
  (follow-up).
* `getChannelData()` writes the noise into a buffer that an
  `AudioBufferSourceNode` on the audio thread may be playing at the same
  time; this is the same benign race as a page writing to that array itself.
* No per-site opt-out (Shields off) yet; the only escape hatch is the global
  flag.
* Compatibility: about 1 colour in 16 of exported/read pixels and the low
  mantissa bits of read audio change. Invisible and inaudible, but tools that
  rely on exact values (pixel-art palettes, exact-colour tests, bit-exact
  audio analysis) will see it.

## Verification status

Verified without a Chromium checkout:

* `farbling_unittest.cc` (22 tests) passes standalone with g++
  (ASan/UBSan/LSan) and clang (UBSan trap; `-Wunsafe-buffer-usage
  -Wshorten-64-to-32 -Wimplicit-int-conversion -fno-exceptions`); HMAC
  vectors from Python's `hmac`, and 80,000 random pixel/sample/byte/
  hardwareConcurrency outputs matched against an independent Python model.
* `molt_fingerprinting.cc` (Blink glue) type-checks with the same strict
  clang flags against stand-ins of the pinned Blink/Skia/V8 signatures, and
  an 11-test harness runs the real glue: key resolution for windows (site,
  nonce, opaque, non-web, re-derivation), dedicated workers sharing their
  page's key, the shared/service-worker fallback, WebGL pack layouts, the
  snapshot copy pipeline, the uniform screen math and the UTC/en-US side
  effects.
* The chrome/{common,browser,renderer} glue type-checks against stand-ins
  of the pinned content/profile signatures; the switch helpers' unit tests
  pass; the chrome://flags macro expands against a `FeatureEntry` of the
  pin's shape; both flag JSON files parse with the entry sorted.
* `tools/chromium/verify_patches.py`: `patches/` reproduces `src/`.

Needs the build machine (not verified here): compiling and linking the real
Blink/Chrome targets, `blink_unittests` (`MoltFarblingTest.*`,
`MoltFingerprintingTest.*`), `molt_ai_unittests`, and the behaviour in a
running browser: test pages for every row of the table above, in a normal
window, an Incognito window and a MoltNet window, plus fingerprinting suites
(e.g. CreepJS, FingerprintJS) across two sites and two sessions.

## Code map

| Where | What |
|---|---|
| `src/third_party/blink/renderer/core/molt_fingerprinting/farbling.{h,cc}` | pure math (+ `farbling_unittest.cc`) |
| `src/third_party/blink/renderer/core/molt_fingerprinting/molt_fingerprinting.{h,cc}` | Blink glue (+ `molt_fingerprinting_test.cc`) |
| `src/third_party/blink/public/web/web_molt_fingerprinting.h` | embedder entry point |
| `src/chrome/common/molt_ai/fingerprinting/` | feature, switch names, seed encoding |
| `src/chrome/browser/molt_ai/fingerprinting/` | session seed, renderer switches, chrome://flags entry |
| `src/chrome/renderer/molt_ai/fingerprinting/` | hands the switches to Blink |
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
`webgl_debug_renderer_info.cc`, `audio_buffer.cc`, `analyser_node.cc`,
`navigator_base.cc`, `screen.cc`, `local_dom_window.cc`, `mouse_event.cc`.

## Follow-ups

1. Build and run everything above on the build machine; add browser tests
   (canvas/WebGL/audio values stable within a site, different across sites
   and sessions; MoltNet values uniform).
2. Per-site opt-out tied to MoltShield's "Shields off" (needs a per-document
   signal and propagation to the document's dedicated workers, since the
   mode is per process).
3. Stable keys for shared and service workers (plumb the top-level site).
4. Real letterboxing of MoltNet windows (round the content area itself), then
   CSS device media queries and `devicePixelRatio`.
5. Close readback bypasses: WebCodecs `VideoFrame` from canvas,
   `captureStream`, WebGPU, WebGL2 pack buffers, float `readPixels`,
   `AudioWorklet`.
6. Fonts and text metrics (an allow-list of system fonts per platform, as
   Tor Browser and Brave do), and the WebGL parameter surface in MoltNet.
7. Move the seed off the command line (channel-associated
   `RendererConfiguration` message from the browser).
8. MoltNet: renderer UI strings and form-control formats in en-US; touch
   coordinates; Window Management API.
