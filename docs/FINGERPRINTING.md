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
* fingerprinting surfaces outside the list below (per-character font
  fallback, text measured through element geometry and, outside MoltNet,
  `measureText()`, see [Text metrics](#text-metrics-moltnet); WebGL
  parameters, WebGPU rendering, `GPUAdapter.info`, ...).

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
        |
 per commit (ReadyToCommitNavigation),
 frame-associated ShieldsDecision:
        +-- SetForNextDocument(down, site) ----->  ShieldsDecisionAgent -> the new
        |                                            document (DidCreateNewDocument)
 per service / shared worker start:
        +-- with its storage key --------------->  the worker, before its script
                                                            |
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
creating the channel and before `InitializeRenderer`, any frame creation or
navigation commit; channel messages are delivered in order, so documents
always have it. Service and shared workers are started over other pipes (at
152, `EmbeddedWorkerInstance` and `SharedWorkerServiceImpl` bind their
clients with `RenderProcessHost::BindReceiver`, which goes over the
`mojom::ChildProcess` pipe, not the channel), so in principle one could
start before the seed is processed. A worker's key is only derived when
the worker first needs one (once its script has been loaded and runs), not
when it starts, so the seed only has to arrive before that first farbled
read. Any context that does need a key before the seed arrived gets a
random key for its whole life (fail closed: its values are unlinkable, just
not stable). The renderer exposes the interface only when its mode is
farble or uniform. This channel carries only the seed: whether Shields are
off is decided per document and per worker and sent with each of them (see
[Shields off](#shields-off)), never as a per-process list.

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
farbling is off for every page whose top-level site the user turned Shields
off for: the page, its iframes (they take the top-level site's decision),
the dedicated workers and worklets it creates, and service and shared
workers whose storage key has that top-level site (third-party ones
included).
MoltNet keeps farbling regardless: there the point is to look like every
other MoltNet user, and the real canvas/audio values would link New
Identity sessions on that site. MoltNet windows also don't inherit the
regular profile's Shields-off sites, so MoltShield filters them there
([settings from the regular
profile](#moltnet-settings-from-the-regular-profile)); turning Shields off
in a MoltNet window lasts for that session. MoltShield's *global* switch
and the `ADS` *default* don't affect farbling (independence from
`--disable-molt-shield` is deliberate, see the kill switch below).

* **Which exceptions count.** A site has Shields off if its own URL
  (`https://example.com/`) matches an explicit Shields-down exception
  (`shield::HasShieldsDownException`). The site-scoped
  `https://[*.]example.com` the MoltShield UI writes therefore turns
  farbling off on all of example.com; a narrower exception (one subdomain)
  doesn't, because the farbling key is shared by the whole site.

### Per document and per worker, decided by the browser

No renderer holds the profile's list of Shields-off sites. The browser
decides "farble or not" for each document it commits and for each service
or shared worker it starts, from the top-level site and the user's setting
at that moment, and sends only that bit (with the site it was decided for),
only to the renderer that gets the document or the worker. A renderer can't
ask about a site.

| Context | Decision | How it gets there |
|---|---|---|
| A document the browser commits (any cross-document navigation: links, reloads, error pages, `srcdoc`, `about:blank` and `data:` URLs that go through the browser, MHTML) | the Shields setting of the top-level site of its frame tree: for a main frame (outermost, a fenced frame's root, a prerendered page's) the origin it commits, for an iframe the committed origin of its tree's main frame; an opaque origin counts as its precursor's site (CSP-sandboxed pages) | `ShieldsDecisionObserver::ReadyToCommitNavigation` (browser) sends `ShieldsDecision.SetForNextDocument` to the committing frame; `ShieldsDecisionAgent::DidCreateNewDocument` (renderer) hands it to the new document (`WebMoltFingerprinting::SetDocumentShieldsDecision`) |
| An iframe, cross-process ones included | the top-level site's, as above; its own site doesn't count | the same, to the iframe's own renderer |
| An initial empty document (a new iframe, `window.open()`) | its parent's, or else its opener's, if that frame is in the same process | Blink, when the agent is created (`RenderFrameCreated`) |
| A synchronous `about:blank`, `javascript:` URL or XSLT document | its frame's latest decision from the browser, or else its parent's or opener's | `DidCreateNewDocument` |
| Same-document navigations | unchanged (same document) | nothing is sent |
| Back/forward cache restores, prerender activations | unchanged: the documents keep the decisions they committed with | nothing is sent |
| Dedicated workers (nested too) | their creator's | `DedicatedWorker::CreateGlobalScopeCreationParams` hands it over in the worker's `WorkerClients` |
| Worklets | their creator's (the farbling key handed over, or none) | `ProvideToWorklet`, unchanged |
| Service workers | the Shields setting of their storage key's top-level site, decided when the browser starts the worker | `ContentBrowserClient::AreMoltShieldsDownForWorker`; sent with the storage key in `ServiceWorker.InitializeGlobalScope` |
| Shared workers | the same for the constructor's storage key | the same; sent in `SharedWorkerInfo` with `CreateSharedWorker` |

**Fail safe.** A context without a decision is farbled: farbling can break a
page, but it never gives a fingerprint away. And a window's decision only
counts for the top-level site it was made for: Blink compares it with the
site of the top frame of the window's frame tree (`FrameTree::Top()`, local
or remote, the precursor's for an opaque origin) and farbles on any
mismatch. Whatever the timing, a decision can't reach a document of another
site.

**Why the decision is there before any script runs (152).**

1. `NavigationRequest::CommitNavigation()` (and `CommitErrorPage()`) call
   `ReadyToCommitNavigation()`, which runs the `WebContentsObserver`s
   synchronously, and only then send the commit
   (`RenderFrameHostImpl::CommitNavigation` over `mojom::NavigationClient`).
2. The observer sends the decision through
   `RenderFrameHost::GetRemoteAssociatedInterfaces()`, the frame's route on
   the renderer's IPC channel. For browser-initiated navigations the
   `NavigationClient` comes from that same provider; for renderer-initiated
   ones it is the renderer's own endpoint, passed over the channel-associated
   `FrameHost`. Same channel, decision first.
3. In the renderer, `ChannelAssociatedGroupController::Accept` posts every
   message for a main-thread endpoint to the IPC task runner in arrival order
   ("so that FIFO can be guaranteed across all interfaces"), also while the
   endpoint is still being bound. So `SetForNextDocument` runs before
   `CommitNavigation`. (The agent binds on `kInternalNavigationAssociated`,
   like content's own frame interfaces.)
4. `RenderFrameImpl::CommitNavigation` -> `DocumentLoader::CommitNavigation`
   installs the new document and then calls
   `RenderFrameImpl::DidCommitNavigation`, whose `DidCreateNewDocument`
   gives Blink the decision, all synchronously and before
   `StartLoadingResponse` creates the parser, so before any page script.
   (MHTML archives commit later, still after the decision.) Only embedder
   and DevTools hooks at script-context creation run in between (DevTools'
   "evaluate on new document" scripts, for example); they see the fail-safe
   (farbled).
5. With RenderDocument (on for all frames at 152), every cross-document
   navigation after a frame's first commits in a new RenderFrameHost, a
   fresh provisional frame no script can reach, so the decision can only
   be taken by the document it was sent for. Where a frame is reused (its
   first navigation away from the initial empty document, or with
   RenderDocument off), a document the renderer makes by itself between the
   decision and the commit can pick it up, but only if it has the same
   top-level site (the check above): the same decision, unless the user
   toggled Shields for that very site in that very instant.

Workers: a service worker's decision comes with
`ServiceWorker.InitializeGlobalScope`, which must arrive before any of its
script runs; a shared worker's with `CreateSharedWorker`, before its thread
starts. Dedicated workers and worklets get theirs from their creator, on the
creator's thread, before theirs starts.

**Turning Shields off or on** applies from the next navigation: decisions
are made from the settings as they are at each commit and worker start, and
never change for a live document or worker. MoltShield's toggle reloads the
page, so the page and its frames get the new decision; iframes that
navigate later in a page that wasn't reloaded get the new one too. Pages in
the back/forward cache or prerendered keep theirs: they already ran with
it, so restoring them shows them nothing they hadn't seen. Service and
shared workers get the new decision when they next start (an idle service
worker stops after about 30 seconds).

**Privacy.** A renderer learns a decision only for a document or worker it
hosts, about that context's own top-level site, which the renderer is told
anyway (the committed origin, the storage key). A compromised renderer
can't list the user's Shields-off sites: it can't ask, and a site's
documents only commit in its process where the browser puts them.
Previously every renderer of the profile received the whole list
(`SetUnfarbledSites`) at launch and on every change; that list, its
renderer-side copy and the content-settings observer that re-sent it are
gone (follow-up 7).

**Choices.**

* The decision follows the top frame of the frame tree, as the browser's
  `RenderFrameHost::GetMainFrame()` and Blink's `FrameTree::Top()` agree:
  a fenced frame takes its own root's site, a credentialless iframe the
  page's. Their farbling keys stay nonce-keyed as before.
* A sandboxed top-level page (opaque origin) takes its precursor's site's
  decision; it used to be farbled even with Shields off.
* A popup's initial empty document inherits its opener's decision only if
  its own site matches. A popup that a third-party iframe opens has the
  iframe's origin: it is that third party's own top-level page, so the
  embedding page's "Shields off" doesn't follow it (farbled until its own
  navigation commits).
* The browser observer is attached to every WebContents
  (`AttachUniversalWebContentsObservers`), not only tabs.

Shields off is also the per-site escape hatch for the costs below (canvas
video pipelines, PBO readback).

**Verification.** Without a Chromium checkout: `shields_decision.cc`,
`molt_fingerprinting.cc`, `shared_worker_key.cc` (and the other glue
including them) type-check with the strict clang flags against stand-ins
of the pinned Blink signatures, and a 17-test harness runs the real glue
under ASan/UBSan in kOff, kFarble and kUniform (the fail-safe, the site
check, MoltNet ignoring decisions, non-web and sandboxed top-level pages,
inheritance from local and remote parents and from openers, a tracker's
popup, the public entry points, dedicated, shared and service workers,
worklets); 12 mutants of the glue all fail it. The browser side
(`molt_shields_decision.cc`, `molt_shields_delivery.cc`) and the renderer
agent type-check against stand-ins of the pinned content, mojo, url and
net signatures, and a 9-test harness runs the real decision and delivery
code (which origin a commit is decided for, which frame it is sent to,
what is skipped, MoltNet and the kill switch, workers); 7 mutants all fail
it. **Needs the build machine:** compiling the hooks and the mojom changes,
`blink_unittests` (`MoltShieldsDecisionTest.*`,
`MoltSharedWorkerKeyTest.*`, `MoltFingerprintingTest.*`,
`MoltSystemFontsTest.*`), `molt_ai_unittests` (`MoltShieldsDecisionTest.*`,
`MoltShieldSiteExceptionsTest.*` with a real `HostContentSettingsMap`),
`content_unittests` (the service and shared worker fakes), and in a running
browser: a page with Shields off and on (canvas, `hardwareConcurrency` in
the page, an about:blank iframe, a cross-site iframe, a dedicated worker, a
service worker and a shared worker; a popup), reloading after the toggle,
back/forward, and a MoltNet window with Shields off.

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
| `MediaRecorder`: the default codec, `isTypeSupported()`, the encoder and its threads | the GPU's encoders where it has them; threads by the machine's cores | **the build's software encoders**, threads for **4** cores ([details](#moltnet-mediarecorder)) | `video_track_recorder.cc`, `media_recorder_handler.cc`, `media/base/video_encoder.cc` |
| Decoded video read back (`VideoFrame.copyTo()`, `new VideoFrame(video)`, `drawImage(video)`, `texImage2D(video)`); the bytes `VideoEncoder`, `AudioEncoder` and `MediaRecorder` write for input they scale or resample, and Opus | the GPU's video decoder and colour conversion, the CPU's vector instructions | **software decoding, CPU conversion, an integer scaler, C kernels**: the same on every machine of an OS ([details](#moltnet-pixels-and-encoded-bytes)) | `default_decoder_factory.cc`, `mappable_shared_image_video_frame_pool.cc`, `paint_canvas_video_renderer.cc`, `video_frame_converter_internals.cc`, `sinc_resampler.cc`, `vector_math.cc`; libopus |
| WebAssembly relaxed SIMD, `Math.pow` and `Math.tanh`, colours read back in another colour space, images with a colour profile, decoded audio (`AudioData`); how much heap V8 gives a page | the CPU's vector extensions (relaxed SIMD's results, the code Skia, skcms, Symphonia, FFmpeg and the C library pick); the machine's memory | **one code path for every CPU**: relaxed SIMD off, V8's own `Math` code, portable colour and audio code; V8 configured as on a 4 GB machine ([details](#moltnet-cpu-features)) | `flag-definitions.h`, `function-body-decoder-impl.h`, `ieee754.cc`, `v8.cc`, `isolate_holder.cc`, `chrome_content_renderer_client.cc`; Skia, skcms, Symphonia, `molt_portable_media.cc` |
| WebGPU `queue.copyExternalImageToTexture()` from a canvas, `OffscreenCanvas`, `ImageBitmap`, `<img>`, `ImageData` | farbled (before upload) | farbled | `gpu_queue.cc`: `copyExternalImageToTexture` |
| `OffscreenCanvas.transferToImageBitmap()`, `createImageBitmap(canvas)` | covered where the bitmap is read (see below) | same | - |
| HTML-in-canvas (`drawElementImage`, WebGL `texElementImage2D`, WebGPU `copyElementImageToTexture` and `drawElementImageToTexture`) | **off**: the `HTMLInCanvas` origin trial's tokens are refused (only `chrome://flags` turns it on) | same | `origin_trial_policy_impl.cc`: `IsFeatureDisabled` |
| `WEBGL_debug_renderer_info` | real | **unavailable** (null from `getExtension`, absent from `getSupportedExtensions`) | `webgl_debug_renderer_info.cc`: `Supported` |
| WebGL limits (`getParameter` `MAX_*`, ranges, `MAX_VIEWPORT_DIMS`, ...), `getShaderPrecisionFormat`, `getInternalformatParameter(SAMPLES)` | real | **fixed baselines**, never above the real value ([details](#webgl-and-webgpu-in-moltnet-windows)) | `webgl_rendering_context_base.cc`, `webgl2_rendering_context_base.cc` |
| WebGL extensions (`getSupportedExtensions`, `getExtension`) | real | **fixed allow-list of 23**, intersected with the GPU's | `webgl_rendering_context_base.cc`: `ExtensionSupportedAndAllowed` |
| WebGPU `navigator.gpu.requestAdapter()` | real | **null** (no adapter) | `gpu.cc`: `RequestAdapterImpl` |
| Installed fonts: CSS `font-family`, canvas text, `document.fonts.check()`, `local()` | per-OS allow-list + 1 in 16 of the others, per site | **none: MoltBrowser's bundled font set**, the same on every OS, generic families and fallback included (per-OS allow-list in a build without the set) ([details](#fonts)) | `font_fallback_list.cc`, `font_selector.{h,cc}`, `css_font_selector_base.{h,cc}`, `local_font_face_source.cc`; the bundled set: `font_platform_data.{h,cc}`, the platforms' `font_cache*` ([details](#moltnet-the-bundled-font-set)) |
| `queryLocalFonts()` (after its permission prompt) | filtered like CSS | filtered like CSS | `font_access.cc`: `DidGetEnumerationResponse` |
| Canvas `measureText()` (`TextMetrics`, the `ExtendedTextMetrics` methods), windows and workers | real | **rounded**: whole CSS pixels, a 64th to a 128th of the font size above 127px ([details](#text-metrics-moltnet)) | `base_rendering_context_2d.cc`: `measureText`; `text_metrics.{h,cc}` |
| How glyphs are measured (canvas and layout), Linux | the desktop's hinting and the screen's scale factor | **unhinted with subpixel positioning** for everyone, as for `text-rendering: geometricPrecision` ([details](#text-metrics-moltnet)) | `font_platform_data.{h,cc}`: `QuerySystemRenderStyle` |
| Emoji fallback font, Linux | fontconfig's first colour emoji font | **Noto Color Emoji** when installed | `font_cache_linux.cc`: `PlatformFallbackFontForCharacter` |
| `AudioBuffer.getChannelData()`, `copyFromChannel()` (incl. `OfflineAudioContext` results and `ScriptProcessorNode`'s `inputBuffer`/`outputBuffer`) | farbled | farbled | `audio_buffer.cc` (binding entry points only) |
| `AudioWorkletProcessor.process()` `inputs` | farbled | farbled | `audio_worklet_processor.cc`: `CopyPortToArrayBuffers` |
| `AnalyserNode.get{Float,Byte}{Frequency,TimeDomain}Data()` | farbled | farbled | `analyser_node.cc` |
| Web Audio's output device: `AudioContext.sampleRate` (no rate given), `baseLatency`, `outputLatency`, `getOutputTimestamp()`, `playbackStats`, `destination.maxChannelCount`, `createScriptProcessor()`'s default size | the device's | **one 48 kHz stereo device** with a 10 ms buffer and a 24 ms output latency; the context runs at 48 kHz ([details](#moltnet-audio-drm-and-media-capabilities)) | `audio_context.cc`: `Create`, `outputLatency`, `getOutputTimestamp`, `HandlePreRenderTasks`; `realtime_audio_destination_handler.cc` |
| `requestMediaKeySystemAccess()`, `MediaKeys`, Media Capabilities with a `keySystemConfiguration` | the build's and the system's key systems (Clear Key in MoltBrowser's builds) | **none**: `NotSupportedError` | `encrypted_media_utils.cc`: `GetEncryptedMediaClientFromLocalDOMWindow` |
| `mediaCapabilities.decodingInfo()` / `encodingInfo()` | the build's decoders, then the decode history and the GPU | video: the build's decoders, **smooth**, **not power efficient**; `"webrtc"` **unsupported** | `media_capabilities.cc`: `decodingInfo`, `encodingInfo`, `GetPerfInfo` |
| WebCodecs: `isConfigSupported()` / `configure()` with `"prefer-hardware"`, the decoder or encoder used | the GPU's codecs | **software codecs only**; `"prefer-hardware"` unsupported | `gpu_factories_retriever.cc`, `video_decoder_broker.cc` |
| `navigator.hardwareConcurrency` (window and workers) | per site, in [2, real] | **4** | `navigator_base.cc`: `hardwareConcurrency` |
| `navigator.deviceMemory` (window and workers), `navigator.cpuPerformance` | real | **8**, **0** ("unknown") ([details](#moltnet-no-touchscreen-client-hints-memory-and-cpu)) | `navigator_device_memory.cc`; browser: `GetCpuPerformanceTierOverride` |
| `navigator.connection` (`effectiveType`, `rtt`, `downlink`, `saveData`; window and workers) and its `change` event; what Blink decides from the network quality (lazy-loading distances, the slow-network font and `document.write()` measures, media preload); request scheduling | the browser's estimate (`rtt` and `downlink` with a per-site factor) | **4g**, **200**, **1.6**, **false**, never changes; scheduling as on 4g ([details](#moltnet-connection-and-network-quality)) | `network_state_notifier.{h,cc}`: `SetUniformNetworkState`; network service: `network_context.cc` |
| `screen.width/height/availWidth/availHeight` | real | viewport in CSS pixels rounded **down** to 200x100 steps, max 1400x900 | `screen.cc`: `GetRect` |
| `screen.availLeft/availTop` | real | **0** | `screen.cc`: `GetRect` |
| `screen.colorDepth/pixelDepth`, `screen.isExtended` | real | **24**, **false** | `screen.cc` |
| `outerWidth/outerHeight` | real | = `innerWidth/innerHeight` | `local_dom_window.cc` |
| `screenX/screenY/screenLeft/screenTop` | real | **0** | `local_dom_window.cc` |
| `MouseEvent`/`PointerEvent`/`WheelEvent`/`DragEvent` `.screenX/Y` | real | relative to the viewport, in CSS pixels (as if the window sat at 0,0 with no browser chrome) | `mouse_event.cc`: `SetCoordinatesFromWebPointerProperties` |
| Page size (`innerWidth/innerHeight`, layout, width/height media queries) | real | **letterboxed**: the page area itself is rounded down to the screen steps above, in CSS pixels at every zoom level, and centred ([details](#letterboxing-moltnet)); the media queries, `vw`/`vh` and `visualViewport` report it in whole CSS pixels ([details](#zoom)) | browser: `contents_web_view.cc`: `GetInsets`, `PrimaryMainFrameWasResized`, `PrimaryPageChanged`; `local_frame_view.cc`, `dom_visual_viewport.cc` |
| Classic scrollbars' width | the theme's DIPs (15 or 17) over the page zoom | **16 CSS pixels** at every zoom, scale factor and OS ([details](#zoom)) | `scrollbar.cc`, `scrollable_area.cc`, `paint_layer_scrollable_area.cc`, `layout_text_control.cc` |
| Scrollbar style: overlay (takes no room) or classic | the OS setting (macOS's "Show scroll bars", which follows whether a mouse is connected; on Windows and Linux classic unless the overlay-scrollbars flag is on) | **the platform's default**: overlay on macOS, classic on Windows and Linux ([details](#moltnets-display-colour-and-scrollbar-style)) | `web_view_impl.cc`: `UpdateRendererPreferences` |
| `device-width/device-height` media queries | real | = the uniform screen | `media_values.cc` |
| `devicePixelRatio`, `resolution` media queries, `srcset`/`<picture>`/`image-set()` selection | real | **1** | `local_dom_window.cc`, `media_values.cc`, `html_image_element.cc`, `style_resolver_state.cc` |
| `color-gamut`, `dynamic-range`, `video-dynamic-range`, `color`, `monochrome` media queries | real | **srgb**, **standard**, **standard**, **8** bits, **0** ([details](#moltnets-display-colour-and-scrollbar-style)) | `media_values.cc`: `CalculateColorGamut`, `CalculateDeviceSupportsHDR`, `CalculateColorBitsPerComponent`, `CalculateMonochromeBitsPerComponent` |
| Client hints (`Sec-CH-DPR`, viewport, width, memory, network, high-entropy `Sec-CH-UA-*`), `navigator.userAgentData.getHighEntropyValues()` | sent / real | **never sent** but `Sec-CH-UA`, `-Mobile`, `-Platform`, `Save-Data` and the preference hints; high-entropy values **blank** ([details](#client-hints)) | `frame_fetch_context.cc`, `navigator_ua.cc`; browser: `client_hints.cc` (content), `client_hints_factory.cc` |
| Page zoom | regular profile's | **100%**, not inherited from the regular profile; a zoomed page is still letterboxed ([details](#zoom)) | `off_the_record_profile_impl.cc`: `Init` |
| Font families and sizes, default encoding, other user settings in WebPreferences/RendererPreferences (Do Not Track, caret browsing, focus highlight, ...) | the user's | **MoltBrowser's defaults** ([details](#moltnet-settings-from-the-regular-profile)); the default fonts, minimum logical font size and encoding **fixed per OS**, not by UI language ([details](#default-fonts-font-size-and-encoding)) | `off_the_record_profile_impl.cc`: constructor (MoltNet's pref service) |
| Permission and content-setting state (`navigator.permissions.query`, embargo, blocked JavaScript/images/..., Shields off), third-party cookies, Privacy Sandbox APIs | the profile's | **defaults + this session's changes** only, nothing from the regular profile | same |
| Installed extensions: web-accessible resources, page-to-extension messaging (`chrome.runtime`), content-script changes, blocked or rewritten requests | the user's (in Incognito: those allowed there) | **none**; only the built-in PDF viewer ([details](MOLTNET_COMPLIANCE.md#2-how-it-is-built)) | `extension_util.cc`: `IsIncognitoEnabled`, `IsExtensionVisibleToContext`; `web_request_api.cc` |
| Autoplay with sound and no user gesture (`navigator.getAutoplayPolicy()`, unmuted `play()`) | allowed on sites with high media engagement | **never** | `media_engagement_service.cc`: `HasHighEngagement` |
| `navigator.storage.persist()` | granted for bookmarked, installed or often-used sites | **false** | `persistent_storage_permission_context.cc`: `DecidePermission` |
| `navigator.storage.estimate()` quota | the usage + 10 GiB (the disk, rounded up to whole GiB, where it is smaller) | the usage + **10 GiB**, static in off-the-record profiles since Chromium 156 | none (upstream) |
| Storage limits a page reaches by writing: quota per site and for all sites (IndexedDB, Cache Storage, OPFS), blob memory | from the disk (regular profile) or the memory (Incognito) | **fixed**: 60 MiB per site (all its storage keys together), 600 MiB for all sites, nothing evicted for another site, 800 MiB of blobs ([details](#moltnet-storage-limits)) | `storage_partition_impl.cc`: `GetQuotaSettings`; `chrome_blob_storage_context.cc`: `GetFor`; `quota_manager_impl.cc`: `UsageAndQuotaInfoGatherer`; `quota_temporary_storage_evictor.cc`: `OnGotEvictionRoundInfo` |
| Third-party cookies, Shared Storage | the user's settings | third-party cookies **blocked** (as in Incognito); Shared Storage: removed upstream at 156 | `cookie_settings_factory.cc` |
| `Touch.screenX/Y`, touch contact (`radiusX/Y`, `rotationAngle`, `force`; touch `PointerEvent` `width/height/pressure/tilt/twist`) | real | like `MouseEvent`; contact **hidden** ([details](#touches)) | `touch_event_manager.cc`, `pointer_event_factory.cc` |
| `navigator.maxTouchPoints`, `pointer`/`any-pointer`/`hover`/`any-hover` media queries, touch-event feature detection (`ontouchstart`, `createEvent('TouchEvent')`) | real | a mouse-only desktop: **0**, **fine**, **hover**, **off** ([details](#touchscreens)) | browser: `chrome_content_browser_client.cc`: `OverrideWebPreferences` |
| `getScreenDetails()`, `window-management` permission state | real | **denied** (no prompt) | `window_screen_details.cc`, `permission_status_listener.cc` |
| `screen` and orientation `change` events | real | **never fired** | `screen.cc`, `screen_orientation_controller.cc` |
| `screen.orientation.type/angle` | real | from the uniform screen, angle **0** | `screen_orientation.cc` |
| Date/time/number form-control formats | UI language and region | **en-US** | `platform_locale.cc`: `DefaultLocale` |
| Renderer strings pages read or measure (`validationMessage`, the file input's labels, default `<details>` and button labels, date-field placeholders, permission element labels) | UI language | **en-US** ([details](#renderer-strings)) | none: `ui::ResourceBundle` overrides at renderer start (`moltnet_locale_strings.cc`) |
| Default text locale (text without `lang`: CJK glyph variants, hyphenation, line breaking), date picker language | UI language | **en-US** ([details](#default-text-locale)) | `language.cc`: `DefaultLanguage`, `UserPreferredLanguages` |
| Time zone (`Date`, `Intl`) | real | **Etc/UTC** (renderer-wide `TimeZoneController` override) | glue: `Configure` |
| `Intl` / ICU default locale | real | **en-US** (`LocaleController` override) | glue: `Configure` |
| `Accept-Language`, `navigator.language(s)` | real | **en-US** (MoltNet profile's `intl.accept_languages`, held above its own settings, so no language setting or policy changes it) | `molt_net_language_policy.h`, `molt_net_pref_isolation.cc` (MoltBrowser files) |

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

**Cost.** Measured with a scratch benchmark that links the previous loops
("before") and the current ones ("after") into one binary and runs them
alternately on the same inputs, calling them as `molt_fingerprinting.cc`
does (one call per row; one per buffer for audio). Clang 18, `-O2
-ffp-contract=off` as in Chromium builds, unchecked spans as in the earlier
figures; each figure is the median of 6 runs of the median of 15, on the
2.1 GHz VM these changes were developed on, where a warm 8 MB `memcpy`
takes about 0.65 ms. Expect a fraction of this on a current Mac. The two
versions' outputs were compared byte for byte (identical).

| Path | Content | Before | After |
|---|---|---|---|
| canvas readback, 1920x1080 RGBA8 | flat | 2.9 ms | 1.5 ms |
| | text-like | 8.6 ms | 5.2 ms |
| | gradient | 6.8 ms | 2.9 ms |
| | camera-like | 32 ms | 12 ms |
| | every pixel distinct | 35 ms | 11 ms |
| | random, alpha included | 43 ms | 23 ms |
| canvas readback, 1920x1080, `float16` / `float32` storage | camera-like | 64 / 49 ms | 40 / 30 ms |
| exported frame (premultiplied), 640x360 | canvas-like / camera-like | 0.75 / 3.9 ms | 0.28 / 1.3 ms |
| exported frame, 1280x720 | canvas-like / camera-like | 3.0 / 15 ms | 1.1 / 5.4 ms |
| exported frame, 1920x1080 | canvas-like / camera-like | 6.7 / 35 ms | 2.5 / 12.5 ms |
| `readPixels` 1024x1024 | `RGB`/`UNSIGNED_BYTE`, random | 18 ms | 7.3 ms |
| | `RGBA`/`FLOAT`, random in [0, 1) | 15 ms | 13 ms |
| | `RGBA`/`HALF_FLOAT`, random in [0, 1) / smooth render | 57 / 37 ms | 15 / 15 ms |
| `AudioWorklet` render quantum | 128 frames, stereo | 0.73 µs | 0.71 µs |
| 10 s of 44.1 kHz audio | `getChannelData` | 1.3 ms | 1.25 ms |
| `AnalyserNode` byte data | 16384 bins | 165 µs | 47 µs |

Where the time went: whether a pair collapses (one in eight) and onto
which value (one in two) are coin flips, and the old loops branched on
them; those branches were mispredicted so often that they cost more than
the PRF. The loops now compute both outcomes and select (with
`__builtin_unpredictable` where the compiler has it), derive the farbled
channel once per alpha per call (the float pixel paths derived it for
every pixel) and a half's pair context once per sign, and still reuse the
result for runs of one colour. Float32 and audio were already bound by
their two mix rounds (four 64-bit multiplies) per distinct value, and stay
as they were. With bounds-checked spans and hardened libc++, as in a
Chromium build, both versions are up to 50% slower (most on flat content,
hardly at all on distinct pixels) and the speedups are the same. Tried and
dropped: a per-call cache of recent colours (text 20% faster, camera-like
and distinct pixels 15-30% slower: its hit-or-miss branch is as
unpredictable as the one removed), and keeping the key in local registers
(no measurable difference).

On top of the CPU work: exported frames from accelerated canvases need a
synchronous GPU readback and become CPU-backed (`captureStream` loses its
one-copy GPU path while farbling), and every farbled `readPixels` into a
`PIXEL_PACK_BUFFER` costs a synchronous GPU-process round trip per few
MiB (reading the rows back), so the async-readback idiom loses its
asynchrony. Pages that
stream a canvas as video (canvas recording, effects pipelines in video
calls that build frames with `new VideoFrame(canvas)` or `captureStream()`)
would pay the export cost on every frame: at 720p on this VM, camera-like
content costs 5.4 ms of CPU per frame, about a sixth of a core at 30 fps
(half a core before the loops above were optimised), plus the GPU readback,
which this benchmark can't measure. **So the two per-frame hooks
(`VideoFrame` from a canvas, `captureStream`) only run in MoltNet windows**
(`FarblesVideoExports()` in `molt_fingerprinting.h`), and normal windows
keep the one-copy GPU capture path. The cost of that choice: in normal
windows a script can still read unfarbled canvas pixels by wrapping the
canvas in a `VideoFrame` and calling `copyTo()`, or through a
`captureStream()` track processor. Planned fix:
farble at those readback points (`VideoFrame.copyTo`, `MediaStreamTrackProcessor`
frames) instead of on every exported frame, then turn them on everywhere.
WebGPU uploads from a canvas stay farbled in both (they are per upload, not
per frame; camera `<video>`/`VideoFrame` sources are not farbled). Nothing is
copied or computed when protection is off for the context.

Decoded video is not farbled anywhere: its pixels show the clip, not the
machine's rendering. In MoltNet windows they are also decoded and
converted the same way on every machine of an OS, and so are the bytes
the encoders write ([pixels and encoded
bytes](#moltnet-pixels-and-encoded-bytes)).

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

**Automated** by [`tools/leak_test`](LEAK_TESTING.md) on Linux, in a MoltNet window: every limit, range,
the viewport and every precision format against its baseline (equal, or
below it on a GPU that lacks it, never above), the sample counts, the
extension allow-list, no `WEBGL_debug_renderer_info` and the null WebGPU
adapter; `leaktest.sh diff` compares two machines' runs. Still manual: the
report pages, engine samples and the KTX2 sample, and macOS and Windows.

## Letterboxing (MoltNet)

MoltNet windows used to report a rounded screen size while the page itself
kept the window's true content area: `innerWidth`/`innerHeight`, the CSS
`width`/`height` media queries, `vw`/`vh` and layout showed a size that
depends on the window size, the toolbars (bookmarks bar, infobars), the OS,
the DPI and so on, one of the strongest MoltNet fingerprints left. Now the
browser rounds the page's area itself, Tor Browser style: the page's size
in **CSS pixels** is rounded **down** to 200 x 100 steps, capped at
1400 x 900, at whatever zoom level and screen scale factor, and the page
is centred in the tab's content area with margins around. A page's size is
then one of 63 (7 widths x 9 heights), the same for everybody whose window
rounds the same way.

### What a MoltNet page sees

| Value | MoltNet |
|---|---|
| `innerWidth`/`innerHeight`, CSS `width`/`height`/`aspect-ratio` media queries, `vw`/`vh`, `visualViewport.width/height`, layout | the letterboxed size: a multiple of 200 x 100 CSS pixels, at most 1400 x 900, at every zoom level; the media queries, `vw`/`vh` and `visualViewport` report it as exactly that whole number of CSS pixels, strict comparisons such as `(width > 1400px)` included (see [Zoom](#zoom)) |
| classic scrollbars' width (`innerWidth - document.documentElement.clientWidth`, a scrolling box's `offsetWidth - clientWidth`, `scrollbar-gutter`, `vw` with `overflow: scroll`, a `<textarea>`'s width) | **16 CSS pixels** (12 for `scrollbar-width: thin`) at every zoom level and scale factor and on every OS (the nearest whole device pixels where 16 CSS pixels aren't); overlay scrollbars (macOS's default) take none, as upstream (see [Zoom](#zoom)) |
| `screen.width/height/availWidth/availHeight`, `outerWidth/outerHeight` | the same size, in the page's CSS pixels (`screen.*` counts the page's scrollbars, as `innerWidth` does) |
| CSS `device-width`, `device-height`, `device-aspect-ratio` | the same size |
| `MouseEvent`/`PointerEvent`/`Touch` `screenX/Y` | relative to the page, in its CSS pixels: the main frame's `clientX/Y` (as if the window sat at 0,0 with no browser chrome) |
| `devicePixelRatio`, the `resolution` / `-webkit-device-pixel-ratio` media queries, the density `srcset` / `<picture>` / `image-set()` candidates are picked for | **1** (see [Device pixel ratio](#device-pixel-ratio)) |
| page zoom | starts at 100%; the user can zoom, and the page stays a letterbox size (see [Zoom](#zoom)) |

Normal and Incognito windows are unchanged (their goal is per-site
unlinkability; a rounded window would only make users look like other
MoltBrowser users). The kill switch turns all of this off with the rest of
the protection.

**The browser side.** The active tab's page is shown by `ContentsWebView`, a
`views::WebView` that lays the page's native view out in its
`GetContentsBounds()`. In MoltNet profiles `ContentsWebView::GetInsets()`
returns the letterbox margins (`molt_letterbox_view.cc`), computed from the
view's size, the page's zoom and the screen's scale factor, so the page
gets the centred rectangle whose CSS size is a step and is resized only
when that changes (a few times while dragging a window edge, not on every
pixel; when the zoom or the scale factor changes, see [Zoom](#zoom)).
The view's solid-colour layer, under the page, shows in the margins.
Everything else in the content area (DevTools, overlays, dialogs, the status
bubble) still uses the view's full bounds, so none of it moves. The geometry
is constexpr (`molt_letterbox.h`), including a bit-for-bit copy of the
renderer's rounding; the steps and caps are Blink's (`farbling.h`), and
both are duplicated because browser code can't include renderer headers:
`molt_letterbox_unittest.cc` checks the constants, the copy of the
rounding against `farbling.h`'s (which Blink's `letterbox_test.cc` checks
against Blink and gfx themselves), and that Blink's rounding of every
letterboxed size is that size again.

**The renderer side** (`core/molt_fingerprinting/letterbox.{h,cc}`). The
uniform screen is computed from the main frame's size *with* its
scrollbars (before the letterbox: without, so a page with a classic
scrollbar got a screen one step narrower than its `innerWidth`), converted
to the page's CSS pixels with `innerWidth`'s own rounding and zoom
(`AdjustForAbsoluteZoom::AdjustInt` by the main frame's layout zoom, device
scale factor x page zoom; `UniformScreenScale`), then bucketed. On a
letterboxed page `screen.width` and `innerWidth` are therefore the same
number at every zoom level; a page the browser couldn't letterbox (below)
still gets a bucketed screen. `device-width`/`device-height` come from the
same screen (hooked in `MediaValues`, so `matchMedia`, `<source media>` and
the preload scanner agree); Blink already re-evaluates device media queries
when the page's size or zoom changes (`LocalFrame::SetZoomFactors` calls
`MediaQueryAffectingValueChanged`). `MouseEvent` and `Touch` `screenX/Y`
are in the same CSS pixels (`UniformScreenPoint`). The page's float
viewport lengths (media queries, `vw`/`vh`, `visualViewport`) are snapped
to whole CSS pixels where they are within float rounding of them, and
classic scrollbars are 16 CSS pixels thick, so neither shows the zoom
(both in [Zoom](#zoom)).

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

### New windows

A new MoltNet window used to open at the size of the last window (or the
saved one, or maximized like the last one), so its page started with
margins of whatever size that left. Now it opens **restored, with a page
area of exactly the largest letterbox size that fits on its screen**
together with the browser UI around the page: 1400 x 900 wherever there is
room (a 1920 x 1080 screen), 1200 x 600 on a 1366 x 768 laptop. Tor Browser
opens its windows at a rounded size the same way.

* **How.** `MoltNetWindow`, through which every MoltNet window opens (the
  toolbar, Settings, the `.onion` interstitial and Onion-Location bar, New
  Identity), creates the window with a normal show state instead of the
  default one (which copies a maximized last window), then asks for that
  page size: `WindowLetterboxSize()` (`molt_letterbox.h`) takes the
  window's size minus its page area as the size of the browser UI (frame,
  tab strip, toolbar, side panel) and returns the largest letterbox size
  that fits in the screen's work area with it, at the screen's scale
  factor and 100% zoom (on a 1.2 screen 1200 wide, see [Zoom](#zoom)), and
  `BrowserWindow::
  SetContentsSize()` grows or shrinks the window by the difference and
  moves it back onto the work area if needed. The resize happens before the
  window is shown when it has been laid out by then, else as soon as it has
  (at most 10 posted retries).
* **Left alone:** a window that is maximized, minimized or fullscreen
  anyway (`--start-maximized`, the window manager), a screen too small for
  one step, and windows that don't come from `MoltNetWindow`: popups a page
  opens with a size and tabs dragged out of a MoltNet window (they keep
  their size and get margins). (Ctrl+N in a MoltNet window opens a regular
  window.) Resizing or maximizing the window later is up to the user; the
  letterbox takes over from there.
* The kill switch turns it off with the letterbox (the window then opens as
  upstream does).

### Zoom

**MoltNet resets zoom, doesn't lock it, and letterboxes the zoomed page.**

* MoltNet's profile used to copy, and keep following, the regular profile's
  default zoom and per-site zoom levels (Chromium does that for every
  off-the-record profile). A site zoomed in a regular window opened zoomed
  in MoltNet, which both broke the letterbox and linked the two profiles.
  Now MoltNet pages always start at 100% (`OffTheRecordProfileImpl::Init`
  skips `TrackZoomLevelsFromParent` for MoltNet).
* The user can still zoom (an accessibility need; Tor Browser doesn't lock
  zoom either). A zoom chosen in a MoltNet window applies to that site for
  the MoltNet session and is forgotten at New Identity.
* A zoomed page is letterboxed in CSS pixels, so its size is a step at
  every zoom level and the zoom no longer shows in it (below).

**What zoom changes at 152.** Page zoom multiplies Blink's layout zoom
(`WebFrameWidgetImpl::SetZoomInternal`: device scale factor x zoom factor),
and everything in CSS pixels is the page's device pixels divided by it. With
the letterbox in DIPs, a page at 125% in a 1400 x 900 DIP letterbox was
1120 x 720 CSS pixels:

| Value | Chromium at 152 | MoltNet, letterbox in DIPs | MoltNet now |
|---|---|---|---|
| `innerWidth`/`innerHeight` (and `outerWidth`/`outerHeight`, which are the same in MoltNet), `width`/`height` media queries, `vw`/`vh`, `visualViewport.width/height` | DIPs / zoom | 1120 x 720: the zoom shows | a step: e.g. 1200 x 800 in a 1500 x 1000 DIP area; the float lengths exactly that step (below) |
| `screen.*`, `device-width`/`device-height` | the display in DIPs, unzoomed | the page in DIPs, 1400 x 900: `screen.width / innerWidth` was the zoom | the page in CSS pixels, = `innerWidth`/`innerHeight` |
| `MouseEvent`/`Touch` `screenX/Y` | the point on the display, in DIPs | relative to the page, in DIPs: `screenX / clientX` was the zoom | in CSS pixels, = the main frame's `clientX/Y` |
| `devicePixelRatio`, `resolution` media queries, `srcset`/`image-set()` | device scale factor x zoom | 1 | 1 |
| `visualViewport.scale` | pinch zoom only | 1 | 1 |
| `DPR`, `Viewport-Width` and the other client hints | the zoomed values | not sent ([Client hints](#client-hints)) | not sent |
| classic scrollbars' width in CSS pixels (`innerWidth - documentElement.clientWidth`) | thickness / zoom (scrollbars follow the scale factor, not the zoom: `ScrollableArea::ScaleFromDIP`) | the zoom shows | 16 at every zoom and on every OS where that is whole device pixels; the nearest whole device pixels elsewhere (below) |

**The page's CSS size is letterboxed.** The browser picks the page's DIP
size so that, at the page's zoom and the screen's scale factor, its CSS
size is a step. What the renderer makes of a DIP size is float arithmetic
in another process: content sends the size in device pixels
(`ScaleToCeiledSize`: DIPs x scale factor, rounded up) and Blink divides by
the layout zoom and rounds (`AdjustForAbsoluteZoom::AdjustInt`, which
`innerWidth` uses: + 0.5 device pixel when the layout zoom is above 1, then
+ 0.01, truncated). The browser can't ask, so `molt_letterbox.h` repeats
that arithmetic bit for bit
(Chromium builds with `-ffp-contract=off`, so both processes round alike)
and, from the largest step that fits down, looks for the DIP lengths whose
CSS length is exactly that step (a binary search: the CSS length never
shrinks as the DIP length grows); of those it takes the one whose
media-query length (`ViewportSizeForMediaQueries`: device pixels x 1/zoom,
in float) is closest to the step. Blink reports the page's CSS size,
bucketed, as the screen, so `screen.width == innerWidth` at every zoom
level, and `devicePixelRatio` stays 1. On a 1x screen with a 1904 x 960 DIP
content area (a maximized 1080p window):

| Zoom | 25% | 33% | 50% | 67% | 75% | 80% | 90% | 100% | 110% | 125% | 150% | 175% | 200% | 250% | 300% | 400% | 500% |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Page in DIPs | 350 x 225 | 400 x 300 | 700 x 450 | 800 x 600 | 1050 x 675 | 1120 x 720 | 1260 x 810 | 1400 x 900 | 1540 x 880 | 1750 x 875 | 1800 x 900 | 1750 x 875 | 1600 x 800 | 1500 x 750 | 1800 x 900 | 1600 x 800 | 1000 x 500 |
| CSS size | 1400 x 900 | 1200 x 900 | 1400 x 900 | 1200 x 900 | 1400 x 900 | 1400 x 900 | 1400 x 900 | 1400 x 900 | 1400 x 800 | 1400 x 700 | 1200 x 600 | 1000 x 500 | 800 x 400 | 600 x 300 | 600 x 300 | 400 x 200 | 200 x 100 |

**Steps that can't be hit.** Not every step comes out exactly at every zoom
and scale factor; the page then takes the next smaller one.

* At 33% and 67% a DIP is 3 (or 1.5) CSS pixels, so only every third 100
  can be hit: a page is at most 1200 wide (900 high is fine).
* The renderer's float rounding skips some lengths. At a scale factor of
  1.2, 1400 DIPs are 1681 device pixels (1.2 in float is a little more
  than 1.2, so 1400 x 1.2 comes out as 1680.0001, rounded up) and 1401 CSS
  pixels, so a page on a 1.2 screen is 1200 wide at 100% (the letterbox in
  DIPs gave it an `innerWidth` of 1401, a listed gap, now closed).
* Where device pixels don't divide evenly (a 4/3 screen at 100%, 1.25 at
  75%, 1.5 at 175%, ...), a step can come out exact for `innerWidth` but up
  to a device pixel off for media queries and `vw` (500.19 instead of 500),
  which `(max-height: 500px)` and a rounded `getBoundingClientRect()` see.
  The browser then prefers the next smaller step that comes out exact for
  media queries too, within their own tolerance (1/64 pixel,
  `LayoutUnit::Epsilon()`); only when none does, the largest it can hit.

Over every preset zoom level, 14 scale factors (1, 1.1, 1.15, 1.2, 1.25,
1.3, 4/3, 1.4, 1.5, 1.75, 2, 2.25, 2.5, 3) and every content area up to
3000 x 2000 DIPs: 90% of the areas are letterboxed (the rest are less than
a step at high zoom); 96.5% of the letterboxed pages have a media-query size
within 1/64 pixel of the step, 99.7% on 1x and 2x screens (the rest: small
windows at 67%, where the only steps that fit are half a pixel off), and
those now see exactly the step (*Viewport lengths*, below); and 11%
are a step (on average 150 CSS pixels) smaller than the largest they could
be, to stay exact.

**When the page is laid out again.** The margins depend on the page's zoom
and the screen's scale factor, taken as content sends them to the renderer
with the page's size: the RenderWidgetHostView's scale factor, and the zoom
level `WebContentsImpl::GetPendingZoomLevel()` picks (the pending
navigation's host zoom while there is one, the page's own otherwise),
clamped as the renderer clamps it. `ContentsWebView` lays the page out again
when its renderer has just been sent new visual properties
(`PrimaryMainFrameWasResized`: a zoom changed through `HostZoomMap`, a
temporary or default zoom, a move to a screen with another scale factor)
and when a new page has committed (`PrimaryPageChanged`), and only if the
margins change. Not on the zoom controller's notifications: `HostZoomMap`
sends the renderer its new zoom *before* it tells its observers, a
navigation to a host with another zoom is no zoom change at all, and a new
scale factor isn't zoom; every one of them reaches the renderer as new
visual properties. Laying out at once (as `WebView` does on a fullscreen
change) sends the new size in the same task, right behind the zoom.

**Background tabs.** A tab that isn't shown isn't in a `ContentsWebView`.
It keeps the size it was last shown at, or, opened in the background, the
active tab's (`TabStripModel` sizes it so), and both were letterboxed for
another page's zoom: a background tab opened from a page at 125% loading a
site at 100%, a site zoomed in another tab, a background page navigating to
a site zoomed differently. `BackgroundTabLetterbox`, attached to every
MoltNet `WebContents` (`AttachUniversalWebContentsObservers`), letterboxes
such a page again within its size on the same two notifications; once the
tab is shown, `ContentsWebView` letterboxes it in the whole content area.

**Viewport lengths: whole CSS pixels.** `innerWidth`/`innerHeight` are
integers (`AdjustInt` rounds), but the `width`/`height` media queries,
`vw`/`vh` (and the media queries' own `vw`) and `visualViewport.width/height`
are floats: the frame's device pixels over its layout zoom, itself a float.
Where the letterbox comes out exact they are the step plus float rounding,
a float step or two (0.00012 each at 1400), and that rounding differs between
them and with the zoom and scale factor, which strict comparisons see. Two
sources, both in Blink at 152:

* the layout zoom is a float product (`0.9f` is 0.89999998, and 1.5 x 0.9
  comes out as 1.3499999), so device pixels over it miss whole numbers by a
  float step: 270 device pixels at 90% on a 1.5 screen are 200.0000153 CSS
  pixels for `vw` and `visualViewport` too;
* the media queries multiply by the float reciprocal of the zoom
  (`LocalFrameView::ViewportSizeForMediaQueries`, device pixels x (1 /
  zoom), two roundings) where `vw`/`vh` (`SmallViewportSizeForViewportUnits`,
  `InvScale`) and `visualViewport` (`VisibleWidthCSSPx`) divide (one): at 90%
  on a 1x screen a 1400 x 900 page is 1260 x 810 device pixels, 1260 / 0.9f
  is exactly 1400 for `vw` and 1260 x (1 / 0.9f) is 1400.0001 for the media
  queries, so `(width > 1400px)` and `(width > 100vw)` matched there and
  nowhere else.

Over every preset zoom, 14 scale factors and every content area up to 3000 x
2000 DIPs, the media-query length used to be exactly the step on 69.4% of
the letterboxed pages and the `vw`/`visualViewport` length on 81.6%, while
96.5% were within 1/64 CSS pixel of it (within float rounding, in fact).
Fixing the reciprocal alone would not do (the zoom's own rounding stays), so
MoltNet reports any viewport length within 1/64 CSS pixel of a whole number
as that whole number (`SnapViewportLength`, farbling.h; hooked into
`ViewportSizeForMediaQueries`, `SmallViewportSizeForViewportUnits`, which
the large and dynamic units and the media queries' `vw` build on, and the
main frame's `visualViewport.width/height`; subframes' `visualViewport`
sizes are rounded to integers upstream already). 1/64 is Blink's own
tolerance for the media queries' `=`, `<=` and `>=` (`LayoutUnit::Epsilon()`)
and the precision of layout, so nothing a page lays out moves. On those
96.5% the media queries, `vw`/`vh` and `visualViewport` now report exactly
`innerWidth` x `innerHeight` and agree with each other, strict comparisons
included. The other 3.5% are the browser's fallback (it prefers a smaller
step whose length comes out exact, and none fits: pages a few hundred CSS
pixels across at high zoom, small windows at 67%, 4/3 screens); their
lengths stay up to 0.91 CSS pixel off the step, as before. Every frame gets
it (an iframe's viewport rounds the same way), MoltNet web content only.

**Classic scrollbars: 16 CSS pixels.** Upstream a classic scrollbar
(Windows, Linux, macOS with scroll bars always shown) is a fixed number of
DIPs at every page zoom: the theme's thickness (15 on Linux and in Windows'
Fluent theme, 17 in classic Windows, 15 for macOS's legacy scrollbars)
times the device scale factor (`ScrollableArea::ScaleFromDIP`), so its
width in CSS pixels, `innerWidth - document.documentElement.clientWidth` or
a scrolling box's `offsetWidth - clientWidth`, is the thickness over the
zoom (15 at 100% on Linux, 12 at 125%, 7 or 8 at 200%), and it tells the OS
apart too. Firefox and Tor Browser are no different: their scrollbars don't
follow full zoom either (`GetDPIRatioForScrollbarPart` scales by the
unzoomed device ratio), their sizes differ per OS (17 CSS pixels on
Windows, 15 for classic macOS, the GTK theme's on Linux), and Tor Browser
only stopped Windows from reporting the system's own scrollbar size
(tor-browser#42528).

Two ways to make the width the same at every zoom: scale the theme's
scrollbar with the zoom (a hook in `ScaleFromDIP`), or pin the thickness in
CSS pixels. Scaling keeps each OS's thickness and the theme's own rounding
(Aura truncates, Fluent rounds), so the OS still shows and the CSS width
still wobbles with the zoom by a device pixel. MoltNet pins it: a classic
scrollbar is `kUniformScrollbarThickness`, **16 CSS pixels** of the document
it scrolls (12 for `scrollbar-width: thin`, 0 for `none`), on every OS, at
every zoom level and scale factor: the whole number of device pixels
nearest to 16 x the frame's layout zoom (`UniformScrollbarDevicePixels`,
farbling.h). 16 sits between the themes' 15 and 17 and is a whole number of
device pixels far more often than either: on 1, 1.25, 1.5, 1.75, 2, 2.5 and
3x screens at 100%, on a 1x screen at every preset zoom but 33, 67, 80, 90
and 110%, and at 126 of the 238 preset zoom x scale factor pairs (15: 82,
17: 36). It is pinned where Blink asks the theme for a thickness: the
scrollbar's own (`Scrollbar::ScrollbarThickness`), the room
`scrollbar-gutter: stable` keeps (`ComputeHypotheticalScrollbarThickness`),
the scrollbar a `<textarea>`'s width counts (`layout_text_control.cc`) and
the one `vw`/`vh` leave out of a root that always has one (`overflow:
scroll`; `ComputeScrollbarWidthsForViewportUnits`, which upstream takes the
theme's DIPs as CSS pixels: 16 there); all through `ScrollbarThicknessFor`
in letterbox.h. The rest of a MoltNet scrollbar (arrows, thumb, insets) is
drawn at the page zoom too (`ScaleFromDIP` returns the layout zoom for it),
so it keeps its proportions: it looks like a 16-pixel part of the page,
larger at 200%, thinner at 50%. Overlay scrollbars (macOS's default,
Android), which take no room, and custom `::-webkit-scrollbar` ones, which
the page sizes itself, are left alone; so are regular and Incognito windows
and internal pages. (Shields off doesn't lift MoltNet's uniformity, see
[Shields off](#shields-off).)

Over the same sweep, `innerWidth - documentElement.clientWidth` is exactly
16 on 88.6% of the letterboxed pages; at the 126 pairs where 16 CSS pixels
are whole device pixels, every page the browser letterboxes exactly sees
exactly 16 there and `innerWidth` - 16 as `visualViewport.width`.
Elsewhere the scrollbar is the nearest whole
number of device pixels: 90% on a 1x screen gives 14 device pixels, 15.56
CSS pixels, which `clientWidth` and a scrolling box still round to 16, but
`visualViewport.width` with a scrollbar (1384.44 next to an `innerWidth` of
1400) and `getBoundingClientRect()` show.

**What is left.**

* *The moment of a change* (reviewed again, still open). Content sends the
  renderer the new zoom (or scale factor) in one visual-properties update
  and, once the browser has laid the page out again, the new size in the
  next, from the same browser task (`ContentsWebView` lays out as soon as
  it sees the first, `PrimaryMainFrameWasResized`). A script that runs
  between the two messages (a timer; not layout and `resize` events, which
  wait for a frame) sees the old size at the new zoom. No contained fix:
  `HostZoomMap` has content send the zoom (`WebContentsImpl::UpdateZoom`,
  `RenderWidgetHostImpl::SynchronizeVisualProperties`) before it tells any
  observer, so merging the two into one update needs a new content hook
  that lets the letterbox resize the page first; deferring the zoom in the
  renderer until the size arrives would leave pages the browser doesn't
  resize (smaller than a step) unzoomed. When the window moves to a screen
  with another scale factor, the renderer draws a frame at the new scale
  before it takes the next size, so one frame can show the size in
  between.
* *Scrollbars where 16 CSS pixels aren't whole device pixels* (33, 67, 80,
  90 and 110% on 1x and 2x screens; most zoom levels on 1.1, 1.15, 1.2, 1.3,
  4/3 and 1.4 screens): the scrollbar is the nearest whole number of device
  pixels, so its sub-pixel width shows the zoom (above), and the integer
  measures can read 15 or 17 (at 33% on a 1x screen, 15).
* *Overlay or classic.* Since Chromium 156 MoltNet pages get their
  platform's default style whatever the OS setting says (overlay on macOS,
  classic on Windows and Linux; [MoltNet's
  display](#moltnets-display-colour-and-scrollbar-style)), so whether
  scrollbars take room (0 or 16) only tells macOS from the other systems,
  which the user agent says anyway.
* *Sub-pixel values* still carry the layout zoom, as they carry the scale
  factor (see [Device pixel ratio](#device-pixel-ratio)): layout geometry
  in device pixels (`getBoundingClientRect()`, `ResizeObserver` and
  `IntersectionObserver` sizes), `devicePixelContentBoxSize`, text metrics,
  and the viewport lengths of the 3.5% of pages the browser can't
  letterbox exactly. The media queries, `vw`/`vh` and `visualViewport` no
  longer do (above).
* DevTools' device emulation overrides the page's size, as before.

**What it costs MoltNet users.** Zooming out doesn't show more of a page
once it is at 1400 x 900 CSS pixels: the page gets smaller instead (350 x
225 DIPs at 25% on a 1080p screen). Zooming in drops it to smaller steps,
like a smaller window. At some zoom levels a page is a step smaller than
the window allows (above). Classic scrollbars are a DIP wider than Linux's
and Fluent's at 100% and a DIP narrower than classic Windows', and they
zoom with the page: 48 DIPs thick at 300%, 4 at 25% (harder to grab;
the mouse wheel and keyboard still scroll).

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
  measured), `ResizeObserver`'s `devicePixelContentBoxSize` and the
  favicon size the browser fetches. As with the WebGL baselines, this stops
  scripts that read the value, not ones written against MoltBrowser. The
  DPR client hints (`Sec-CH-DPR`, `DPR`, and `Sec-CH-Width`, which is
  multiplied by the ratio) are no longer sent in MoltNet ([client
  hints](#client-hints)).

### Known gaps (letterboxing)

* A content area smaller than one step in CSS pixels (narrower than 200 or
  shorter than 100 CSS pixels: at 100% on a 1x screen, DIPs; at 200%, 400 x
  200 DIPs) is not letterboxed (there is no smaller step); the screen still
  reports one step.
* Cross-process iframes only know the main frame's size *without* its
  scrollbars, so with classic (Windows, Linux) scrollbars a scrolling main
  frame's cross-site iframes can report a screen one step narrower than the
  main frame's. Device media queries in an iframe whose own size doesn't
  change are re-evaluated at its next media-value change.
* (Closed: at a device scale factor of 1.2, Chromium's float rounding gave
  the page one device pixel more than the browser laid out, so `innerWidth`
  was 1401 for 1400. The browser now predicts that rounding and gives such
  a page the next step, 1200, see [Zoom](#zoom).)
* Zoom: where 16 CSS pixels aren't whole device pixels (33, 67, 80, 90 and
  110% on 1x and 2x screens, most zooms on less common scale factors) a
  classic scrollbar's sub-pixel width shows it; layout's sub-pixel values
  carry it; and a script can catch the moment between a new zoom and the
  new size ([Zoom](#zoom), "What is left"). At some zoom levels and scale
  factors a page is a step smaller than its window allows, and on the
  3.5% of pages the browser can't letterbox exactly the media queries and
  `vw` are up to a device pixel off the step.
* Scrollbars: whether they take room at all (overlay or classic) follows
  the platform's default since 156, so it only shows the OS
  ([Zoom](#zoom)).
* A maximised window on a large monitor shows the page at 1400 x 900 with
  wide margins: the cap is what makes everyone with a large window
  identical. New MoltNet windows open without margins ([New
  windows](#new-windows)); maximizing one brings them back.
* During the side panel's flyover animation the page's clip rectangle is
  computed for the view without its margins, so the page can be clipped a
  margin's width off for the length of the animation (cosmetic).

### Code

| Where | What |
|---|---|
| `chrome/browser/molt_ai/fingerprinting/molt_letterbox.h` | the geometry in CSS pixels at any zoom and scale factor, constexpr and std-only, with the copy of the renderer's rounding (`PageDevicePixels`, `PageCssPixels`, `PageMediaQueryPixels`), including the page size new windows open at (`WindowLetterboxSize`) (+ `molt_letterbox_unittest.cc` in `molt_ai_unittests`, against Blink's constants and `farbling.h`'s copy of the rounding) |
| `chrome/browser/molt_ai/fingerprinting/molt_letterbox_view.{h,cc}` | which tabs, the page's zoom and scale factor as content sends them, the insets, when to lay out again, the margin colour, background tabs (`BackgroundTabLetterbox`) (desktop only, `:letterbox_view`) |
| `chrome/browser/molt_ai/tor/molt_net_window.cc` | new windows: restored, resized to that page size at the screen's scale factor (`FitPageToLetterbox`) |
| `core/molt_fingerprinting/letterbox.{h,cc}` | the screen size with scrollbars in the page's CSS pixels (`UniformScreenScale`), the device pixel ratio, the scrollbar thickness (`ScrollbarThicknessFor`, `ScrollbarThicknessForViewportUnits`, `ScrollbarScaleFromDIP`) and the whole viewport lengths (`UniformViewportSize`, `UniformViewportLength`) for MoltNet web content (+ `letterbox_test.cc`) |
| `core/molt_fingerprinting/farbling.{h,cc}` | the steps and caps (`LetterboxBucket`), the renderer's rounding as the browser predicts it (`PageViewportPixels` too), checked against Blink in `letterbox_test.cc`; the uniform scrollbar thickness and its device pixels (`UniformScrollbarDevicePixels`), the snapping (`SnapViewportLength`) |

Upstream hooks (each marked `MoltBrowser:`): browser
`chrome/browser/ui/views/frame/contents_web_view.{h,cc}` (`GetInsets`,
`UpdateBackgroundColor`, `DidToggleFullscreenModeForTab`,
`PrimaryMainFrameWasResized`, `PrimaryPageChanged`),
`chrome/browser/universal_web_contents_observers.cc`
(`AttachUniversalWebContentsObservers`: background tabs) and
`chrome/browser/profiles/off_the_record_profile_impl.cc` (`Init`); Blink
`css/media_values.cc` (`CalculateDeviceWidth`, `CalculateDeviceHeight`,
`CalculateDevicePixelRatio`), `frame/local_dom_window.cc`
(`devicePixelRatio`), `html/html_image_element.cc`
(`FindBestFitImageFromPictureParent`, `SelectSourceURL`) and
`css/resolver/style_resolver_state.cc` (the constructor's image density),
and for scrollbars and viewport lengths `scroll/scrollbar.cc`
(`Scrollbar::ScrollbarThickness`), `scroll/scrollable_area.cc`
(`ScaleFromDIP`), `paint/paint_layer_scrollable_area.cc`
(`ComputeHypotheticalScrollbarThickness`,
`ComputeScrollbarWidthsForViewportUnits`),
`layout/forms/layout_text_control.cc` (`ScrollbarThickness`),
`frame/local_frame_view.cc` (`ViewportSizeForMediaQueries`,
`SmallViewportSizeForViewportUnits`) and `frame/dom_visual_viewport.cc`
(`width`, `height`).
`UniformScreenRect()` (`molt_fingerprinting.cc`) takes its size from
`letterbox.cc`, and `UniformScreenPoint()` its scale.

**Verified without a Chromium checkout:** `molt_letterbox_unittest.cc`
(4 tests and static_asserts) passes standalone against the real
`farbling.cc` with g++ (ASan/UBSan) and clang (UBSan trap,
`-Wunsafe-buffer-usage -Wshorten-64-to-32 -fno-exceptions`);
`letterbox.cc` and `molt_fingerprinting.cc` type-check with the strict
flags against stand-ins of the pinned Blink signatures, and a harness runs
the real glue in real kUniform mode: for 13 device scale factors and every
content area up to 4000 DIPs, the renderer's size (`ceil(DIP x scale)`, as
content computes it) gives a screen equal to `innerWidth`/`innerHeight` and
to the browser's letterbox size (with the 1.2 drift, since closed, see
[Zoom](#zoom)); cross-process
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

**Automated** by [`tools/leak_test`](LEAK_TESTING.md) on Linux: the page and screen size,
`outer*`, `screenX/Y`, `availLeft/Top`, `devicePixelRatio`, the
`resolution` and device-size media queries, the orientation and mouse
`screenX/Y`, at five window sizes and at 1x and 2x
(`--force-device-scale-factor`). Still manual: DevTools docked, the side
panel, infobars, split view, fullscreen, light and dark mode, `srcset` and
`image-set()` selection, a regular-profile zoom, macOS and Windows.

**New windows, verified without a Chromium checkout:** the two new
`molt_letterbox_unittest.cc` tests and seven static_asserts pass
standalone (g++ ASan/UBSan, clang UBSan trap with the strict flags): every
combination of browser-UI size (0 to 320 x 141 DIPs) and work area up to
3000 x 2000 gives a page with no margins, a window that fits, and the
largest size that fits; the result doesn't depend on the previous window
size. `FitPageToLetterbox()`, extracted verbatim from `molt_net_window.cc`,
compiles against stand-ins of the pinned `BrowserWindow`, `ui::BaseWindow`
and `display::Screen` signatures and runs in a harness (1400 x 900 on a
1080p screen, 1200 x 600 on 1366 x 768, maximized, minimized and
fullscreen windows left alone, waiting for layout and giving up after 10
tries, a window closed meanwhile, no screen). **Needs the build machine:**
opening MoltNet windows on Windows, macOS and Linux (X11 and Wayland) at 1x
and 2x, after a maximized regular window and with the bookmarks bar shown:
the page must start at 1400 x 900 (or the largest step) with no margins and
without a visible resize.

[`tools/leak_test`](LEAK_TESTING.md) checks that the first MoltNet window on its 1920 x 1080 Xvfb screen
opens with a 1400 x 900 page (`uniform.new_window_size`); the other
platforms and a maximized regular window first stay manual.

**Zoom, verified without a Chromium checkout** (scratch harnesses):

* `molt_letterbox_unittest.cc` (11 tests, 30 static_asserts) passes
  standalone against the real `farbling.cc` and the pinned `page_zoom.cc`,
  with g++ (ASan/UBSan) and clang (UBSan trap; the header and
  `farbling.cc` also with `-Wimplicit-float-conversion -Wfloat-conversion
  -Wunsafe-buffer-usage -Wshorten-64-to-32 -fno-exceptions`): the browser's
  copy of the renderer's rounding equals `farbling.h`'s for every scale
  factor, preset zoom and size up to 16000 device pixels; for every preset
  zoom, 14 scale factors and every content area up to 3000 x 2000 DIPs
  (1.19 million cases), against an oracle that computes every DIP length's
  CSS length in one pass instead of searching, the page's CSS size is the
  largest step that can be hit exactly (preferring exact media queries),
  Blink reports it as the screen, the media-query length is the best one
  and within a device pixel, and letterboxing again changes nothing; at
  100% on 1, 1.25, 1.5, 1.75, 2, 2.5 and 3x screens the result is the DIP
  letterbox of before; new windows open without margins on every screen.
* `letterbox.cc`, `molt_fingerprinting.cc` and `moltnet_uniformity.cc`
  type-check with the strict clang flags against stand-ins of the pinned
  Blink signatures (`LocalFrame::LayoutZoomFactor`/`CssZoomFactor`
  included), and a harness runs the real glue in real kUniform mode: for
  every preset zoom, 14 scale factors and content areas up to 3000 DIPs,
  the renderer's size and layout zoom (as content and `SetZoomInternal`
  compute them) give an `innerWidth`/`innerHeight` (`AdjustInt`, verbatim)
  equal to the browser's prediction and a screen equal to them; mouse and
  touch `screenX/Y` in CSS pixels; a cross-process frame in a CSS-zoomed
  `<iframe>`, a same-process child with its own zoom, a zero or NaN zoom.
  The MoltNet uniformity harness of before passes again.
* `molt_letterbox_view.cc` type-checks with the strict flags against
  stand-ins of the pinned content/views/tabs signatures and runs, with
  `ContentsWebView`'s hooks as written and the pinned `page_zoom.cc`, under
  g++ ASan/UBSan and clang UBSan: insets at 125% and at 50% on 2x, the
  pending navigation's zoom, the renderer's clamp, the window's scale before
  there is a renderer, a new zoom, a new page and a new screen laying the
  page out once and only then, background tabs letterboxed within their
  size (not shown tabs, not other contents, not with the kill switch off).
* 21 mutants (the renderer's + 0.5 and + 0.01, its ceiling, the layout zoom
  without the scale factor, the media-query length by division, the first
  DIP length instead of the best, no media-query preference, the largest
  step not tried first, the screen and pointer back in DIPs, the iframe's
  CSS zoom kept, the frame's own zoom, the pending zoom ignored, no clamp,
  the window's scale over the renderer's, layout on every notification,
  background tabs when shown or not resized, ...) each fail one of them.

**Needs the build machine:** compiling the hooks and `blink_unittests`'
new `MoltLetterboxTest.*` cases (`ZoomedPagesReportTheirCssSizeAsTheScreen`,
`TheScreenFollowsThePageZoom`, `PredictedArithmeticIsBlinks`: the copy of
the rounding against `gfx::ScaleToCeiledSize`, `AdjustInt` and a real
frame's `innerWidth` and media-query width), and in a MoltNet window at 1x,
2x and a fractional scale (1.25 on Windows, 1.5 on GNOME), on Windows,
macOS and Linux: every zoom level from the menu, with a page printing
`innerWidth`, `screen.width`, `matchMedia('(width: ...)')` and
`devicePixelRatio` on `resize`, and polling them on a timer while zooming
(the moment between the two messages); a link opened in a background tab
from a zoomed page; a site zoomed in another tab; navigating between sites
at different zoom levels (cross-site and same-site); the PDF viewer
(manual zoom); moving the window between screens of different scale
factors.

[`tools/leak_test`](LEAK_TESTING.md) tries every listed zoom level through DevTools key events
(Ctrl+= / Ctrl+-) and checks the letterbox rules at each; when the browser
doesn't take the keys from DevTools, the level is reported as SKIP and stays
manual, like the rest of this list.

**Scrollbars and viewport lengths, verified without a Chromium checkout**
(scratch harnesses):

* `farbling_unittest.cc` (5 new tests) passes standalone with g++
  (ASan/UBSan) and clang (UBSan trap; `farbling.cc` also with
  `-Wimplicit-float-conversion -Wfloat-conversion -Wunsafe-buffer-usage
  -Wshorten-64-to-32 -fno-exceptions`): the thickness against an oracle
  that tries every whole number of device pixels, at every preset zoom on
  14 screens, and over zooms from 0.05 to 20 (never thinner at a larger
  zoom, within half a device pixel); 16 and 12 CSS pixels whole on the
  usual screens, 16 whole more often than 15 or 17; zero, negative, NaN,
  infinite and out-of-range inputs; the snapping (inside and outside 1/64,
  NaN, infinities, idempotent); where the media queries' 1400.0001 comes
  from, and the media queries and `vw` snapping alike for every length up
  to 8000 device pixels at every zoom.
* `molt_letterbox_unittest.cc` (`PagesSeeWholeViewportLengthsAndScrollbars`,
  and the 11 tests of before unchanged): over every preset zoom, 14 scale
  factors and every content area up to 3000 x 2000 DIPs (1.07 million
  letterboxed pages), every page the browser letterboxes exactly (96.5%)
  sees its step as its media-query, `vw` and `visualViewport` length, the
  rest are left as they were; 16 CSS pixels are whole device pixels at 126
  of the 238 zoom x scale pairs, and there every exactly letterboxed page
  sees `innerWidth - clientWidth` = 16 and `visualViewport.width` =
  `innerWidth` - 16 (both exact on 51.3% of all pages).
* `letterbox.cc`, `molt_fingerprinting.cc` and `moltnet_uniformity.cc`
  type-check with the strict clang flags against stand-ins of the 152
  signatures (now with `Document`, `ScrollableArea`, `ScrollbarTheme` and
  `EScrollbarWidth`); the hook statements of the seven upstream files
  type-check against stand-ins of their classes with the 152 member types
  (`ScrollbarTheme& theme_`, `Member<ScrollableArea>`, ...); and a harness
  runs the real glue in real kUniform, kFarble and kOff modes: the
  thickness for https, http, opaque, internal (`chrome://`, extensions,
  `file://`) and child-of-internal frames, classic and overlay themes, no
  document or no frame, every preset zoom on 14 screens (the theme not
  even asked in MoltNet); the viewport units' thickness in CSS pixels; the
  scale from DIPs (a NaN or zero zoom left alone); snapping only in MoltNet
  web content; and end to end with the browser's letterbox and `AdjustInt`
  (verbatim): exact lengths on 96.6% of the pages, and `clientWidth` and
  `visualViewport` exact on every exact page at a whole zoom. PR #27's zoom
  and MoltNet-uniformity harnesses pass again.
* 25 mutants (the thickness floored, ceiled, without its one-pixel minimum
  or zoom guard, 15 CSS pixels, thin as 16; no snapping, snapping
  everything, a 1/16 tolerance, snapping down, `vw` by the reciprocal;
  overlay scrollbars pinned too, every page, internal pages too, the scale
  factor instead of the frame's zoom, thin or none as auto, the viewport
  units' thickness in device pixels, the scale from DIPs unchanged, for
  overlay scrollbars too or with a NaN zoom; only the width snapped,
  snapping outside MoltNet, `visualViewport` unsnapped, every window
  uniform) each fail one of them.

**Needs the build machine:** compiling the seven hooks;
`blink_unittests`' new `MoltLetterboxTest.*` cases
(`ClassicScrollbarsAreUniformInMoltNet`,
`ScrollbarsAreTheNearestDevicePixels`,
`ScrollbarsOutsideMoltNetAreTheThemes`,
`ScrollbarsAreDrawnAtThePageZoomInMoltNet`,
`OverlayScrollbarsAndInternalPagesAreTheThemes`,
`ViewportLengthsAreWholeInMoltNet`, `OnlyNearlyWholeWebViewportLengthsSnap`,
and `PredictedArithmeticIsBlinks` now checking `vw` and `visualViewport`
too); the farbling and letterbox tests in `molt_ai_unittests`. By hand, in
a MoltNet window, what the kit can't do: on Windows 10 and 11 and Linux,
at 1x and a fractional scale, a long
page's `innerWidth - document.documentElement.clientWidth` is 16 at 100%
and through every zoom level (at 33, 67, 80, 90 and 110% on 1x and 2x
screens it may read 15 or 17), 12 with `scrollbar-width: thin`, still 16
with the overlay-scrollbars flag on, and 0 on macOS (overlay whatever "Show
scroll bars" says, since 156); the scrollbar looks proportionate at
25% and 500% (arrows, thumb, the corner), and dragging and clicking it
still work; a `<textarea>`'s width, `scrollbar-gutter: stable` and `100vw`
under `html { overflow: scroll }` leave out 16 CSS pixels; a regular window
keeps its OS's scrollbars; `matchMedia('(width > ' + innerWidth + 'px)')`
is false at every zoom on 1x, 1.25, 1.5 and 2x screens, and
`visualViewport.width` is `innerWidth` on a page without a scrollbar;
pinch zoom on a touchpad still reports fractional `visualViewport` sizes.

[`tools/leak_test`](LEAK_TESTING.md) checks `scrollbar.box` (a scrolling
box's `offsetWidth - clientWidth`: 16, or 0 with overlay scrollbars) and
`mq.viewport_exact` (the `width`/`height` media queries and `vw`/`vh` are
exactly `innerWidth` x `innerHeight`, strict comparisons included) at every
window size, zoom level and scale factor it tries, on Linux with classic
scrollbars. Its default zoom levels (50, 90, 125 and 200%) all read 16;
asked for 33% or 67%, the box can read 15 or 17 by design (above).

## MoltNet: no touchscreen, client hints, memory and CPU

With the screen letterboxed and the GPU uniform, MoltNet users could still
be told apart by whether they have a touchscreen, by the client hints a
server can ask for (the real pixel ratio, memory, network quality, exact OS
version, CPU architecture and browser build), by `navigator.deviceMemory`,
by `navigator.cpuPerformance` (new at 152 and on by default: a CPU class
derived from the CPU model) and by the high-entropy values of
`navigator.userAgentData`. In MoltNet windows all of these are now the same
for everyone:

| API | Before, in MoltNet | Now | Where |
|---|---|---|---|
| `navigator.maxTouchPoints` | 0, or the touchscreen's contact count (often 10) | **0** | browser, `OverrideWebPreferences` |
| `pointer` / `any-pointer` media queries | `fine`; `coarse` in tablet mode; any-pointer `coarse` and `fine` on a touchscreen laptop | **`fine`** / **`fine`** | same |
| `hover` / `any-hover` media queries | `hover`; `none` in tablet mode; any-hover `none` and `hover` | **`hover`** / **`hover`** | same |
| Touch-event feature detection: `ontouchstart` and the other `ontouch*` attributes, `document.createEvent('TouchEvent')` | off on desktop, on with `--touch-events` | **off** | same |
| `navigator.deviceMemory` (window and workers) | the RAM rounded to a power of two, 2 to 32 | **8** | `navigator_device_memory.cc` |
| `navigator.cpuPerformance` | the CPU's tier, 1 to 4 | **0** ("unknown") | browser, `GetCpuPerformanceTierOverride` |
| `navigator.connection` | the browser's network-quality estimate | **4g**, rtt **200**, downlink **1.6** ([details](#moltnet-connection-and-network-quality)) | `network_state_notifier.cc` |
| `navigator.userAgentData.getHighEntropyValues()` | the real platform version, architecture, bitness, WoW64, full versions and form factors | **blank** | `navigator_ua.cc` |
| Client hints | every hint a site asks for | **only** `Sec-CH-UA`, `Sec-CH-UA-Mobile`, `Sec-CH-UA-Platform`, `Save-Data` and the `Sec-CH-Prefers-*` hints | `frame_fetch_context.cc` (subresources); content's `client_hints.cc` and MoltNet's `ClientHintsControllerDelegate` (navigations) |

Normal and Incognito windows are unchanged: their goal is per-site
unlinkability, and none of these values can usefully vary per site (a
random touch-point count or memory size per site would change layouts and
download sizes from site to site; one fixed value would only make users
look like other MoltBrowser users). MoltNet ignores Shields off, as
elsewhere, and the kill switch turns all of it off.

### Touchscreens

**Where 152 decides.** Everything a page can learn about the touchscreen
without being touched comes from six `WebPreferences` fields, which content
fills from the devices in `SlowWebPreferenceCache::Update`:
`touch_event_feature_detection_enabled` (the `--touch-events` switch;
disabled on desktop by default, crbug.com/392584),
`available_pointer_types` and `available_hover_types`
(`ui::GetAvailablePointerAndHoverTypes`), `primary_pointer_type` and
`primary_hover_type` (`ui::GetPrimaryPointerType` / `GetPrimaryHoverType`)
and `pointer_events_max_touch_points` (`ui::MaxTouchPoints`).
`WebContentsImpl::ComputeWebPreferences` loads them and then calls the
embedder's `OverrideWebPreferences`, also when a device change (a
touchscreen plugged in, tablet mode) recomputes them. Blink copies them
into `Settings` and a runtime feature (`WebViewImpl::ApplyWebPreferences`),
where `navigator.maxTouchPoints` (`NavigatorEvents`), the
`pointer`/`hover` media queries (`MediaValues`) and touch-event feature
detection (the `[RuntimeEnabled=TouchEventFeatureDetection]` `ontouch*`
attributes, `Document::createEvent`) read them.

**What MoltNet does: one browser-side override.** In MoltNet profiles
`ChromeContentBrowserClient::OverrideWebPreferences` ends by setting the
six fields to a mouse-only desktop (`molt_net_device.{h,cc}`): no touch
points, a fine pointer that can hover, no touch-event feature detection.
Chosen over Blink hooks because it is one hook for all four signals, in
every frame and renderer of the profile; it holds when the devices change;
and the real values never reach a MoltNet renderer.

**Real touch input still works**, the way Chromium on any desktop delivers
it while touch-event feature detection is off: a touch scrolls, zooms and
taps; pages get pointer events (`pointerType` "touch", with the uniform
contact of [Touches](#touches)), the compatibility mouse events and click
of a tap, and touch events for listeners added with `addEventListener`.
Input routing doesn't read these preferences; the other readers found are
DevTools' emulation and the media controls' first guess (mouse until the
first touch). `TouchEvent`, `Touch` and `TouchList` stay defined, as for
every Chromium desktop user. **Cost:** pages that choose a touch layout from `maxTouchPoints` or
`pointer: coarse` show their desktop layout (smaller targets; hover menus
open on a tap through the compatibility events). Tor Browser reports the
same desktop.

**Not hidden: touching.** A page that waits for the user to touch the
screen learns it is a touchscreen (`pointerType` "touch", touch events,
`sourceCapabilities.firesTouchEvents`, the `pointercancel` of a pan). Hiding
that would mean rewriting touches as mouse input, which breaks the pinch,
pan and multi-finger handling of maps, drawing apps and games, and touch
events would still give it away, so it isn't done. Pen pressure, tilt and
twist stay real ([Touches](#touches)). DevTools' touch emulation still
works.

### Client hints

**What MoltNet sent.** `devicePixelRatio` was already 1, but a site that
asked (with `Accept-CH`, `<meta http-equiv="Accept-CH">`, `delegate-ch`, an
iframe's `allow` attribute, `Critical-CH` or an HTTP/2 `ACCEPT_CH` frame)
got, on its navigations and subresource requests:

* `Sec-CH-DPR` / `DPR`: the real device scale factor (of the primary
  display, on navigations) times the zoom;
* `Sec-CH-Width` / `Width`: an image's layout width times that real ratio;
* `Sec-CH-Viewport-Width` / `-Height`, `Viewport-Width`: the letterboxed
  page size, which script reads anyway;
* `Sec-CH-Device-Memory` / `Device-Memory`: 2 to 32;
* `RTT`, `Downlink`, `ECT`: the browser's network-quality estimate, which
  isn't specific to Tor's connection;
* the high-entropy user-agent hints, which `getHighEntropyValues()` also
  gave every page: `Sec-CH-UA-Platform-Version` (on Windows a version that
  tells Windows 10 from 11 and their feature updates apart, such as
  "10.0.0" or "19.0.0"; on macOS the exact release, such as "15.6.1"; empty
  on Linux at 152), `-Arch` ("x86" or "arm"), `-Bitness`, `-WoW64`,
  `-Model` (empty on desktop), `-Full-Version` and `-Full-Version-List`
  (the exact build, 152.0.7977.169, which the reduced user-agent string
  hides) and `-Form-Factors` ("Desktop").

**What MoltNet sends now: only the low-entropy and preference hints.**
`Sec-CH-UA`, `Sec-CH-UA-Mobile` and `Sec-CH-UA-Platform` (sent on every
request by default, which sites and firewalls rely on, and the same for
every MoltNet user of an OS and major version, like the user-agent
string), `Save-Data` (only while data saver is on) and
`Sec-CH-Prefers-Color-Scheme`, `-Reduced-Motion` and
`-Reduced-Transparency` (which repeat media queries any page can
evaluate). Every other hint is never sent, whatever the site asks for. The
rule is an allow-list (`moltnet_device_values.h`), so a hint a later
Chromium adds is withheld too.

**Why withhold rather than send uniform values** (DPR 1, the letterboxed
viewport, memory 8, ...): client hints are optional; Firefox, Safari and
Tor Browser send none, so every site already works without them. Uniform
values would each have to agree with the rest of the uniform device at
every zoom level and stay in step with it, for nothing a script can't
compute itself. And withholding is one rule at two choke points.

**`getHighEntropyValues()`** returns what Chromium itself reports when it
hides these values (with a `--user-agent` override): `platformVersion`,
`architecture`, `bitness`, `model` and `uaFullVersion` are "",
`fullVersionList` and `formFactors` are empty, `wow64` is false; `brands`,
`mobile` and `platform` are unchanged. Sites that look for Windows 11 in
`platformVersion` see no version and show their default.

**Where.**

* Subresource requests (Blink): `FrameFetchContext`'s
  `ShouldSendClientHint`, the per-hint decision for every subresource
  request of a document, asks `WithholdsClientHint`
  (`moltnet_device.{h,cc}`). Workers' requests only carry `Save-Data` at
  152.
* Navigation requests (browser): content's `client_hints.cc` decides for
  documents, iframes, prefetches, `Critical-CH` and `ACCEPT_CH` restarts
  and the list it gives the network service. It enables a hint from the
  profile's `ClientHintsControllerDelegate` (stored `Accept-CH`, additional
  hints), from a prerendering page's `Accept-CH`, or from an iframe's
  container policy (the `allow` attribute, `delegate-ch`); the last two
  don't go through the delegate. So content's delegate interface gained
  `IsClientHintWithheld()` (no hint by default) and `IsClientHintEnabled()`
  asks it: a withheld hint is never enabled, whatever enabled it.
  `ClientHintsFactory` builds MoltNet profiles a `MoltNetClientHints`
  (`molt_net_client_hints.{h,cc}`), which answers from the same allow-list
  (`molt_net_device_values.h`), doesn't even store a withheld hint from
  `Accept-CH`, and reports only the low-entropy user-agent metadata. A
  `Critical-CH` or `ACCEPT_CH` restart only happens for a header the
  restart would add, and a withheld hint adds none.

### Device memory and CPU performance

* `navigator.deviceMemory` is **8** in MoltNet windows and workers: below
  that, sites start serving "lite" pages; it is at or below the RAM of
  nearly every machine that runs MoltBrowser's on-device model; and it was
  the most any desktop Chromium reported before the cap went up to 32
  (crbug.com/454354290), so it is a common value. The `Device-Memory`
  hints aren't sent at all (above). `performance.memory`'s heap limit is
  the same for everyone too, that of the 4 GB machine every MoltNet
  renderer's V8 heap is configured as
  ([performance.memory](#moltnet-performancememory)).
* `navigator.cpuPerformance` (stable at 152, on secure pages) reports a
  tier from 1 to 4 that content derives from the CPU model
  (`content::cpu_performance::GetTier`) and hands to each renderer at
  start. In MoltNet profiles `ChromeContentBrowserClient::
  GetCpuPerformanceTierOverride` now returns **0**, the API's "unknown",
  which pages already handle (CPUs the tier tables don't know report it);
  it takes precedence over the user's and the enterprise override, which
  would differ between users.

### Which contexts

The browser-side values (touch, pointer, hover, the CPU tier and the
navigation hints) cover every frame of the MoltNet profile. The Blink-side
ones (memory, subresource hints, `userAgentData`) cover the whole MoltNet
renderer process, like `navigator.hardwareConcurrency` and the user agent
itself. Both follow the kill switch.

### Code

| Where | What |
|---|---|
| `core/molt_fingerprinting/moltnet_device_values.{h,cc}` | the client-hint allow-list and the memory value, std-only (+ `moltnet_device_values_unittest.cc`, in `:unit_tests`) |
| `core/molt_fingerprinting/moltnet_device.{h,cc}` | Blink glue: memory, withheld hints, blank `userAgentData` values (+ `moltnet_device_test.cc`) |
| `chrome/browser/molt_ai/fingerprinting/molt_net_device_values.h` | the browser's values: the same allow-list, the input devices, the CPU tier; std-only (+ `molt_net_device_values_unittest.cc`, against Blink's) |
| `chrome/browser/molt_ai/fingerprinting/molt_net_device.{h,cc}` | touch / pointer / hover WebPreferences and the CPU tier (`:device`) |
| `chrome/browser/molt_ai/fingerprinting/molt_net_client_hints.{h,cc}` | MoltNet's `ClientHintsControllerDelegate` (`:client_hints`) |

Upstream hooks (each marked `MoltBrowser:`): Blink
`loader/frame_fetch_context.cc` (`ShouldSendClientHint`),
`frame/navigator_ua.cc` (`NavigatorUA::userAgentData`),
`frame/navigator_device_memory.cc` (`deviceMemory`); content
`public/browser/client_hints_controller_delegate.{h,cc}` (new virtual
`IsClientHintWithheld`, false by default) and
`browser/client_hints/client_hints.cc` (`ClientHintsExtendedData` keeps
the delegate, `IsClientHintEnabled` asks it); browser
`chrome/browser/client_hints/client_hints_factory.cc`
(`BuildServiceInstanceForBrowserContext`) and its `BUILD.gn`,
`chrome/browser/chrome_content_browser_client.cc`
(`OverrideWebPreferences`, `GetCpuPerformanceTierOverride`).

### Verification

Standalone (no Chromium checkout): `moltnet_device_values_unittest.cc` (5
tests) and `molt_net_device_values_unittest.cc` (3 tests, including the
browser's allow-list against Blink's for every value from -8 to 255) pass
with g++ (ASan/UBSan) and clang (UBSan trap, `-fno-exceptions
-Wunsafe-buffer-usage -Wshorten-64-to-32 -Wimplicit-int-conversion
-Wexit-time-destructors -Wglobal-constructors`), with the module's other
pure tests still passing. `moltnet_device.cc`, `molt_net_device.cc`,
`molt_net_client_hints.cc` and the changed
`client_hints_controller_delegate.cc` type-check with the same strict
flags against stand-ins of the pinned signatures (the pinned
`client_hints.h`, `enabled_client_hints.h`, `pointer_device.h` and
`user_agent_metadata.h` themselves; the enums generated from the pinned
mojoms), and so do the hooked `navigator_ua.cc` and
`navigator_device_memory.cc` as they are. Two harnesses run the real code
under ASan/UBSan: the Blink one with `ShouldSendClientHint` extracted
verbatim from `frame_fetch_context.cc` (6 tests: normal windows untouched,
the allow-list, opt-in and permissions policy still applying, memory,
`userAgentData` through the real `NavigatorUA::userAgentData`), the
browser one with `IsClientHintEnabled` extracted verbatim from content (8
tests: the factory and kill switch, a hint enabled by delegation still
withheld, nothing withheld stored or reported, the low-entropy metadata,
the input-device preferences for MoltNet only, the CPU tier). 26 mutants
of the new code and hooks, the new-window code included (a hook removed,
a value left real, the allow-list changed, every profile treated as
MoltNet, ...), all fail these checks.
**Needs the build machine:** compiling the hooks and the new targets
(including `gn check`, DEPS and the new `ClientHintsControllerDelegate`
virtual in every implementation), `blink_unittests`
(`MoltNetDeviceTest.*`, which checks the headers of a real
`FrameFetchContext`), `molt_ai_unittests`, and in a MoltNet window:
`matchMedia('(pointer: fine)')`, `navigator.maxTouchPoints`, `'ontouchstart'
in window` on a Windows touchscreen laptop (also in tablet mode and with
`--touch-events`) and that touch scrolling, tapping and pinch-zoom still
work; a client-hints test page (`Accept-CH`, `Critical-CH`, `delegate-ch`,
an iframe `allow="ch-dpr"`) showing only the allowed headers on documents,
iframes and images; `getHighEntropyValues()` and `navigator.cpuPerformance`
on Windows, macOS and Linux.

**Automated** by [`tools/leak_test`](LEAK_TESTING.md) on Linux: the pointer and hover queries,
`maxTouchPoints`, touch-event detection, `deviceMemory`, `cpuPerformance`,
blank `getHighEntropyValues()`, and the hints a server receives on a
subresource request after asking for every hint with `Accept-CH` and
`Critical-CH` (the navigation's headers are in the run's server log). Still
manual: touchscreens, `delegate-ch` and iframe delegation, Windows and macOS.

## MoltNet: connection and network quality

The network service keeps one network-quality estimate for the whole
browser (`net::NetworkQualityEstimator`): an effective connection type
(slow-2g to 4g), HTTP and transport round-trip times and a downlink
throughput, worked out from the requests of every window. Content sends it
to every renderer, together with the operating system's connection type
and whether data saver is on, and Blink uses it in several places. In
MoltNet windows that estimate described the user's own connection (mixed
with Tor's), not something every MoltNet user shares; it moved while
pages loaded, so two sites open at the same time, or one site before and
after New Identity, could each see changes the other caused; and it
decided how MoltNet's network context scheduled a page's requests. The
`ECT`, `RTT` and `Downlink` client hints were already withheld ([Client
hints](#client-hints)). Now nothing a MoltNet page can observe depends on
the estimate:

| What a page reads | Before, in MoltNet | Now |
|---|---|---|
| `navigator.connection.effectiveType` (windows and workers) | the browser's estimate | **`4g`** |
| `navigator.connection.rtt` | the estimate, times a per-site and per-renderer factor between 0.9 and 1.1, in 50 ms steps | **200** |
| `navigator.connection.downlink` | the same for the throughput, in 50 kbps steps up to 10 | **1.6** |
| `navigator.connection.saveData`, the `Save-Data` header, `prefers-reduced-data` | false: Chromium never turns data saver on in an off-the-record profile | **false** |
| `navigator.connection.type`, `downlinkMax` (exposed on Android and ChromeOS only) | the system's connection | **`unknown`**, **`Infinity`** |
| `change` and `typechange` events on `navigator.connection` | whenever the estimate or the connection changed | **never** (DevTools' network emulation still fires them) |
| Blink's own network-quality measures (below) | followed the estimate | **as on a 4g network**, always |
| Request scheduling in MoltNet's network context | held back low-priority requests while the estimate said 3g or slower | **as on a network of unknown quality** (the same as on 4g), always |
| `navigator.onLine`, `online` and `offline` events | the system's | unchanged |

Normal and Incognito windows keep Chromium's behaviour, per-site factor
included: a fixed value there would only make a user look like other
MoltBrowser users, which normal windows don't try.

### Blink's own uses

Everything in Blink that depends on the estimate, the connection type or
data saver reads it from the renderer's `NetworkStateNotifier`. At 152:
`navigator.connection` (`modules/netinfo`); the `ECT`, `RTT`, `Downlink`
and `Save-Data` request headers (`frame_fetch_context.cc`,
`worker_fetch_context.cc`); `prefers-reduced-data` (`media_values.cc`);
the lazy-loading distances of images and iframes, which are longer on
slower networks (`lazy_load_media_observer.cc`,
`lazy_load_frame_observer.cc`); the web-font measure that shows text in a
fallback font at once on 3g and slower (`remote_font_face_source.cc`);
low-priority iframes on slow networks
(`frame_resource_fetcher_properties.cc`); blocking scripts inserted with
`document.write()` on 2g (`document_write_intervention.cc`); preloading
only metadata of media on cellular connections (`html_media_element.cc`);
and preloading eligibility under data saver
(`anchor_element_interaction_tracker.cc`). Blink's own request scheduler
(`ResourceLoadScheduler`) doesn't read the estimate at 152. Each of these
changes what a page can see or time, so in MoltNet each now behaves as on
a 4g network for every user.

### What MoltNet does

**One renderer-side choke point.** When a MoltNet renderer starts,
`Configure(kUniform)` pins its `NetworkStateNotifier` to fixed values
(`moltnet_network.{h,cc}`, through one new upstream method,
`NetworkStateNotifier::SetUniformNetworkState`), before any document or
worker exists. From then on the updates content sends
(`SetWebConnection`, `SetNetworkQuality`, `SetSaveDataEnabled` and the
holdback experiment's `SetNetworkQualityWebHoldback`) change nothing, so
no change event fires, and `RoundRtt`/`RoundMbps` add no per-site factor,
so every site reads the same numbers. Every reader above, in every
window, iframe and worker of the process, sees the fixed values; none of
them needed a hook of its own. The on-line state isn't pinned, and
DevTools' network emulation still applies on top while it is on.

**The network context.** MoltNet's network context has no network-quality
estimator (`NetworkContextParams.molt_no_network_quality_estimator`, set by
`molt_net_network_context_policy.h`). Its request scheduler
(`ResourceScheduler`, which at 152 holds back low-priority requests while
the estimate says 3g or slower) treats every MoltNet page as on a network
of unknown quality, which schedules as on 4g; and MoltNet's own requests
no longer feed the estimate that regular windows read.

**The values** (`moltnet_network_values.h`):

* `effectiveType` **4g**: the fastest class and what nearly every desktop
  reports; the class in which Blink applies none of its slow-network
  measures; and the one in which sites serve their normal pages, as they
  do in Firefox, Safari and Tor Browser, which have no
  `navigator.connection`.
* `rtt` **200** and `downlink` **1.6**: Chromium's own typical network
  quality for 4g (175 ms and 1600 kbps in `net/nqe`), which Blink already
  reports for a type when it shows the type without the measurement (the
  network-quality web holdback), on NetInfo's 50 ms and 50 kbps grid.
  Chromium calls any HTTP round trip under 272 ms 4g, so the three agree.
  A round trip through Tor is usually slower, which Chromium would call
  3g; but 3g turns on the slow-network measures above and makes sites
  serve their low-bandwidth pages, which Tor's throughput usually doesn't
  call for. A moderate downlink makes adaptive players that start from it
  begin at a moderate quality and adapt from there.
* `saveData` **false**: data saver only exists on Android, and Chromium
  never turns it on for an off-the-record profile, which MoltNet's is, so
  the user's setting never counted in MoltNet. Pinning it also keeps the
  `Save-Data` header of subresource requests (never sent in MoltNet) and
  `saveData` in agreement.
* `type` **unknown** and `downlinkMax` **Infinity**: what Chromium itself
  reports when it hides the connection type (its `NetInfoConstantType`
  feature). Not a cellular type, so media keep their usual preload.

**`navigator.onLine` stays as upstream.** It and the `online` and
`offline` events come from whether the operating system has a network at
all, not from the estimate; a site can't change it; while a MoltNet page
loads, it is `true` for everybody; and when the network goes down, every
site sees its requests fail at that moment anyway. Pages use it to show
that they are offline and to retry.

**What it costs MoltNet users.** Sites that adapt to the connection
always see a 4g connection with a moderate downlink, so on a slow circuit
they don't switch to lighter pages by themselves (as in Tor Browser);
adaptive video still adapts to the throughput it measures. Blink's
slow-network measures never apply: text set in a web font waits for the
font as on a fast network, and lazy images and iframes load at the 4g
distances, Chromium's shortest. MoltNet's network context schedules
requests as on 4g whatever the circuit does.

### Which contexts

The renderer values cover the whole MoltNet renderer process (windows,
iframes, dedicated, shared and service workers), like the other uniform
values, and follow the kill switch. The network-context setting covers
every network context of a MoltNet profile; it is part of MoltNet's
network settings ([MOLTNET_COMPLIANCE.md](MOLTNET_COMPLIANCE.md) §2,
*Shared network state*), not of fingerprinting protection, so the kill
switch doesn't turn it off.

### Code

| Where | What |
|---|---|
| `core/molt_fingerprinting/moltnet_network_values.h` | the fixed values, std-only (+ `moltnet_network_values_unittest.cc`, in `:unit_tests`) |
| `core/molt_fingerprinting/moltnet_network.{h,cc}` | the values as Blink's `NetworkState` and the pin, called from `Configure(kUniform)` (+ `moltnet_network_test.cc`) |
| `chrome/browser/molt_ai/tor/molt_net_network_context_policy.h` | `no_network_quality_estimator` for MoltNet's network contexts (+ `molt_net_network_context_policy_unittest.cc`) |

Upstream hooks (each marked `MoltBrowser:`):
`platform/network/network_state_notifier.{h,cc}`
(`SetUniformNetworkState`, an early return in `SetWebConnection`,
`SetNetworkQuality`, `SetNetworkQualityWebHoldback` and
`SetSaveDataEnabled`, and no factor in `GetRandomMultiplier` once
pinned); `services/network/network_context.cc` (`MakeURLRequestContext`:
no `set_network_quality_estimator` for MoltNet) and
`services/network/public/mojom/network_context.mojom` (the new field).

### Verification

Standalone (no Chromium checkout): the real `network_state_notifier.cc`
with the hooks, `moltnet_network.cc`, `moltnet_network_test.cc` (8 tests:
the fixed state, every site reading the same rounded values where a
regular notifier still varies them, the browser's updates changing
nothing and firing no event, the on-line state still following the
system, DevTools' emulation, one change at pinning, an unpinned notifier
unchanged) and `moltnet_network_values_unittest.cc` (6 tests: the enum
values, agreement with the estimator's thresholds and typical values,
NetInfo's grid) pass against stand-ins of the 152 base and WTF types with
g++ (ASan/UBSan) and clang (UBSan trap, `-fno-exceptions`); the hooked
file warns exactly where the unchanged 152 file does under
`-Wshorten-64-to-32 -Wimplicit-int-conversion -Wshadow
-Wexit-time-destructors -Wglobal-constructors`. The network-context hook,
extracted verbatim from `network_context.cc` with the
`ResourceSchedulerClient` creation that reads its result, passes 2 tests
against stand-ins, and the policy's unit tests pass against the mojom
stand-in. 32 mutants of the hooks, values and policy (each hook removed
or inverted, the per-site factor kept, the on-line state overwritten, the
real values kept, each value changed, the policy applied to every
profile or to none) all fail these checks. Every changed C++ file
type-checks against Chromium 152's headers (`tools/compile_check`, with
the mojom regenerated). **Needs the build machine:** compiling and linking,
`blink_unittests` (`MoltNetNetworkTest.*`), `molt_ai_unittests`, and in a
browser started with `--force-effective-connection-type=Slow-2G` (which
makes the estimate slow-2g): a regular window reports slow-2g and its
lazy images and iframes load early, while a MoltNet window and its
workers report 4g, 200, 1.6 and false, load lazy content at the 4g
distances, don't show fallback text at once while a `font-display: auto`
web font loads, and don't hold back low-priority requests
(`chrome://net-export`); no `change` event in MoltNet while another window
loads heavily. **Automated** by [`tools/leak_test`](LEAK_TESTING.md):
`navigator.connection` in a MoltNet window and in a dedicated worker, and
no `change` event while the page runs.

## MoltNet: audio, DRM and media capabilities

Three media APIs answered from the machine itself: Web Audio from the
output device (its sample rate, buffer, channels and delay, and when it
asks for audio), Encrypted
Media Extensions from the content decryption modules (CDMs) the build and
the operating system offer, and Media Capabilities and WebCodecs from the
video decoders and encoders, including the GPU's, and from the profile's
decode history. In MoltNet windows all three now describe the same media
hardware for every user:

| What a page reads | Before, in MoltNet | Now |
|---|---|---|
| `AudioContext.sampleRate` when the page gives no `sampleRate` | the output device's rate (44.1, 48, 96 kHz, ...) | **48000**; the context really runs at 48 kHz |
| `baseLatency`, for each `latencyHint` | the device's buffer over its rate (256 frames on macOS, 480 on Windows, 512 with PulseAudio, more with Bluetooth) | the uniform device's: **0.01** for `"interactive"` and `"balanced"`, **1024/48000** for `"playback"`, and Chromium's rule for a number ([below](#web-audios-output-device)) |
| `createScriptProcessor()` with no buffer size | four device buffers, rounded to a power of two | **2048** (4096 for `"playback"`) |
| `outputLatency` | the device's output delay (100 ms and more with Bluetooth) in 8 ms steps, 1 ms steps once the page may use the microphone | **0.024** |
| `getOutputTimestamp()` | the device's delay and its clock | **{0, 0}** until something has been rendered, then `currentTime` minus 24 ms, heard now |
| `playbackStats` (latency, underruns) | the device's delay to 0.1 ms (5 µs when cross-origin isolated); the system's glitches | **24 ms**; **no underruns** |
| `destination.maxChannelCount` | the device's channels (1, 2, 6, 8, ...) | **2**; a `channelCount` above 2 throws `IndexSizeError` |
| `renderSizeHint: "hardware"` | 128 at 156 (no device value is read) | **128**, pinned |
| When rendering happens: `currentTime` steps, AudioWorklet `process()` bursts, `onaudioprocess` callbacks, `getOutputTimestamp()` steps | when the device asks for audio: its buffer (128 to 2048 frames and more), rate and callback pattern, a stall or a gap when it changes | the uniform device's clock: **480 frames every 10 ms** (3 or 4 render quanta), from the moment the context starts, whatever the device ([below](#when-web-audio-renders)) |
| A machine with no output device | a 44.1 kHz fallback device, an `error` event (`setSinkId("")`, a lost device), no `audiooutput` in `enumerateDevices()` | **like every other machine**: the same values, schedule and device list, no `error` |
| `requestMediaKeySystemAccess()`; Media Capabilities with a `keySystemConfiguration` | Clear Key (and, in builds that have them, which Widevine or PlayReady version, robustness levels and hardware-secure decoding the machine has) | **`NotSupportedError`**, every key system; `{supported: false}` |
| `mediaCapabilities.decodingInfo()` for video | supported from the decoders, then `smooth` and `powerEfficient` from the decode history and the GPU's decoders | **supported** when the build has its own decoder, **smooth**, **not power efficient** |
| `decodingInfo()` and `encodingInfo()` for `"webrtc"` | the machine's WebRTC codecs, hardware ones included | **unsupported** (WebRTC is off in MoltNet) |
| WebCodecs `isConfigSupported()` and `configure()` with `hardwareAcceleration: "prefer-hardware"`; which decoder or encoder runs otherwise | the GPU's decoders and encoders (and macOS's H.264 encoder) | **not supported**; always the build's software codecs |

Normal and Incognito windows keep Chromium's behaviour: there these values
can't vary per site without breaking audio and video, and a fixed value
would only make a user look like other MoltBrowser users. MoltNet ignores
Shields off, as elsewhere, and the kill switch turns all of it off.

### Web Audio's output device

**Where 156 gets the values.** An `AudioContext` opens an output stream
through content's `RendererWebAudioDeviceImpl`, which reads the device's
parameters (rate, channels, buffer size and its limits) and derives the
stream's callback buffer from them and the `latencyHint`
(`GetOutputBufferSize`). `sampleRate` is the device's rate unless the page
gave one; `baseLatency` is the callback buffer (or the render quantum, if
longer) over the rate; `createScriptProcessor()` sizes a default
ScriptProcessorNode from the callback buffer; `maxChannelCount` is the
device's channel count; `outputLatency`, `getOutputTimestamp()` and
`playbackStats` follow the delay the device reports with each callback.

**What MoltNet does.**

* `AudioContext::Create` runs a context at **48 kHz** when the page gives
  no rate, as if the page had asked for it, and renders
  `renderSizeHint: "hardware"` in Chromium's default 128 frames (156 reads
  no device value there either; the pin keeps a later Chromium that does
  from showing it). A rate, latency hint or render quantum the page chooses
  is its own choice and is kept.
* `RealtimeAudioDestinationHandler` reports the callback buffer of one
  uniform device (`GetCallbackBufferSize`, `GetFramesPerBuffer`), so
  `baseLatency` and the default ScriptProcessorNode size describe it, and a
  maximum of **2** channels, which `channelCount` is checked against.
* `outputLatency` is **24 ms**; `getOutputTimestamp()` is `{0, 0}` until
  the context has rendered anything, then the frame rendered 24 ms ago,
  heard now; `playbackStats` sees a 24 ms delay and no underruns for every
  rendered buffer (they are still only collected while the page is
  visible).

**The uniform device** (`moltnet_media_values.h`): 48 kHz, stereo, a
480-frame (10 ms) buffer and a 24 ms output latency. The context's callback
buffer is derived from it exactly as Chromium derives it from a device
without buffer limits of its own: the 10 ms at the context's rate for
`"interactive"` and `"balanced"`, 20 ms rounded up to a power of two
(1024 frames at 48 kHz) but at least the 10 ms for `"playback"`, and for a
number the frames it asks for, at least 128 and the 10 ms unless it asks
for 128 or fewer, rounded up to 128 frames, at most 8192.

* **48 kHz** is what nearly every output device runs at today (Windows'
  and PipeWire's default, the built-in speakers of current Macs, HDMI and
  USB interfaces), Chromium's own rate when it can't read a device, and the
  rate of Opus and of WebRTC audio, so most MoltNet users need no
  resampling at all.
* **480 frames** is Windows' shared-mode period and the buffer Chromium
  falls back to when it can't read a device.
* **24 ms** is a 10 ms device with double buffering and a few milliseconds
  of converter latency, and it is on the 8 ms grid Chromium rounds
  `outputLatency` to, so it reads the same with or without microphone
  permission.
* **Stereo**: what almost every device offers and what Tor Browser reports.

**Running at 48 kHz, and what it costs.** Since 156 Chromium opens the
renderer's stream at the context's rate when a page asks for one
(`WebAudioRemoveAudioDestinationResampler`, on by default on desktop), and
the audio service converts it to the device's rate with its
`SincResampler`, as it already does for every page that picks a rate. On a
48 kHz device nothing changes. On another device the conversion costs, per
second of stereo audio, measured on the 2.1 GHz Xeon VM this was developed
on with 156's resampler loop and convolutions (64 taps, 10 ms requests,
`-O2`): 2.1 ms of CPU to 44.1 kHz, 3.2 ms to 96 kHz and 5.6 ms to 192 kHz
with AVX2 (0.2-0.6% of a core); 2.8, 4.7 and 8.7 ms with SSE; 5.0, 9.6 and
17.8 ms without SIMD. It only runs while a context plays. It adds the
filter's delay (32 frames, under 1 ms) and at most one 10 ms request of
buffering (an estimate, not measured). The sound is the same resampled
audio any page that asks for 48 kHz already gets.

**Tor Browser** (Firefox's resistFingerprinting mode, checked in Firefox's
current sources) reports 44.1 kHz, `baseLatency` 0 (always, in Firefox),
an `outputLatency` fixed per OS (25 ms on Linux, 40 ms on Windows, 512
frames on macOS), 2 channels, and `getOutputTimestamp()` derived from
`currentTime` and `performance.now()` minus that latency. Firefox has no
`latencyHint`, `sinkId`, `renderSizeHint` or `playbackStats`. MoltNet
matches it on the channels and on deriving the timestamp from a fixed
latency, and not on the rest: a page tells Chromium from Firefox in many
other ways, so what matters is that MoltNet users all answer alike, and
48 kHz spares most of them the resampler that 44.1 kHz would cost.

**`sinkId` and `setSinkId()`** are unchanged. Output device IDs only reach
a page once it has a media permission (Chromium hides them, and lists one
device per kind, before that), and they are salted per site and per
MoltNet identity. Before that, `setSinkId("")` succeeds and every other ID
is `NotFoundError` for everyone (machines without an output device
included, [below](#when-web-audio-renders)); after it, the choice of
output device is what the user granted.

**What it costs MoltNet users.** Surround audio (5.1, 7.1) plays as
stereo. Pages that use `outputLatency` or `baseLatency` to line sound up
with pictures (games, music tools) are off by the difference between the
real delay and 24 ms: a few milliseconds on built-in speakers, up to about
200 ms with Bluetooth headphones. `playbackStats` never reports underruns,
so a page can't react to them. 44.1 and 96 kHz devices pay the resampler
above. Rendering on MoltNet's own clock adds 13 to 32 ms of latency
([below](#when-web-audio-renders)).

### When Web Audio renders

**What a page can time.** Chromium renders an `AudioContext` when the
output device asks for audio: `AudioDestination::Render` runs on the
device's audio thread (or posts to the AudioWorklet thread) with the
device's buffer, and renders as many 128-frame quanta as that buffer
needs, back to back. Fixing the reported values doesn't hide this: a page
that polls `currentTime`, counts AudioWorklet `process()` calls between
messages, times `onaudioprocess` or `getOutputTimestamp()` steps, or
compares them with `requestAnimationFrame` sees bursts of 1 quantum every
2.7 ms (a 128-frame device), 8 every 21 ms (1024), 16 every 43 ms (a
Bluetooth or `"playback"` buffer), and 3 or 4 every 10 ms on Windows'
480-frame default. Asking the device for the uniform buffer wouldn't help
either: the audio service still pulls the renderer's buffers when the
hardware wants them, in the hardware's pattern. A machine with no output
device shows too: Chromium opens a fake 44.1 kHz stream for it, or the
stream fails and the context fires `error` and suspends.

**What MoltNet does: a clock of its own.** Every realtime
`AudioDestination` in a MoltNet renderer gets its `WebAudioDevice` from
MoltNet (`MoltNetAudioDevice`, one hook in `AudioDestination`'s
constructor) instead of the platform. It tells the destination that the
device is the uniform one (48 kHz, or the page's rate; the uniform rule's
buffer for the context's latency hint, 480 frames for `"interactive"` at
48 kHz), and calls `Render()` on a realtime thread of its own on a fixed
schedule: one buffer every buffer's duration (10 ms), from the moment the
context starts or resumes, measured on the system's monotonic clock.
The clock is `media::FakeAudioWorker`, the one Chromium uses for
`{type: "none"}` sinks and for silent output: a late tick runs once and
the next one is the next on time, never a burst of catch-up. The
destination's own logic is unchanged, so rendering follows the uniform
device exactly as it would follow a real device with that buffer: 4, 4, 4
and 3 quanta per tick with output buffering bypassed (Chromium's default
on desktop), the AudioWorklet's earmark and its thread's realtime period
(macOS) from the uniform buffer.

What it renders goes into a ring buffer (`OutputRing`); the real device,
opened at the context's rate as Chromium opens it, reads the ring from its
own thread through `media::MultiChannelResampler`. Nothing the device
passes (its buffer, delay, timing, glitches) reaches the destination: the
clock's ticks pass a fixed delay and no glitches.

* **Drift.** The system clock and the device's clock differ by up to a
  few hundred parts per million. `DriftControl` (`moltnet_audio_clock.h`)
  keeps the ring short without running dry from the device side only: at
  each read it takes the clock's sawtooth out of the ring's level (the
  level it would have if the clock rendered continuously), and every half
  second of audio the lowest headroom of the last 1.5 s above a 3 ms
  margin sets the resampler's ratio, 1 plus the excess over 4 s, within
  1 ± 0.2% (3.5 cents; a typical device needs about 0.01%). The clock
  never changes for it, so the correction is not a device signal: a page
  can only hear it, through a microphone it has permission for.
* **Starting.** The ring fills to one of the device's callbacks and a
  cushion (another callback, at most 20 ms), plus the margin and 5 ms,
  before the device hears it; until then the device gets silence. An
  excess the device started with (a device that took long to start) is
  played rather than skipped and worked off by the ratio; silent excess
  (a context that hasn't played anything yet) goes at once.
* **Underruns and stalls.** If the ring runs dry (the clock's thread was
  held up beyond the margin) the device gets silence until it has filled
  again, and the margin grows by a callback (at least 2 ms, at most 20 ms
  or a callback) for the rest of the session. If the device stops reading
  for a while (a device change), the excess that grew on top of what it
  started with is dropped once the last 1.5 s all show it: one skip, where
  Chromium's audio has a gap anyway.
* **No device, a lost device.** With the default output device (no
  `sinkId`, or `""`) the device's status questions (`setSinkId()` checks,
  the echo-canceller update) always answer OK, and its failures (no device
  at all, the last one unplugged, a Bluetooth headset gone) stay inside:
  the context keeps rendering on its clock, its output goes nowhere, and
  the device is opened again after 1, 2, 4 ... 30 s. `AudioContext`
  accepts the default sink ID `""` even when the machine lists no output
  device (`IsValidSinkDescriptor`), so `setSinkId("")` resolves and a
  device list without outputs fires no `error`. `enumerateDevices()` lists
  an `audiooutput` entry before any permission, as on every machine with
  a device (and a microphone and a camera: [cameras, microphones and
  outputs](#moltnet-cameras-microphones-and-outputs)). Changes between
  devices (the system's default moving to
  headphones) were already handled in the audio service; they now show
  only as a gap in the sound.
* **A device the page chose by its ID** (which takes a media permission,
  see `sinkId` above) renders on the clock too, but answers for itself:
  `setSinkId()` gets its real status, and if it goes away the context gets
  `error`, as the spec and Chromium do.
* **Silent sinks** (`{type: "none"}`) run on the same clock with no device
  at all.
* **Suspend, resume, frozen frames, `setSinkId()`.** `suspend()` stops the
  clock (nothing renders after it returns), `resume()` starts it again
  with a fresh schedule from that moment; a frozen frame pauses and
  resumes it the same way. `setSinkId()` and a `channelCount` change make
  a new destination, as in Chromium, with its own clock from when it
  starts; `currentTime` carries on.

**What a page reads now** (checked against simulated devices of 128, 256,
441 (44.1 kHz), 480, 512, 1024 and 2048 frames, a bursty converted
stream, Chromium's fake 44.1 kHz stream and no device at all, single-
thread and AudioWorklet rendering, with and without output buffering:
every render quantum at the same time in every case, see Verification):
`currentTime` advances by 384 or 512 frames every 10 ms at 48 kHz (480
frames on average); AudioWorklet `process()` runs in those bursts on its
thread; a default `ScriptProcessorNode` (2048 frames) fires every 42.7 ms
on average; `getOutputTimestamp()` follows `currentTime` (it is
`currentTime` minus 24 ms). `playbackStats` counts the frames rendered,
with the fixed 24 ms latency and no underruns. The only variation left is
the operating system's scheduling of the clock's thread and how much of
the machine's CPU the graph takes (a slow machine renders late), which a
page can measure in many other ways.

**What it costs.** Latency: measured from rendering a frame to the device
playing it, against Chromium's own path, MoltNet adds about 13 ms on
128- and 256-frame devices, 15 to 17 ms on 480- and 512-frame devices,
20 ms on 1024-frame devices, 22 ms on a 44.1 kHz device's converted
stream and about 32 ms on 2048-frame devices (the ring holds about half a
device buffer, half a tick and the margin; simulated, steady state). A
context that plays from its first moment on a device that starts slowly
hears that delay as extra latency for its first seconds. CPU: the
resampler on the device side, about 2.3 ms per second of stereo audio
with AVX2 (0.23% of a core on the 2.1 GHz VM this was measured on), 2.7
to 2.9 ms with SSE, 6.5 to 7.7 ms without SIMD, with the ratio changed
every half second; the clock's thread does the rendering the device's
thread did before, plus a copy into the ring. Memory: the ring, a few
hundred kilobytes at most. Normal and Incognito windows, and
`OfflineAudioContext` (which renders on its own thread as fast as it
can), are unchanged.

### Encrypted Media Extensions

**What 156 offers.** `requestMediaKeySystemAccess()` (and Media
Capabilities' `keySystemConfiguration`, which runs the same request) asks
the frame's EME client, which asks the browser which key systems exist and
what they support. Widevine and PlayReady need Chrome branding, so
MoltBrowser's desktop builds only have Clear Key, a test and
interoperability key system real DRM content doesn't use; in a build with
CDMs the answers would show the CDM's version, robustness levels and
hardware-secure decoding.

**What MoltNet does: no key systems.** One hook,
`EncryptedMediaUtils::GetEncryptedMediaClientFromLocalDOMWindow`, where
every request gets its client, hands MoltNet pages a client that refuses
every key system with `NotSupportedError` and Chromium's own message for
an unknown one ("Unsupported keySystem or supportedConfigurations."). That
covers `requestMediaKeySystemAccess()` (also from a prerendered page once
it is shown) and Media Capabilities, which resolves `{supported: false,
smooth: false, powerEfficient: false}` with no `keySystemAccess`. A
`MediaKeys` can only come from a granted request, so `createMediaKeys()`,
`setMediaKeys()` and `getStatusForPolicy()` are never reached. The frame's
own client is never created, so a MoltNet renderer never asks the browser
about key systems. `encrypted` events still fire for encrypted media (they
describe the file).

**Why off rather than one uniform answer.** Tor Browser ships with DRM
off; DRM content needs a CDM these builds don't ship anyway; and "no key
system" is the same answer whatever CDMs a build, a later Chromium or the
operating system adds. (Firefox, and so Tor Browser, still grants Clear
Key; refusing it too changes no answer between two MoltNet users.) *Cost:*
Clear Key test pages and players don't work in MoltNet windows; DRM video
didn't play there before either.

### Media Capabilities, codec support and WebCodecs

**How 156 answers.** `decodingInfo()` first checks the codec against the
decoders the build supports (`media::IsDecoderSupportedVideoType`), then
asks the profile's decode history (`VideoDecodePerfHistory`; MoltNet's is
in memory, but learns from what MoltNet played on this machine) whether
such a video was smooth and power efficient, and the GPU whether it has a
decoder for it; a codec only a platform decoder handles is unsupported
without one. `"webrtc"` queries ask WebRTC's codec factories, hardware
codecs included. Audio-only queries depend on the build alone (supported,
smooth and power efficient alike).

**What MoltNet answers.**

* Video, clear: **supported** when the build has its own decoder for the
  codec (`media::IsDecoderBuiltInVideoCodec`: libvpx for VP8 and VP9, dav1d
  for AV1), **smooth**, and **not power efficient** (software decoding),
  from a fixed table in `MediaCapabilities::GetPerfInfo`, never from the
  history or the GPU. Tor Browser reports the same (smooth, not power
  efficient) in resistFingerprinting mode.
* Audio-only: Chromium's answer, unchanged.
* `"webrtc"` (`decodingInfo()` and `encodingInfo()`): **unsupported**, as
  WebRTC is off in MoltNet windows.
* With a `keySystemConfiguration`: unsupported (no key systems, above).
* `encodingInfo()` of type `"record"` is behind the experimental
  `MediaCapabilitiesEncodingInfo` flag at 156 (a `TypeError` by default);
  re-check when it ships.

**`canPlayType()` and `MediaSource.isTypeSupported()`** follow the codecs
compiled into the build, the same for every user of a build: MoltBrowser's
builds have no proprietary codecs (no H.264, HEVC or AAC) and no platform
HEVC or Dolby decoders, so 156's `media::IsDefault*SupportedType` answers
from libvpx, dav1d, libaom and the open-source audio decoders only, and
Chrome's renderer client doesn't override it. Not changed; the leak kit
compares them between machines. `MediaRecorder.isTypeSupported()` also
counted the machine's hardware encoders; in MoltNet it now answers from the
build's own encoders (see [MoltNet: MediaRecorder](#moltnet-mediarecorder)).

**WebCodecs.** `isConfigSupported()` with `"prefer-hardware"` asks the GPU
for a decoder or encoder, and with no preference a decoder or encoder may
still be the GPU's (its output frames and bitstreams show it). MoltNet's
WebCodecs gets no GPU codec factories (`RetrieveGpuFactories`) and no
platform decoders (`VideoDecoderBroker`), so it only uses the build's
software codecs: `"prefer-hardware"` is never supported and `configure()`
with it fails with `NotSupportedError`, as on a machine without hardware
codecs; every other preference decodes with libvpx or dav1d and encodes
with libvpx or libaom. macOS's own H.264 encoder isn't used either.

**`getVideoPlaybackQuality()`** is unchanged. Its dropped frames measure
how fast the machine plays a video, which a page can time itself
(`requestVideoFrameCallback`, animation frames); hiding them would stop
adaptive players from stepping down on slow machines.

**What it costs MoltNet users.** WebCodecs pages (video editors, cloud
gaming) decode and encode on the CPU; pages that require hardware fall
back or refuse. Sites that choose formats by `powerEfficient` see `false`
for every video and pick by their own preference. Media elements decode
in software too ([pixels and encoded
bytes](#moltnet-pixels-and-encoded-bytes), with its costs).

### What is left

* A page that picks a sample rate, latency hint or render quantum gets it;
  that is its own choice, the same for everyone.
* Output device IDs, once the user grants a media permission
  (`sinkId`, `setSinkId()`, `enumerateDevices()`), and a chosen
  device's failures.
* How the operating system schedules the audio clock's thread, and how
  long the graph takes to render (the machine's speed, which a page can
  time in many other ways).
* `<video>` and `<audio>` decode with the build's software decoders
  ([pixels and encoded bytes](#moltnet-pixels-and-encoded-bytes)); their
  speed still shows (`getVideoPlaybackQuality()`, frame timing).
* Normal windows' encoders (see *Encoder identity* in [Known
  gaps](#known-gaps-and-attacks); `MediaRecorder` in MoltNet: [its own
  section](#moltnet-mediarecorder)); `RTCRtpSender.getCapabilities()` (see
  the same list).
* `encodingInfo()` of type `"record"`, behind a flag at 156.

### Which contexts

The whole MoltNet renderer process: windows, iframes and workers (Media
Capabilities and WebCodecs exist in workers), like the other uniform
values. The kill switch turns it off.

### Code

| Where | What |
|---|---|
| `core/molt_fingerprinting/moltnet_media_values.{h,cc}` | the uniform device and its buffer rule, the output position, the key-system message, the Media Capabilities table; std-only (+ `moltnet_media_values_unittest.cc`, in `:unit_tests`) |
| `core/molt_fingerprinting/moltnet_media.{h,cc}` | which processes, the values as Blink types, MoltNet's EME client (+ `moltnet_media_test.cc`; the device list is in `moltnet_media_devices.{h,cc}`, [cameras, microphones and outputs](#moltnet-cameras-microphones-and-outputs)) |
| `core/molt_fingerprinting/moltnet_audio_clock.{h,cc}` | the clock's rate, the output ring and its drift control; std-only (+ `moltnet_audio_clock_unittest.cc`, in `:unit_tests`, with a simulation against devices of 128 to 8192 frames, bursts, drift and a late clock) |
| `core/molt_fingerprinting/moltnet_audio_device.{h,cc}` | MoltNet's `WebAudioDevice`: the clock (`FakeAudioWorker` on a realtime thread of its own), the real device and its failures; installed at `Configure()` (+ `moltnet_audio_device_test.cc`) |
| `core/molt_fingerprinting/moltnet_audio_output.{h,cc}` | the ring and the resampler to the real device (`MoltNetAudioOutput`), shared with [media playback's clock](#moltnet-media-playback-clock) |

Upstream hooks (each marked `MoltBrowser:`):
`modules/webaudio/audio_context.cc` (`AudioContext::Create`: the rate and
the `"hardware"` quantum; `outputLatency`; `getOutputTimestamp`;
`HandlePreRenderTasks`: the playbackStats update;
`IsValidSinkDescriptor`: the default device always valid),
`platform/audio/audio_destination.{h,cc}` (the constructor gets its
`WebAudioDevice` from `SetMoltNetDeviceFactory`'s factory when one is set),
`modules/mediastream/media_devices.cc` (`DevicesEnumerated`: the listed
output device, now [one of each kind](#moltnet-cameras-microphones-and-outputs)),
`modules/webaudio/realtime_audio_destination_handler.cc`
(`MaxChannelCount`, `GetCallbackBufferSize`, `GetFramesPerBuffer`),
`modules/encryptedmedia/encrypted_media_utils.cc`
(`GetEncryptedMediaClientFromLocalDOMWindow`),
`modules/media_capabilities/media_capabilities.cc` (`decodingInfo` and
`encodingInfo`: the `"webrtc"` branch; `GetPerfInfo`),
`modules/webcodecs/gpu_factories_retriever.cc` (`RetrieveGpuFactories`)
and `modules/webcodecs/video_decoder_broker.cc` (`UpdateDecoderFactory`).
All nine files are new in `src/` (the last three since the audio clock).

### Verification

Standalone (no Chromium checkout): `moltnet_media_values_unittest.cc` (14
tests: the device, the buffer for every latency hint, rate and nonsense
input, the base latency, the output latency on Chromium's grid, the
ScriptProcessorNode size, the output position, the key-system message, the
Media Capabilities table, the listed devices and what a failed
`getUserMedia()` reports) passes with g++ (ASan/UBSan) and clang (UBSan
trap, `-fno-exceptions` and the strict warning set). A harness runs the
real `moltnet_media.cc` and the hooked code extracted verbatim from all six
upstream files, plus 156's unhooked `ChooseBufferSize` from
`script_processor_node.cc`, against stand-ins of the types they touch, in
kOff, kFarble and real kUniform (13 tests: the device rate and quantum, the
destination's channels and buffers on mono, 5.1, 7.1 and Bluetooth-sized
devices for every hint, the base latency and ScriptProcessorNode size as
Chromium computes them, the output latency with and without microphone
permission, the output timestamp, playbackStats, key systems through the
real lookup with the frame's client never asked, Media Capabilities with
the history and the GPU never asked, WebCodecs' GPU factories on the main
thread and in a worker, the decoder factories for every preference); the
values' ScriptProcessorNode rule equals 156's for every buffer from 1 to
20000 frames. 47 mutants of the values, the glue and every hook each fail
it or the values test. Every changed C++ file, the two tests included,
compiles without errors against Chromium 156's real headers
(`tools/compile_check`; it now mounts libwebm, which
`media_capabilities.cc` reaches through MediaRecorder).

**The audio clock.** `moltnet_audio_clock_unittest.cc` (18 tests: the
clock's rate and ticks, the ring's order, overflow, missing and short
channels, discarding, leading silence, nonsense input and its size for
every device, the clock's sawtooth taken out, priming, the ratio following
the excess within its limit, silent excess, a large excess dropped at
once, an excess the device started with kept, underruns and the growing
margin, and a simulation that runs the ring and the drift control against
devices of 128, 256, 441 (44.1 kHz), 480, 512, 1024, 2048 and 8192 frames
and a converted stream reading two buffers at once, with the clocks up to
200 ppm apart and up to 4 ms of scheduling jitter: once filled it never
runs dry or drops audio, follows the device's clock within 0.1% and holds
little more than the device's buffer and a tick) passes in the same two
configurations. A second harness
runs the real `audio_destination.cc` (hooked), 156's `PushPullFIFO` and
`FakeAudioWorker`, and the real `moltnet_audio_device.cc` and
`moltnet_media.cc` against stand-ins of the platform, on simulated time,
with simulated devices of 128, 256, 441 (44.1 kHz), 480, 512, 1024 and
2048 frames, a bursty converted stream, Chromium's fake 44.1 kHz stream and
no device (9 tests): the time of every render quantum (what `currentTime`,
`process()`, `onaudioprocess` and `getOutputTimestamp()` follow) is the
same for every device, single-thread and AudioWorklet, with and without
output buffering, while a normal window's follows the device; the other
latency hints and rates; no device (OK status, no error, the device opened
again on its schedule); a device lost and a device switched mid-play;
suspend, resume and a frozen frame; silent sinks; a chosen device's errors;
every frame rendered reaching the device in order, with the latency
quoted above. With it, the hooked `IsValidSinkDescriptor` and
`OnDevicesChanged`, extracted verbatim from `audio_context.cc` (3 tests;
the listed devices have tests of their own now): no `error` without an
output device in MoltNet, as against Chromium's. 28 mutants of the
clock, the device, the
hooks and the values each fail one of them. The resampler's cost was
measured with Chromium 156's `SincResampler` and `MultiChannelResampler`
built standalone. Every changed C++ file compiles without errors against
Chromium 156's real headers.

**Needs the build machine:** compiling and linking, `blink_unittests`
(`MoltNetMediaTest.*`, `MoltNetMediaValuesTest.*`, `MoltNetAudioClockTest.*`,
`MoltNetAudioDeviceTest.*`, and the upstream Web Audio and media devices
suites), `blink_platform_unittests` (`AudioDestination*`), `molt_ai_unittests`,
and the checks in `docs/BUILD_MACHINE_CHECKLIST.md` tagged "fp-media" (audio
on 44.1 and 96 kHz devices, a 5.1 device, Bluetooth, EME and WebCodecs
pages) and "fp-audio" (when Web Audio renders on 44.1 and 96 kHz, 128- and
1024-frame and Bluetooth devices, sound quality and latency, device changes
and a machine without an output device). **Automated** by
[`tools/leak_test`](LEAK_TESTING.md): every value in the first table except
the device-dependent cases a kit run can't produce, codec support compared
between runs, the render cadence (`audio.cadence_interval_ms`,
`audio.cadence_burst_frames`) and the listed output device
(`media.audiooutput`).

## MoltNet: media playback clock

`<audio>` and `<video>` play their sound through the output device, and in
Chromium the device also keeps their time. In MoltNet windows that time now
comes from a clock of MoltNet's own, the same on every machine:

| What a page reads | Before, in MoltNet | Now |
|---|---|---|
| `currentTime` while an element with sound plays, and when it starts moving after `play()` | when the device asks for audio (its buffer and callback pattern), the delay it reports with each callback (its latency, 100 ms and more with Bluetooth), and its clock (a few hundred parts per million off the system's, visible over minutes) | MoltNet's media clock: the system's monotonic clock, the uniform device (48 kHz, stereo, 480 frames) and a fixed delay ([below](#what-moltnet-does-a-media-clock)) |
| When a video's frames are shown, so `requestVideoFrameCallback()`'s `mediaTime`, `expectedDisplayTime` and `presentedFrames`, and what `drawImage(video)` copies at a given moment | scheduled from that audio time | scheduled from the clock's time |
| The buffers AudioRendererImpl and the element's mixer choose | from the device's rate and buffer | from the uniform device |
| A machine with no output device | playback times itself on a fallback sink with other buffers | **like every other machine** |
| The output device going away or stalling mid-play (unplugged headphones, a device change) | an audio renderer error, or time standing still while the device sends no callbacks | **nothing**: time goes on, the sound goes nowhere until a device opens again |
| `setSinkId("")` on an element without a player | the system's answer for its default device, which can be `NotFoundError` on a machine without one | **resolves** on every machine |

Normal and Incognito windows keep Chromium's behaviour. MoltNet ignores
Shields off, as elsewhere, and the kill switch turns it off.

### Where 156 gets the time

Every media element of a frame plays through one `AudioRendererMixer` per
output device and stream format (`AudioRendererMixerManager`), whose sink
is the frame's `AudioOutputDevice` (`AudioDeviceFactory::NewMixableSink`); a
MediaStream track played in an element (`TrackAudioRenderer`) gets an
output device of its own (`NewAudioRendererSink`). `AudioRendererImpl`
asks the sink for the device's parameters to choose its buffers, renders
when the device asks for audio, and keeps `currentTime` with an
`AudioClock` fed by the delay the device reports, interpolated on the
system clock between callbacks. A video's frames are shown when that
audio time says (`GetWallClockTimes`, what `VideoRendererImpl` and the
compositor follow). When the device's status isn't OK the renderer falls
back to a `NullAudioSink`, which renders on a `FakeAudioWorker` with its
own buffers; a device error during playback reaches the renderer as an
audio renderer error.

### What MoltNet does: a media clock

`AudioDeviceFactory` (one hook in `NewMixableSink`'s sink callback and one
in `NewAudioRendererSink` for MediaStream tracks) gives media elements'
mixers and played tracks a `MoltNetMediaAudioSink` in MoltNet renderers,
wrapping the output device Chromium would have used:

* **Device info.** For the default output device (`""` or `"default"`)
  it answers the uniform device: 48 kHz, stereo, a 480-frame buffer,
  status OK, whether or not the machine has a device, without asking one.
  The mixer and AudioRendererImpl choose their rates and buffers from
  that, as on every MoltNet machine of the same OS (the mixer renders
  1024 frames at a time at 48 kHz, 960 on Windows).
* **The clock.** It renders its client (the mixer, or the track's
  renderer) one buffer every buffer's duration from `Start()` or `Play()`,
  on a realtime thread MoltNet's media sinks share, measured on the
  system's monotonic clock. The clock is `media::FakeAudioWorker`, as for
  Web Audio's clock ([When Web Audio renders](#when-web-audio-renders)): a
  late tick runs once, never a burst of catch-up. Every tick passes the
  delay Chromium's own fake sinks report (two buffers,
  `FakeAudioWorker::ComputeFakeOutputDelay`, as `NullAudioSink` for a
  muted element) and no glitches. So `currentTime`, and the video frames
  that follow it, run on the same schedule on every machine.
* **The real device.** What the clock renders goes through
  `MoltNetAudioOutput`, the ring, drift control and resampler Web Audio's
  clock uses (now shared), to the real device, opened at its own rate
  with the buffer Chromium gives media playback. Nothing it passes back
  (its buffer, delay, timing, glitches) reaches the client.
* **No device, a lost device.** The default device's failures stay
  inside: rendering goes on, its output goes nowhere, and a new device is
  opened after 1, 2, 4 ... 30 s. `setSinkId("")` on an element that has no
  player yet resolves in MoltNet without looking for a device
  (`HTMLMediaElementAudioOutputDevice`).
* **A device the page chose by its ID** (which takes a media permission,
  [below](#moltnet-cameras-microphones-and-outputs)) is timed by the clock
  too, but answers for itself: its status, and its errors.
* **What already followed no device.** A muted element (muted autoplay)
  renders on AudioRendererImpl's own `NullAudioSink`, whose buffers now
  follow the uniform device; a video without sound is timed by the
  system's clock (`WallClockTimeSource`). Pause, playback rate (with its
  pitch-preserving stretch), seeking and volume work as before, on the
  clock. Web Audio has its own clock, which an element routed through
  `createMediaElementSource()` follows; WebRTC's output is off in MoltNet.

**What a page reads now** (checked against simulated devices of 128, 256,
441 (44.1 kHz), 480, 512, 1024 and 2048 (96 kHz) frames and a
Bluetooth-like one with 180 ms of latency, clocks up to 200 ppm off, no
device, a device lost and back and a device that stalls, see
Verification): `currentTime` read every millisecond and the display times
of the video frames around it at every 60 Hz frame are the same for every
device, through pause, rate changes, seeks, volume changes, muted
autoplay unmuted, streams of 22.05 to 96 kHz and a device the page chose;
a played MediaStream track is rendered at the same moments on every
device. Outside MoltNet the same harness tells the devices apart. What
remains is how the operating system schedules the clock's thread and how
long decoding and rendering take, which a page can time in many other
ways.

### What it costs

* **Latency.** The ring fills to one of the device's callbacks (a 21 ms
  playback buffer) and a cushion before the device hears anything, and
  plays what it started with rather than skip it, then works the excess
  off within 0.2% of the rate. Against Chromium on the same device, sound
  starts about 60 ms later, 45 ms later after 10 s, and 13 to 35 ms later
  once the ring has settled after 20 to 30 s (simulated).
* **Picture and sound.** A video's frames follow the clock's fixed delay,
  not the device's, so for the first seconds the sound trails the picture
  by about 25 to 37 ms on a wired output with 5 to 15 ms of latency of its
  own, about 15 ms at 10 s, and within about 12 ms either way after 20 s
  (simulated). An output with a long latency of its own (Bluetooth
  headphones, 150 to 250 ms) is not made up for: Chromium delays the
  picture by the latency the system reports, which would show in the
  timing, so in MoltNet the sound trails the picture by about that much.
* **CPU and memory.** A wakeup per mixer buffer on the clock's thread, the
  resampler on devices that don't run at 48 kHz (the Web Audio figures
  above), and the ring.

### Which contexts

The whole MoltNet renderer process: every media element and played
MediaStream track in it (media elements only exist in windows). The kill
switch turns it off.

### Code

| Where | What |
|---|---|
| `core/molt_fingerprinting/moltnet_audio_sink.{h,cc}` | `MoltNetMediaAudioSink`: the uniform device info, the shared clock thread, the real device and its failures, `setSinkId("")` without a player (+ `moltnet_audio_sink_test.cc`) |
| `core/molt_fingerprinting/moltnet_audio_output.{h,cc}` | `MoltNetAudioOutput`: the ring, drift control and resampler to the real device, shared with Web Audio's `MoltNetAudioDevice` |

Upstream hooks (each marked `MoltBrowser:`):
`modules/media/audio/audio_device_factory.cc` (`NewAudioRendererSink`
for MediaStream tracks; `NewMixableSink`'s sink callback for media
elements; `NewMoltNetOutputDevice` makes the real device for each
attempt) and `modules/audio_output_devices/
html_media_element_audio_output_device.cc` (`DoSetSinkId` without a
player). Both files are new in `src/`.

### Verification

Standalone (no Chromium checkout): a harness compiles MoltBrowser's real
`moltnet_audio_sink.cc`, `moltnet_audio_output.cc`,
`moltnet_audio_clock.cc`, `moltnet_media.cc` and the hooked
`audio_device_factory.cc` with Chromium 156's `AudioRendererMixerManager`,
`AudioRendererMixerInput`, `AudioRendererMixer`, `AudioConverter`,
`LoopbackAudioConverter`, `AudioClock`, `AudioLatency`, `FakeAudioWorker`
and `NullAudioSink` unchanged, and AudioRendererImpl's `Render`,
`CurrentMediaTime`, `GetWallClockTimes`, `SetPlaybackRate`, `SetVolume`,
`SetMediaTime` and the methods that start and stop its sinks extracted
verbatim, against stand-ins of the rest, on simulated time with the
simulated devices above (13 tests, g++ ASan/UBSan and clang
`-fno-exceptions` with UBSan traps): the timing above, identical for every
device in MoltNet and telling them apart in a normal window; no device
(no fallback, no error, as with one; Chromium falls back); a lost device
reopened and a stalled one, without an error or a change in timing; a
chosen device's status and errors; one realtime clock thread; the
MediaStream track sink; `setSinkId("")` without a player; and the
latency and picture-and-sound figures above. 9 mutants of the sink and
its hooks each fail it. Web Audio's own harness passes with the ring
moved into `MoltNetAudioOutput`. Every changed C++ file, the tests
included, compiles without errors against Chromium 156's real headers
(`tools/compile_check`).

**Needs the build machine:** compiling and linking, `blink_unittests`
(`MoltNetAudioSinkTest.*`, `MoltNetAudioDeviceTest.*` and the upstream
media and audio output device suites), and the checks in
`docs/BUILD_MACHINE_CHECKLIST.md` tagged "fp-mediaclock" (playback, A/V
sync and device changes on real 44.1, 48 and 96 kHz devices, Bluetooth,
no output device). **Automated** by [`tools/leak_test`](LEAK_TESTING.md):
an `<audio>` element's clock running when it plays unmuted
(`media.clock_running`), `setSinkId("")` without a player
(`media.sink_default`), and, for the record, how long after `play()` its
time starts moving (`media.clock_start_ms`).

## MoltNet: cameras, microphones and outputs

Before a page may use a camera or microphone, Chromium tells it which
kinds of device the machine has, and answers some `getUserMedia()` calls
without asking the user. In MoltNet windows every machine now has one
microphone, one camera and one output device until the user lets a page
use them:

| What a page reads | Before, in MoltNet | Now |
|---|---|---|
| `enumerateDevices()` before a permission | one entry, without ID, label or group, for each kind the machine has: no `videoinput` on a desktop without a camera, no `audioinput` without a microphone (no `audiooutput` without an output was already fixed, [above](#when-web-audio-renders)) | **`audioinput`, `videoinput`, `audiooutput`**, one each, without ID, label or group, on every machine |
| `getUserMedia()` for a kind the machine has none of | `NotFoundError` at once, no prompt | **the permission prompt** of any request; then `NotAllowedError` if the user blocks or dismisses it, `NotFoundError` if they allow it |
| `getUserMedia()` with constraints no device meets (an exact size the camera doesn't have) | `OverconstrainedError` at once, no prompt | the same: **the prompt** first |
| `devicechange` | fires when devices change, before any permission unless Chromium's `DeviceChangeRequiresPermission` is on | fires only once the page may use the devices |

Normal and Incognito windows keep Chromium's behaviour. MoltNet ignores
Shields off, and the kill switch turns it off.

### What MoltNet does

* **The list.** `MediaDevices::DevicesEnumerated` adds, after each kind,
  one entry without ID, label or group when Chromium listed none of that
  kind and the page doesn't see that kind's devices. Microphone
  permission shows a page both the microphones and the outputs (Chromium
  ties outputs to it), so an ID of either audio kind counts for both. The
  order is Chromium's (microphones, cameras, outputs), and an added
  input entry's `getCapabilities()` is empty, as for every entry
  without a label. Once the user grants a kind, its real devices are
  listed as in Chromium.
* **`getUserMedia()`.** When a request for a camera or microphone would
  fail because no device of a kind it asks for exists (`NO_HARDWARE`) or
  none meets its constraints (`CONSTRAINT_NOT_SATISFIED`),
  `UserMediaProcessor::GetUserMediaRequestFailed` first asks for the
  permissions the request needs (`PermissionService.RequestPermissions`,
  the request a page makes for a device that exists, with its prompt; a
  permission already granted or blocked is answered without one). Then:
  all granted, the real error (the page may see the devices now anyway);
  any blocked, `NotAllowedError` (`PERMISSION_DENIED`); dismissed or
  ignored, `NotAllowedError` (`PERMISSION_DISMISSED`), as a request a
  device could meet ends. Before an answer, the two machines look the
  same: a prompt, and a promise that waits for it. Screen capture and
  other failures are unchanged.
* **`devicechange`** fires only for a page that may use the devices
  (Chromium's own rule behind `DeviceChangeRequiresPermission`, kept in
  MoltNet whatever the feature's state), so plugging in headphones or a
  camera shows no page that may not use them.

**Why a prompt.** The two ways out were a prompt for every request, or
a fake device. A fake device would have to keep up the act after the user
allows it (a stream of what?), and the user would have allowed a device
that isn't there. Asking first keeps the machines alike until the user
decides, and after that the page may see the devices anyway.

**What it costs.** A user without a camera (or microphone) is asked
about one when a page wants it, and if they allow it the page then finds
none. Pages see a `devicechange` only once they may use the devices.

### Which contexts

Windows and their frames, the only contexts these APIs exist in, in the
whole MoltNet renderer process. The kill switch turns it off.

### Code

| Where | What |
|---|---|
| `core/molt_fingerprinting/moltnet_media_values.{h,cc}` | the list rule (`ListsUniformDevice`) and what a failed `getUserMedia()` reports after asking (`UserMediaFailureVerdict`); std-only (+ `moltnet_media_values_unittest.cc`) |
| `core/molt_fingerprinting/moltnet_media_devices.{h,cc}` | the Blink glue: the added entries, `devicechange`, the permission request (+ `moltnet_media_devices_test.cc`) |

Upstream hooks (each marked `MoltBrowser:`):
`modules/mediastream/media_devices.cc` (`DevicesEnumerated`: an entry per
kind; `MaybeFireDeviceChangeEvent`) and
`modules/mediastream/user_media_processor.cc`
(`GetUserMediaRequestFailed`). `user_media_processor.cc` is new in
`src/`.

### Verification

Standalone (no Chromium checkout): `moltnet_media_values_unittest.cc` (14
tests, the list rule and the verdict for every mix of answers included)
passes with g++ (ASan/UBSan) and clang (UBSan trap, `-fno-exceptions`).
Every changed C++ file, the tests included, compiles without errors
against Chromium 156's real headers (`tools/compile_check`).

**Needs the build machine:** `blink_unittests`
(`MoltNetMediaDevicesTest.*`, which runs the permission request against a
fake permission service in a real window, and the upstream
`MediaDevices` and `UserMediaProcessor` suites), and the checks in
`docs/BUILD_MACHINE_CHECKLIST.md` tagged "fp-mediaclock" (machines with
no camera or no microphone, the prompt and its three answers,
`devicechange`). **Automated** by [`tools/leak_test`](LEAK_TESTING.md):
the device kinds, their order and blank fields (`media.devices`,
`media.device_order`, `media.device_blank`) and `getUserMedia()` before a
permission, for a camera and microphone and with constraints no camera
meets (`media.gum`, `media.gum_overconstrained`: still waiting after
1.5 s, or refused).

## MoltNet: performance.memory

`performance.memory` (windows only, non-standard) reports the JavaScript
heap's size and limit; `performance.measureUserAgentSpecificMemory()`
(cross-origin isolated windows, shared and service workers) breaks memory
use down by context. Beneath both, V8 configures its heap from the
machine's memory, and on Linux the renderer's data limit follows it too:
a page that allocates until it runs out of memory, or watches when the
garbage collector runs, measures the machine. In MoltNet windows the
machine no longer shows in any of them:

| What a page reads or measures | Chromium 156 | MoltNet |
|---|---|---|
| How much JavaScript heap a page can fill before it runs out of memory, and when V8 collects garbage | V8's configuration for the machine's memory, in every isolate (windows, workers, worklets): an old generation of half the memory (256 MiB to 4 GiB), new-space areas of a 128th of it (2 to 32 MiB), and the heap's growth and initial size following the memory | **every isolate is configured as on a 4 GB machine**: an old generation of 2 GiB, new-space areas of 32 MiB, the 4 GB machine's growth and limits |
| `jsHeapSizeLimit` | that configuration's limit, to the byte: 2,248,146,944 on a 4 GB machine, 4,395,630,592 from 8 GB on, and every value between for the sizes between | **2,330,000,000**: the 4 GB machine's limit, which V8 really has, rounded as below |
| `usedJSHeapSize`, `totalJSHeapSize` | to the byte, refreshed every 50 ms (a renderer locked to a site, as on desktop) | rounded up to one of 100 sizes from 10 MB to 3.76 GB (three significant digits), refreshed once every 20 minutes |
| `measureUserAgentSpecificMemory()` | byte counts per context, as precise as the heap's layout; which entries a context gets depends on the process model (a frame in this process that wasn't measured has none, a frame in another process an entry of 0 bytes, a context without canvas buffers no canvas entry) | every count rounded the same way (0 stays 0); **every context gets the same entries**, its JavaScript and its canvas memory (0 bytes where nothing was measured), and so do the shared and Blink memory |
| On Linux, how much a renderer can reserve (`ArrayBuffer`s, WebAssembly memories, untouched) before an allocation fails | the renderer's data limit: 8 GiB on machines up to 16 GB, 16 or 32 GiB above (`kHigherRendererMemoryLimit` doubles them) | **8 GiB** on every machine |

Normal and Incognito windows keep Chromium's behaviour. The kill switch
turns it off.

### What MoltNet does

* **V8's heap.** gin configures every isolate of a MoltNet renderer as
  for a machine of `moltnet::kV8HeapPhysicalMemory`, 4 GiB
  (`gin::IsolateHolder::SetPhysicalMemoryForIsolates()`, called before V8
  starts): V8 sizes the generations, its growth heuristics and the
  initial heap from that number, whatever the machine has. 4 GB is the
  smallest machine MoltBrowser supports (the README's Basic tier), so the
  configuration never promises more than the smallest machine can back;
  V8's sizes are limits, not reservations, so larger machines lose
  nothing but the extra headroom.
* `MemoryInfo` always reports the rounded, cached values Chromium reports
  when a renderer isn't locked to a site (`Precision::kBucketized`, the
  20-minute cache), whatever the caller asked for and whatever
  `--enable-precise-memory-info` says.
* The used and total sizes are the page's own, rounded with a fixed table
  of Chromium's 100 sizes (`moltnet::QuantizedMemorySize`): Chromium
  computes the sizes in floating point at run time with the platform's
  math library, MoltNet uses the same values on every machine and thread.
* `jsHeapSizeLimit` is the limit V8 156 computes for the 4 GiB machine (an
  old generation of 2 GiB and three new-space areas of 32 MiB, 2 GiB +
  96 MiB, `moltnet::V8HeapSizeLimit`), rounded like the other two:
  2,330,000,000.
* `measureUserAgentSpecificMemory()` gives every entry of the browser's
  measurement a JavaScript and a canvas size, and the measurement a shared
  and a Blink size, 0 where there was none
  (`molt_fingerprinting::MakeMemoryMeasurementUniform`), then rounds every
  byte count the same way, before the totals are added up.
* **The data limit (Linux).** The sandbox sets a MoltNet renderer's
  `RLIMIT_DATA` to 8 GiB on every machine
  (`sandbox::policy::molt::ProcessDataSizeLimit`), the limit Chromium
  gives machines up to 16 GB. Reserving memory without touching it
  reaches the limit in a fraction of a second, so the limit was a cheap
  measurement of the machine.

### Which contexts

The whole MoltNet renderer process: every isolate (windows, iframes,
dedicated, shared and service workers, worklets) and, on Linux, the
process's data limit; `performance.memory` exists only in windows, the
memory measurement also in shared and service workers. The kill switch
turns it off.

### What it costs MoltNet users

A page can fill 2 GiB of JavaScript heap instead of up to 4 GiB on a
machine with 8 GB or more before its tab runs out of memory (very few
pages come near either), and V8 collects garbage as it does on a 4 GB
machine: a little more often on large machines, where Chromium lets the
heap grow further between collections. On Linux machines with more than
16 GB, a MoltNet renderer can reserve 8 GiB instead of 16 or 32 GiB.

### Code

| Where | What |
|---|---|
| `core/molt_fingerprinting/moltnet_memory_values.{h,cc}` | the 4 GiB machine, V8's heap limit for a machine's memory, the uniform limit, the fixed table of sizes; std-only (+ `moltnet_memory_values_unittest.cc`, in `:unit_tests`) |
| `core/molt_fingerprinting/moltnet_memory.{h,cc}` | the Blink glue (+ `moltnet_memory_test.cc`) |
| `core/molt_fingerprinting/moltnet_cpu.{h,cc}` | `PinUniformMachineBeforeV8()`, which configures gin before V8 starts (+ `moltnet_cpu_test.cc`: the heap model is V8's, gin configures the fixed machine) |
| `sandbox/policy/linux/molt_data_limit.h` | the data limit (+ `chrome/common/molt_ai/fingerprinting/molt_data_limit_unittest.cc`) |

Upstream hooks (each marked `MoltBrowser:`): `core/timing/memory_info.cc`
(`MemoryInfo`'s constructor and `HeapSizeCache::Update`),
`core/timing/measure_memory/measure_memory_controller.cc`
(`MeasurementComplete`, `ConvertResult`),
`gin/public/isolate_holder.h` and `gin/isolate_holder.cc`
(`SetPhysicalMemoryForIsolates`, used by `getDefaultIsolateParams`, which
every isolate's parameters come from), `sandbox/policy/linux/sandbox_linux.cc`
(`LimitAddressSpace`) and `sandbox/policy/BUILD.gn`. All are new in `src/`.
The call comes from `PinUniformMachineBeforeV8()` ([CPU
features](#moltnet-cpu-features)).

### Verification

Standalone (no Chromium checkout): `moltnet_memory_values_unittest.cc` (4
tests: the uniform limit is the 4 GiB machine's, the limit V8 156's heap
configuration gives 1 to 64 GB, fractional and nonsense memory sizes, and
the table's rounding at, between and beyond its sizes, Chromium's own test
values included; the table was generated with Chromium's computation)
passes with g++ (ASan/UBSan) and clang (UBSan trap, `-fno-exceptions`);
`molt_data_limit_unittest.cc` (3 tests: the switch's names agree with
chrome's, MoltNet renderers get 8 GiB whatever Chromium's tier and keep a
smaller limit, other renderers and processes keep Chromium's) with g++
(ASan/UBSan) against a `base::CommandLine` stand-in. A probe of the data
limit (node under `prlimit`, untouched `ArrayBuffer`s) reaches 1 to 8 GiB
limits in about 60 ms with 40 MB resident. Every changed C++ file, the
tests included, compiles without errors against Chromium 156's real
headers (`tools/compile_check`).

**Needs the build machine:** `blink_unittests` (`MoltNetMemoryTest.*`,
which reads `performance.memory` through a real `MemoryInfo` in a V8
context and checks the measurement's entries, `MoltNetCpuTest.*`, which
creates real isolates, and the upstream `MemoryInfo` and measure-memory
suites), `chrome`'s `molt_ai_unittests` (`MoltDataLimitTest.*`), and the
checks in `docs/BUILD_MACHINE_CHECKLIST.md` tagged "fp-mediaclock" and
"fp-cpu" (a 4 GB machine, a large one, workers). **Automated** by
[`tools/leak_test`](LEAK_TESTING.md): `jsHeapSizeLimit`
(`perf.jsHeapSizeLimit`), the sizes rounded (`perf.heap_rounded`) and not
refreshed after an allocation (`perf.heap_cached`).

## MoltNet: MediaRecorder

`MediaRecorder` encodes a stream (a camera, a canvas's `captureStream()`, a
`<video>`) into a file the page reads back. At 156 it asks the machine what
to encode with; in MoltNet windows every answer now comes from the build:

| What a page sees | Chromium 156 | MoltNet |
|---|---|---|
| The codec recorded when the page names none (`mimeType` from the `start` event on) | a codec the GPU can encode, when it has one; else VP8 (VP9 for MP4) | **VP8** for WebM and Matroska, **VP9** for MP4 |
| `isTypeSupported()` and the constructor, for H.264, AV1 and HEVC profiles and AAC | the build's encoders plus the GPU's (`media::IsEncoderSupportedVideoType`) and the platform's AAC encoder | **the build's own encoders** (`media::IsEncoderBuiltInVideoType`); AAC unsupported |
| The encoder that writes the recording (its bitstream, rate control and speed) | the GPU's encoder (on macOS also the system's H.264 and HEVC encoders) when it has the codec and the frames are large enough, else the build's | **the build's**: libvpx (VP8, VP9) or libaom (AV1) |
| The software encoders' threads, and AV1's tile layout, which is written into the stream | sized by the machine's cores | sized by **4** cores, the count `navigator.hardwareConcurrency` reports |

Audio is recorded as Opus (libopus) or PCM, by the build, on every
machine, and in MoltNet libopus and the resampler use the same kernels on
every CPU ([pixels and encoded bytes](#moltnet-pixels-and-encoded-bytes)).
A stream whose frames arrive already encoded (a remote WebRTC track) is
recorded as it is; WebRTC is off in MoltNet.

The core count is set for the whole MoltNet renderer process
(`media::SetProcessorCountForSoftwareEncoding`), so WebCodecs' software
encoders, which MoltNet already limits to the build's codecs
([WebCodecs](#media-capabilities-codec-support-and-webcodecs)), size
their threads the same way.

Normal and Incognito windows keep Chromium's behaviour (see *Encoder
identity* in [Known gaps](#known-gaps-and-attacks)). MoltNet ignores
Shields off, and the kill switch turns all of it off.

**What it costs MoltNet users.** Recording runs on the CPU, also where the
GPU could encode: long or large recordings (screen recorders, video
messages) use more CPU and battery. A page that asks for a type only a
platform encoder handles (H.264 or HEVC in MP4, AAC) gets `false` from
`isTypeSupported()` or `NotSupportedError`, as on a machine without that
encoder, and picks another type. On machines with more than 4 cores, large
software encodes use fewer threads than they could.

**Code.** `core/molt_fingerprinting/moltnet_encoders.{h,cc}` (+
`moltnet_encoders_test.cc`). Upstream hooks (each marked `MoltBrowser:`;
all four files are new in `src/`):
`modules/mediarecorder/video_track_recorder.cc`
(`VideoTrackRecorderImpl::NotifyEncoderSupportKnown` and
`GetVEASupportedProfiles`, which `GetPreferredCodec`,
`CanUseAcceleratedEncoder` and the encoder's creation read),
`modules/mediarecorder/media_recorder_handler.cc`
(`CanSupportMimeTypeForCodec`), and `media/base/video_encoder.{h,cc}`
(`GetNumberOfThreadsForSoftwareEncoding`, and the new
`SetProcessorCountForSoftwareEncoding`, called from `Configure`).

**Verification.** Standalone (no Chromium checkout): a harness runs the
real `moltnet_encoders.cc` with the hooked code extracted verbatim from
`video_track_recorder.cc`, `media_recorder_handler.cc` and
`video_encoder.cc`, against stand-ins of the types they touch, in kOff and
real kUniform, built once with MoltBrowser's build flags and once with
every encoder compiled in (4 tests: in MoltNet the GPU is never asked,
and a machine without hardware encoders and one with many get the same
default codec for each container, encoder choice at three frame sizes and
`isTypeSupported()` answer for every VP8, VP9, AV1, H.264, HEVC, Opus, PCM
and AAC type, with threads for 4 cores; outside MoltNet the two machines
differ; the testing switch); 9 mutants of the glue and hooks each fail
it. Every changed C++ file compiles without errors against Chromium 156's
real headers (`tools/compile_check`, which now mounts libvpx and libaom).
**Needs the build machine:** compiling and linking, `blink_unittests`
(`MoltNetEncodersTest.*`), and the checks in
`docs/BUILD_MACHINE_CHECKLIST.md` tagged "fp-probes" (a recording on a
machine with hardware encoders). **Automated** by
[`tools/leak_test`](LEAK_TESTING.md): the recorded type for each container,
and `isTypeSupported()` compared between runs.

## MoltNet: pixels and encoded bytes

A page can read back the media it decodes and encodes: a video frame's
planes and its RGB pixels (`VideoFrame.copyTo()`, `new VideoFrame(video)`),
a `<video>` drawn into a canvas or uploaded to WebGL, and the bytes that
`VideoEncoder`, `AudioEncoder` and `MediaRecorder` write. At 156 those
bytes depend on the machine in two ways: on the GPU and its video decoder,
for video a media element plays, and on the CPU's vector instructions, for
the code that scales, resamples and encodes. In MoltNet windows they are
computed the same way on every machine of an OS:

| What a page reads | Chromium 156 | MoltNet |
|---|---|---|
| `<video>` (and MSE) frames | decoded by the GPU's decoder where Chromium uses one, and kept in GPU memory (NV12); on some platforms software-decoded frames are copied there too | decoded by **the build's software decoders** (libvpx for VP8 and VP9, dav1d for AV1) and kept **in CPU memory** as decoded (I420) |
| A video frame as RGB: `copyTo({format: 'RGBA'})`, `drawImage()`, `texImage2D()` | converted by the GPU (frames in GPU memory, accelerated canvases, WebGL uploads) or by libyuv | converted by **libyuv on the CPU** |
| A `VideoEncoder` frame of another size than the encoder's | scaled by libyuv's box or bilinear filter, whose rounding differs between x86 and Arm | scaled by **an integer box (shrinking) and tent (growing) filter** that rounds the same way on every CPU (`media/base/molt_portable_scale.h`) |
| Opus (`AudioEncoder`, `MediaRecorder`) | libopus with the CPU's SSE, AVX2 or NEON kernels; on x86 its pulse search uses approximate-reciprocal instructions, whose results differ between CPU vendors | **libopus's C kernels** |
| Audio an encoder resamples (any rate but 8, 12, 16, 24 and 48 kHz) or downmixes | `SincResampler` and `vector_math::FMAC` with AVX2 and FMA, SSE or NEON | **their C versions** |

The libopus row was measured with Chromium 156's libopus built for x86-64
and arm64 with Chromium's flags: over 300 random encoder configurations,
the C kernels give other bytes than Chromium's x86-64 build in 257 and than
its arm64 build in 31, and Chromium's x86-64 build gives other bytes on an
emulated x86 CPU than on an Intel one in 7 of 8 typical WebCodecs and
MediaRecorder configurations. With the C kernels the bytes are the same on
x86-64 (natively, and on emulated Nehalem and Core 2 CPUs) and on arm64
(emulated Cortex-A72 and qemu's `max` CPU) in all 300, and a libm whose
results are one unit in the last place off changes none of them.
`SincResampler`'s SSE and AVX2 convolutions give other samples than its C
one for three quarters of the output.

**What was already the same.** Measured with Chromium 156's copies, built
for x86-64 and arm64 with Chromium's flags:

* libyuv: of 97 cases covering the calls the read-back and encoder-input
  paths make, every conversion (YUV to RGB and back with the BT.601,
  BT.709 and BT.2020 matrices, full and limited range, with nearest and
  bilinear chroma, NV12, 10- and 12-bit, with and without alpha) gives the
  same bytes at every SIMD level of x86-64 (C, SSE2 to AVX-512) and of
  arm64 (C, NEON, SVE2, SME), and the same on both architectures; so does
  nearest-neighbour scaling. Only filtered scaling
  differed: between x86 and Arm (libyuv's own C code rounds per
  architecture, to match each one's vector code), and on x86 at one ratio
  (3/8) between SSSE3 and the rest. MoltNet therefore doesn't restrict
  libyuv's CPU features (`libyuv::MaskCpuFlags`): it would change nothing on
  one architecture and not make the two agree.
* libvpx: VP8 and VP9 encoding at Chromium's real-time settings (several
  sizes and thread counts) and decoding give the same bytes at every SIMD
  level of x86-64 and on arm64.

Video drawn into a canvas is read back through the canvas, whose
readbacks are farbled like any drawing. A frame read back in another
colour space (`copyTo({colorSpace: 'display-p3'})`) is converted by
Skia's portable raster pipeline ([CPU features](#moltnet-cpu-features)).

**Which contexts.** The whole MoltNet renderer process, windows, iframes
and workers, set once when it starts (`Configure`, before any media object
exists). Normal and Incognito windows are unchanged, and so are the GPU
process and other renderers. The kill switch turns it off.

**What it costs MoltNet users.** Measured on one core of this VM (Xeon,
2.1 GHz), with FFmpeg and the system's libvpx and dav1d, on busy synthetic
clips at 30 frames a second:

| Clip | VP9 (libvpx) | AV1 (dav1d) |
|---|---|---|
| 1080p (VP9 4 Mbit/s, AV1 3 Mbit/s) | 0.15 cores | 0.09 cores |
| 2160p (VP9 16 Mbit/s, AV1 10 Mbit/s) | 0.68 cores | 0.41 cores |

With four threads a decoder uses a little more CPU in total (1080p 0.19 and
0.14 cores, 2160p 0.75 and 0.61) and runs 3 to 15 times faster than real
time. A GPU decodes the same video for a fraction of a watt; at a typical
laptop core's few watts when busy, software decoding should cost roughly
0.5 to 1 W more at 1080p and 2 to 3 W more at 2160p, which shortens battery
life while video plays by very roughly a tenth to a third (an estimate from
the CPU time, not measured on a battery here). 4K at 60 frames a second
may drop frames on slow machines (`getVideoPlaybackQuality()` shows it,
and Media Capabilities already answers "not power efficient" for every
video in MoltNet). Decoded frames are uploaded for display (3 MB per 1080p
frame, 12 MB at 2160p) instead of staying on the GPU. The rest is small:
reading a frame as RGB costs libyuv 0.5 ms at 1080p and 1.8 ms at 2160p;
the integer scaler takes 9 ms for 1080p to 720p, 14 ms for 2160p to
1080p and 6 ms for 720p to 1080p (I420; libyuv's filters 0.6 to 3 ms), so
a page encoding 30 frames a second at another size than its frames uses up
to half a core more; Opus with its C kernels takes 1.0 to 1.4 times as
long (about 1% of a core for real-time stereo); the C resampler 2 ms per
second of 48 kHz stereo (0.5 ms with AVX2).

**Code.** `media/base/molt_portable_media.{h,cc}` (the process's portable
mode; `PortableScaleFrame()`) and `media/base/molt_portable_scale.{h,cc}`
(the scaler; std-only), with `molt_portable_media_unittest.cc` and
`molt_portable_scale_unittest.cc` in `media_unittests`;
`core/molt_fingerprinting/moltnet_pixels.{h,cc}` (`PinUniformMediaPixels()`,
called from `Configure`; + `moltnet_pixels_test.cc`). Upstream hooks (each
marked `MoltBrowser:`; all new in `src/`):
`media/renderers/default_decoder_factory.cc` (`CreateVideoDecoders`: no
external, that is hardware, decoders),
`media/gfx/mappable_shared_image_video_frame_pool.cc`
(`MaybeCreateHardwareFrame`: frames passed through),
`media/gfx/paint_canvas_video_renderer.cc` (`VideoImageGenerator::QueryYUVA`
and `IsPixelFormatSupportedForYuvSharedImageConversion`: no GPU
conversion), `media/base/video_frame_converter_internals.cc` (`ARGBScale`,
`NVxxScale`, `I4xxxScale`, `I4xxxScale_16`: filtered scaling),
`media/base/sinc_resampler.cc` (`InitializeCPUSpecificFeatures`),
`media/base/vector_math.cc` (`FMAC`), `media/base/BUILD.gn` (the new
files; `//third_party/opus`), and in `third_party/opus`: `BUILD.gn` (the
SSE and SSE2 kernels are chosen at run time instead of presumed, which
`opus_select_arch()` does on every x86-64 CPU, so other processes run the
same code as before: the same bytes in all 300 configurations, and no
measurable difference in speed), `src/celt/celt.c` (`opus_moltbrowser_set_portable()`),
`src/celt/x86/x86cpu.c` (`opus_select_arch()`: the C kernels), and
`src/celt/arm/pitch_neon_intr.c` and `celt_neon_intr.c` (the presumed NEON
kernels hand over to C).

**Verification.** Standalone (no Chromium checkout; harnesses in a scratch
directory): the libyuv comparison above (97 cases, two contents and two
sizes, on 15 CPU configurations: every `MaskCpuFlags` level natively from
AVX-512 down to C, emulated Haswell, Sandy Bridge, Nehalem and Core 2, and
emulated arm64 Cortex-A72, A76, Neoverse N2, qemu's `max` CPU with SVE2
and SME, and C); the libopus comparison above (Chromium 156's libopus with the
hooks, 300 random and 8 typical encoder configurations, also with glibc's
libm replaced by fdlibm and by one perturbed by a unit in the last place);
the libvpx comparison above; `molt_portable_scale_unittest.cc` (9 tests:
the weights, copies, constant planes, block means, interpolation, range,
channels, bad geometry and golden digests) with g++ (ASan/UBSan), clang
(UBSan trap, `-fno-exceptions`, the strict warning set) and clang for
arm64 under qemu, with the same digests; a libFuzzer run of the scaler with
random geometry and short buffers under ASan/UBSan. Every changed C++ file
compiles against Chromium 156's real headers (`tools/compile_check`, which
now mounts FP16 for `paint_canvas_video_renderer.cc`); the four libopus
files, which it doesn't cover, compile without warnings (`-Wall -Wextra
-Wmissing-prototypes`) for x86-64 and arm64 against 156's libopus, and are
the ones the comparisons above ran.
**Needs the build machine:** compiling and linking, `media_unittests`
(`MoltPortable*`), `blink_unittests` (`MoltNetPixelsTest.*`), and the
checks in `docs/BUILD_MACHINE_CHECKLIST.md` tagged "fp-pixels" (two
machines of an OS with different CPUs and GPUs, playback cost).
**Automated** by [`tools/leak_test`](LEAK_TESTING.md): a VP9 frame decoded
by WebCodecs and by a `<video>`, read as YUV and as RGBA, `<video>` frames
in I420 (`pixels.video_frame_format`), and the bytes of a scaled VP8 frame
(I420 and RGBA input) and of Opus at 48 and 44.1 kHz, compared between
runs (`pixels.*`).

## MoltNet: CPU features

Some of what a page computes depends on the CPU it runs on: WebAssembly's
relaxed SIMD instructions are defined to give implementation-specific
results, and the browser's own code picks a code path for the CPU's
vector extensions at run time (SSE4.1, AVX, AVX2, FMA, AVX-512 and VNNI
on x86-64, NEON on Arm) in colour conversion, audio decoding and two
`Math` functions. At 156 those results show which extensions the CPU has.
In MoltNet windows they are computed the same way on every CPU:

| What a page computes | Chromium 156 | MoltNet |
|---|---|---|
| WebAssembly relaxed SIMD (`relaxed_madd` and `nmadd`, `relaxed_swizzle`, `relaxed_trunc`, `relaxed_laneselect`, `relaxed_min` and `max`, `relaxed_q15mulr`, `relaxed_dot`) | always on (not behind a flag or a Blink feature): a fused multiply-add on CPUs with FMA, a separate multiply and add without; out-of-range lanes, NaN and rounding as x86 or Arm treat them; dot products with AVX-VNNI-INT8, AVX-VNNI or SSSE3 sequences | **off** (V8's new `--no-wasm-relaxed-simd`): a module that uses them fails validation (`WebAssembly.validate()` is false, compiling throws a `CompileError`) the same way on every machine, as on a browser without them; pages that detect it load the plain-SIMD build they ship for such browsers |
| `Math.pow` (and `**`), `Math.tanh` | the C library's `pow` and `tanh` (`--use-std-math-pow` is on by default, `tanh` always calls `std::tanh`), which pick their code by CPU feature: glibc has FMA versions it uses on CPUs with FMA, the Windows C runtime FMA3 versions | **V8's own code**, like every other `Math` function: llvm-libc's `pow` and fdlibm's `tanh` (the one V8 used before it called `std::tanh`) over V8's `expm1` |
| Colours a page reads back in another colour space (`getImageData({colorSpace})`, `VideoFrame.copyTo({colorSpace})`, Display P3 and Rec. 2020 images and video drawn into a canvas) and pixels Skia draws on the CPU | Skia's raster pipeline, blitters and swizzles in their SSE2, AVX2 + FMA or AVX-512 versions on x86-64 (the AVX2 and AVX-512 ones fuse multiply-adds; reciprocal estimates differ between Intel and AMD) and NEON on Arm (fused multiply-adds, its own estimates) | **one raster pipeline for every CPU** and no CPU-specific blitters: no fused multiply-add, exact division and square roots instead of estimates, exact half floats, SSE's rules for minimum, maximum, rounding and NaN everywhere; the same bytes on every x86-64 and arm64 CPU |
| Images with a colour profile, decoded by Blink or WebCodecs' `ImageDecoder` | skcms in its baseline, AVX2 or AVX-512 version (the baseline converts half floats approximately, the others and NEON exactly) | **skcms's baseline code with exact half floats**: the same bytes on every x86-64 and arm64 CPU |
| Decoded audio: WebCodecs `AudioDecoder` output (`AudioData`), and the samples `decodeAudioData()` and media elements decode | Symphonia (FLAC, MP3, PCM, Vorbis) computes Vorbis' inverse MDCT with rustfft's AVX + FMA, SSE4.1 or NEON code, whichever the CPU has; FFmpeg, which decodes what Symphonia declines, with its SSE, AVX, AVX2 + FMA or NEON code | **rustfft's scalar code and FFmpeg's C code**: the same samples on every x86-64 and arm64 CPU |

`AudioBuffer`s (`decodeAudioData()`, `OfflineAudioContext`) are also
farbled where a page reads them ([the noise](#the-noise)); `AudioData` and
`VideoFrame.copyTo()` aren't.

### What MoltNet does

All of it is set once per MoltNet renderer, before V8 starts and before
any image, frame or audio is decoded:
`ChromeContentRendererClient::SetRuntimeFeaturesDefaultsBeforeBlinkInitialization()`
calls `molt_ai::fingerprinting::ConfigureRendererProcessBeforeBlink()`,
which for a MoltNet renderer calls
`WebMoltFingerprinting::ConfigureBeforeBlinkInitialization()`, and
`PinUniformMachineBeforeV8()` (`core/molt_fingerprinting/moltnet_cpu.h`):

* gives V8 `--no-wasm-relaxed-simd --no-use-std-math-pow
  --no-use-std-math-tanh` (`v8::V8::SetFlagsFromString`, before gin
  initialises V8, so the flags hold for every isolate and are frozen with
  the others);
* configures every isolate for the 4 GB machine
  ([performance.memory](#moltnet-performancememory));
* turns on skcms's portable mode (`skcms_MoltBrowserEnablePortableMode()`:
  `skcms_DisableRuntimeCPUDetection()` plus exact half floats in the
  baseline code);
* installs Skia's portable raster pipeline
  (`SkGraphics::MoltBrowserUsePortableRasterPipeline()`, a translation unit
  of its own, `SkOpts_moltbrowser.cpp`, built from the same
  `SkRasterPipeline_opts.h` with `SKRP_MOLTBROWSER_PORTABLE` and
  `-ffp-contract=off`).

The browser adds `--disable-skia-runtime-opts` to MoltNet renderers'
command lines (`molt_fingerprinting_browser.cc`), so Skia's CPU-specific
blitters, bitmap samplers and swizzles are never installed either. The
media process mode of [pixels and encoded
bytes](#moltnet-pixels-and-encoded-bytes)
(`media::SetPortableMediaProcess()`) now also turns on Symphonia's
portable FFT (`SymphoniaAudioDecoder` passes it to the Rust bridge's
`set_portable_fft()` before it creates a decoder, which calls
symphonia-core's `moltbrowser_set_portable_fft()`) and gives FFmpeg no CPU
flags (`av_force_cpu_flags(0)`).

**What was already the same.** Measured with Chromium 156's code, built
with Chromium's compiler and flags for x86-64 and arm64:

* V8's other `Math` functions (llvm-libc and fdlibm, compiled without
  fused multiply-adds on x86-64): the same on every x86-64 CPU, and on
  every arm64 CPU.
* MP3, FLAC and PCM decoding, in Symphonia and in FFmpeg (integer
  arithmetic, or no FFT): the same samples on every CPU of both
  architectures. Opus uses libopus's C kernels since [pixels and encoded
  bytes](#moltnet-pixels-and-encoded-bytes).
* skcms's AVX2 and AVX-512 code already gave the same bytes as its arm64
  code; portable mode gives those bytes on every CPU.

**Camera and screen frames.** Frames from a camera or a screen capture,
which only exist after the user grants a permission, keep Chromium's path:
where the platform delivers them in GPU memory, a page's read-back is
converted on the GPU. Converting them on the CPU would add a readback and
an upload to every frame of a call or a screen share (3 MB at 1080p,
12 MB at 2160p).

### Which contexts

The whole MoltNet renderer process: windows, iframes, workers and
worklets, from before V8 starts. Normal and Incognito windows are
unchanged, and so are the GPU process (which rasterises accelerated
canvases and composites pages on the GPU) and other renderers. The kill
switch turns it off (no mode switch, no call).

### What it costs MoltNet users

Measured on one core of this VM (Xeon, 2.1 GHz, with AVX-512):

| Work | Chromium 156 (AVX-512 / AVX2) | MoltNet |
|---|---|---|
| Skia: reading back an sRGB canvas (premultiplied to unpremultiplied 8-bit) | 0.7 / 1.1 ms per megapixel | 2.4 ms |
| Skia: the same in Display P3, or a half-float P3 image to sRGB | 6 / 9 to 10 ms per megapixel | 26 to 30 ms |
| skcms: an image with a P3 profile decoded to sRGB (8-bit or half floats) | 6 / 10 ms per megapixel | 19 to 22 ms |
| Symphonia: 4 s of 48 kHz stereo Vorbis | 5.4 ms | 6.4 ms |
| FFmpeg (fallback): the same Vorbis; FLAC | 5.2 ms; 6.9 ms | 7.3 ms; 7.9 ms |

Skia's baseline code (what a CPU without AVX2 runs) takes 2.4 ms and 21 to
22 ms per megapixel for the first two rows: the portable pipeline costs up
to 36% more than that, for its exact divisions and conversions. A
12-megapixel photo with a P3 profile takes about 0.23 s of a decoding
thread instead of 0.07 to 0.12 s. Canvases Skia draws on the CPU (those
created with `willReadFrequently`, and every canvas where the GPU doesn't
rasterise) draw without AVX2 and AVX-512 blitters; accelerated canvases and
the page itself are drawn on the GPU, as before. Audio still decodes
hundreds of times faster than real time. WebAssembly code that would use
relaxed SIMD (some machine-learning kernels) runs its plain-SIMD version,
typically somewhat slower. `Math.pow` and `Math.tanh` cost about what the
C library's versions cost.

### Code

| Where | What |
|---|---|
| `core/molt_fingerprinting/moltnet_cpu.{h,cc}` | `kMoltNetV8Flags`, `PinUniformMachineBeforeV8()` (+ `moltnet_cpu_test.cc`) |
| `core/molt_fingerprinting/molt_fingerprinting.cc`, `public/web/web_molt_fingerprinting.h` | `WebMoltFingerprinting::ConfigureBeforeBlinkInitialization()` |
| `chrome/renderer/molt_ai/fingerprinting/molt_fingerprinting_renderer.{h,cc}` | `ConfigureRendererProcessBeforeBlink()`: the mode from the command line |
| `chrome/browser/molt_ai/fingerprinting/molt_fingerprinting_browser.cc` | `--disable-skia-runtime-opts` for MoltNet renderers |
| `third_party/skia/src/opts/SkOpts_moltbrowser.cpp` | the portable raster pipeline's translation unit |
| `media/base/molt_portable_media.{h,cc}` | the media process mode: also FFmpeg's CPU flags |

Upstream hooks (each marked `MoltBrowser:`; all new in `src/`):

* V8: `src/flags/flag-definitions.h` (`wasm_relaxed_simd`,
  `use_std_math_tanh`), `src/wasm/function-body-decoder-impl.h`
  (`DECODE(Simd)`: a relaxed opcode fails validation without the flag),
  `src/base/ieee754.{h,cc}` (`tanh`, `SetUseStdTanh`), `src/init/v8.cc`
  (`V8::Initialize`: applies `use_std_math_tanh`).
* `chrome/renderer/chrome_content_renderer_client.cc`
  (`SetRuntimeFeaturesDefaultsBeforeBlinkInitialization`).
* skcms: `skcms.cc`, `src/skcms_public.h`, `src/skcms_Transform.h`,
  `src/Transform_inl.h` (`F_from_Half`, `Half_from_F`: exact in portable
  mode).
* Skia: `include/core/SkGraphics.h`, `src/core/SkGraphics.cpp`
  (`MoltBrowserUsePortableRasterPipeline`), `src/opts/SkRasterPipeline_opts.h`
  (the `SKRP_MOLTBROWSER_PORTABLE` variants: NEON minimum and maximum,
  reciprocals, square roots and multiply-adds; SSE reciprocals, absolute
  value, floor and ceiling; half floats; NaN; the low-precision pipeline's
  minimum, maximum, division by 255 and floor), `skia/BUILD.gn`
  (`SkOpts_moltbrowser.cpp` in `skia_opts`).
* Symphonia: `third_party/rust/chromium_crates_io/vendor/symphonia-core-v0_6/src/dsp/fft/simd.rs`
  (`moltbrowser_set_portable_fft`, the planner every FFT is made with),
  with the same change as a crate patch in
  `third_party/rust/chromium_crates_io/patches/symphonia-core-v0_6/`, so
  vendoring the crate again keeps it; `media/filters/symphonia_decoder_bridge.rs`
  (`set_portable_fft`), `media/filters/symphonia_audio_decoder.cc`
  (`ConfigureDecoder`).

### Verification

Standalone (no Chromium checkout; harnesses in a scratch directory), with
Chromium 156's sources built with Chromium's clang and flags for x86-64
(`-msse3`, the AVX2 and AVX-512 files with theirs) and arm64, run natively
(a Xeon with AVX-512) and under qemu on emulated Core 2, Nehalem, Sandy
Bridge, Haswell and EPYC (Zen 2) CPUs and arm64 Cortex-A72, Neoverse N1 and
qemu's `max` CPU:

* **Relaxed SIMD.** A probe of all 20 relaxed instructions with inputs
  whose results the specification leaves open (Node 22's V8 12.4; 156's
  V8 picks its code for them the same way, by FMA, AVX-VNNI and SSSE3
  support): on x86-64 the multiply-adds differ between CPUs with
  and without FMA and a dot product between Nehalem and later CPUs, and
  arm64 differs from x86-64 in 13 of the 20. The decoder change needs the
  build machine.
* **`Math`.** V8 156's `src/base/ieee754.cc` with 21 functions over a
  million arguments each: with MoltNet's flags, `pow` and `tanh` give the
  same results on every CPU above, x86-64 and arm64; `std::pow` and
  `std::tanh` (glibc 2.39), which Chromium calls, differ between the CPUs
  with FMA and those without. fdlibm's `tanh` stays within 2.4 units in
  the last place (glibc's: 2.2).
* **Skia.** The raster pipeline and `SkConvertPixels` from Chromium 156's
  Skia over 3075 cases (6 colour types, 7 colour spaces to sRGB, P3 and
  Rec. 2020, premultiplied and not; the 22 blend modes; HSL, a colour
  matrix, gamma, PQ and HLG curves): Chromium's code gives 5 different
  results on x86-64 (its baseline, AVX2 and AVX-512 code, the first two
  also differing between the Xeon and the emulated CPUs, whose reciprocal
  estimates differ) and another on arm64; the portable pipeline the same
  bytes on every CPU above.
* **skcms.** 1890 transforms (7 source profiles, a table and CLUT one
  among them, 3 destinations, 6 source and 5 destination formats, 3 alpha
  modes): the baseline code gives other bytes than the AVX2, AVX-512 and
  arm64 code; portable mode the same bytes on every CPU above, the ones
  AVX2, AVX-512 and arm64 give. Its half-float conversion equals F16C's for
  all 65,536 halves and all 2^32 floats.
* **Audio.** Symphonia 0.6.1 as Chromium 156 vendors and configures it
  (cargo, rustfft's AVX, SSE and NEON code on), on 4 s of Vorbis at 48 and
  44.1 kHz, MP3 and FLAC: Vorbis gives 4 different results on the CPUs
  above (AVX + FMA, SSE4.1, scalar on Core 2, NEON), the portable FFT one;
  so do its inverse MDCTs at Vorbis' sizes. FFmpeg (Chromium 156's
  configuration and source list, its x86 assembly with nasm): Vorbis gives
  4 different results (C on Core 2 and Nehalem, AVX on Sandy Bridge, AVX2
  + FMA, NEON), with `av_force_cpu_flags(0)` one, the same on x86-64 and
  arm64. MP3 and FLAC gave the same samples everywhere in both.

Every changed C++ file compiles against Chromium 156's real headers
(`tools/compile_check`: Skia's and skcms's files included, the arm64 side
of Skia's and skcms's code without warnings in the harnesses' builds)
except V8's, which it doesn't cover: `ieee754.cc` builds without warnings
for x86-64 and arm64 (`-Wall -Wextra -Wshadow -Wshorten-64-to-32`) and is
the one the `Math` comparison ran. The Rust bridge builds with the crates
above and cxx (`cargo check`, no clippy warnings).
**Needs the build machine:** compiling and linking (Rust included),
`blink_unittests` (`MoltNetCpuTest.*`), `media_unittests` and V8's own
tests, and the checks in `docs/BUILD_MACHINE_CHECKLIST.md` tagged
"fp-cpu" (two machines of different CPU generations, an arm64 Mac).
**Automated** by [`tools/leak_test`](LEAK_TESTING.md): relaxed SIMD
fails validation (`cpu.wasm_relaxed_simd`); compared between runs, a hash
of `Math` results (`cpu.math`), a frame read back in Display P3
(`pixels.p3_readback`) and decoded Vorbis (`audio.decoded_vorbis`).

## MoltNet: storage limits

A page can't ask how much memory a machine has, but it can store data
until the browser refuses more, and at 156 an off-the-record profile's
limits are computed from the machine's memory. MoltNet keeps all its
storage in memory; its limits are now fixed, and each site's are its own:

| Limit a page reaches by writing | Chromium 156, off the record | MoltNet |
|---|---|---|
| Quota: IndexedDB, Cache Storage, the origin-private file system, service worker scripts and Background Fetch together | a third of the pool for each storage key (an origin, partitioned by top-level site) | **60 MiB per site**: a top-level site's storage keys together, its own origins' and those of what it embeds |
| The pool all sites share | a random 15% to 20% of the machine's memory, drawn per profile; each key's quota is checked on its own, so all keys together may use more | **600 MiB**, the most all sites may store together |
| Eviction | every half hour (and five minutes after the profile starts), the least recently used data of any site, until all sites use less than 70% of the pool | **none**: a site that reaches its limit gets `QuotaExceededError`, and no site's data is removed to make room |
| Blob memory (`Blob`, `File` and `MediaRecorder` data held in the browser), all sites together | 2 GiB on 64-bit desktops, a fifth of the memory on 32-bit ones and ChromeOS, 1% on Android; no disk | **800 MiB**; no disk |
| `localStorage`, `sessionStorage` | 10 MiB per storage area, on every machine | unchanged |

`navigator.storage.estimate()` has reported a fixed quota in off-the-record
profiles since 156 (the usage + 10 GiB, unchanged); the limits above are
the ones writes are checked against, so a page that writes until it gets
`QuotaExceededError` now finds the same limit on every MoltNet machine.
Blob limits that Chromium can scale with memory pressure (an experiment,
off by default at 156) stay fixed.

**Why these values.** MoltNet reports 8 GB of memory, but the limits hold
real data in the browser and storage processes, so they must be safe on
the smallest machines MoltNet runs on. The totals are Chromium's own rules
for a 4 GiB machine, rounded down to 100 MiB: the pool at the low end of
Incognito's range (15% of 4 GiB, 614 MiB) is 600 MiB, and blobs get a
fifth of 4 GiB (819 MiB), Chromium's rule for 32-bit desktops and
ChromeOS. The 600 MiB bound everything MoltNet's sites store, so the
memory MoltNet's storage can take is the same whatever sites do. Each site
gets a tenth of it: Chromium's third per storage key would let a page
fill the pool from three keys (and more with its embedded frames, each a
key of its own), at which point eviction would remove other sites' data.
With a tenth for each site, all of its keys together, a site's writes
fail only once it has used its own 60 MiB, or once ten other sites hold
all of theirs; until then nothing another site stores changes how much a
site may write, and since nothing is evicted, nothing it stores removes
another site's data. On larger machines the limits are less than
Incognito allows (on 8 GB, a pool of 1.2 to 1.6 GiB and 410 to 550 MiB per
key) and far more than sites store in practice (a few MB of caches and
databases).

**Which profiles.** Only MoltNet's storage partitions (its off-the-record
profile, through `ModeForBrowserContext`); normal and Incognito profiles
keep Chromium's limits, its quota per storage key and its eviction. The
kill switch turns it off.

**What a page can still observe.** Its own site's limit: writing until
`QuotaExceededError` finds 60 MiB for the site, the same on every machine,
less what the site's other storage keys (its subdomains, what it embeds)
already use. Nothing of what other sites store, unless ten other sites
hold all 60 MiB each in the same identity; `navigator.storage.estimate()`
reports the usage + 10 GiB, as in every off-the-record profile at 156.

**What it costs MoltNet users.** Offline web apps, in-browser editors and
large downloads assembled in the page (`Blob`, OPFS) stop at 60 MiB per
site and 800 MiB of blobs, with the same `QuotaExceededError` on every
machine. Since nothing is evicted, an identity whose sites have stored
600 MiB together gets `QuotaExceededError` on every site until a site's
data is deleted (by the site, or with New Identity); MoltNet keeps nothing
after the session anyway. Counting a site's usage needs every storage
key's usage, which the quota system keeps in memory: one pass over the
identity's keys per quota check.

**Code.** `chrome/browser/molt_ai/fingerprinting/molt_net_storage_values.h`
(the values and their derivation, + `molt_net_storage_values_unittest.cc`)
and `molt_net_storage.{h,cc}` (the settings for a profile's mode, +
`molt_net_storage_unittest.cc`). Hooks (each marked `MoltBrowser:`):
`content/public/browser/content_browser_client.{h,cc}` (new
`GetFixedQuotaSettings` and `GetFixedBlobStorageLimits`, none by default),
overridden in `chrome/browser/chrome_content_browser_client.{h,cc}`;
`content/browser/storage_partition_impl.cc` (`GetQuotaSettings`),
`content/browser/blob_storage/chrome_blob_storage_context.cc` (`GetFor`)
and `storage/browser/blob/blob_memory_controller.{h,cc}` (the new
`SetFixedStorageLimits`, which the machine-derived and pressure-scaled
limits then leave alone). The site budget:
`storage/browser/quota/molt_storage_budget_policy.h` (the rules, std-only,
+ `molt_net_storage_budget_policy_unittest.cc`) and `molt_storage_budget.h`
(a site's usage from each storage key's), hooked into
`storage/browser/quota/quota_settings.h` (the new `molt_per_site_quota`, 0
everywhere else), `quota_manager_impl.cc` (`UsageAndQuotaInfoGatherer`
counts every key's usage when there is a budget, and its `Completed()`
leaves a key no more room than its site's budget and the total have left)
and `quota_temporary_storage_evictor.cc` (`OnGotEvictionRoundInfo` evicts
nothing for the pool when there is a budget).

**Verification.** Standalone: the values and glue tests pass with g++
(ASan/UBSan) and clang (UBSan trap, `-fno-exceptions` and the strict
warning set); a harness runs the hooked code extracted verbatim from
`storage_partition_impl.cc`, `chrome_blob_storage_context.cc` and
`blob_memory_controller.cc` against stand-ins (5 tests: the fixed quota in
MoltNet, other profiles still asking the machine, test settings still
winning, the fixed blob limits, which memory pressure doesn't scale); 10
mutants of the glue and hooks each fail it or the glue tests. The site
budget: its rules' unit tests (5 tests) pass built standalone with g++
(ASan/UBSan); a second harness compiles `quota_settings.h`, the two
budget headers and `molt_net_storage_values.h` unchanged and includes the
hooked parts of `UsageAndQuotaInfoGatherer` (`OnGotSettings()`, the new
`MoltOnGotGlobalUsage()`, the MoltBrowser block of `Completed()`) and
`QuotaTemporaryStorageEvictor::OnGotEvictionRoundInfo()` verbatim into
stand-ins with 156's members (g++ ASan/UBSan, clang `-fno-exceptions`): 4
tests (a site's subdomains and embedded third parties share its budget
and another site keeps all of its own; the total binds only with ten full
sites; Incognito and unlimited keys keep Chromium's quota and count
nothing more; the evictor evicts nothing for MoltNet's pool and still does
for Incognito's), 8 mutants caught. Every changed C++ file compiles
without errors against Chromium 156's real headers (`tools/compile_check`).
**Needs the build machine:** compiling and linking, `molt_ai_unittests`
(`MoltNetStorageTest.*`, `MoltNetStorageValuesTest.*`,
`MoltNetStorageBudgetPolicyTest.*`), `storage_unittests`
(`QuotaManagerImplTest.*`, `QuotaTemporaryStorageEvictorTest.*`, for
Chromium's own behaviour, unchanged), and the checks tagged "fp-probes"
and "moltnet-shared" in `docs/BUILD_MACHINE_CHECKLIST.md` (writing until
`QuotaExceededError` with IndexedDB, Cache Storage and OPFS, a large blob,
two sites' quotas, and no eviction after more than half an hour).
**Automated** by [`tools/leak_test`](LEAK_TESTING.md): the quota the
browser enforces for the test page's site, read through DevTools (60 MiB
with nothing stored); writing until the limit would take too long for a
kit run, so the configured limit is checked instead.

## Fonts

The set of installed font families is one of the strongest fingerprinting
signals: a page that draws text in `font-family: "X", monospace` and
compares widths, declares `@font-face { src: local("X") }`, calls
`document.fonts.check()`, measures or draws text on a canvas, or calls
`queryLocalFonts()` can test hundreds of families in a few milliseconds.
So protected contexts only see some installed ("system") families:

| Context | Installed families it can name |
|---|---|
| MoltNet window | none: every font comes from MoltBrowser's **bundled font set**, the same on every OS ([below](#moltnet-the-bundled-font-set)); in a build without the set, the platform's **allow-list** only (the same for every MoltNet user on that OS) |
| Normal / Incognito window, web site | the allow-list plus the site's **sample**: each other family name is in it with probability 1/16, decided by `PRF(site key, name)`, so stable within a site and session, unrelated across sites and sessions |
| Normal / Incognito, a context with a random key (opaque top-level site, SVG image document, third-party shared worker, a context that started before the seed) | the allow-list only (a per-context random sample could be summed over many such contexts) |
| Shields off (normal / Incognito), non-http(s) top-level site, protection off | every font, exactly as upstream |

Never affected outside MoltNet: generic families (`serif`, `sans-serif`,
`monospace`, `cursive`, `fantasy`, `system-ui`, `math`, `-webkit-body`) and
the fonts the user chose for them in settings, web fonts (`src: url()`,
`FontFace` from data), and the per-character system fallback. Web fonts
work in MoltNet too; generic families and fallback there use the bundled
set.

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

**`src: local()`** names one face by its full or PostScript name ("DejaVu
Sans Bold", "DejaVuSans-Bold"). At 156 Blink looks such a name up in the
platform's table of every installed face (fontconfig, through the browser,
on Linux; on Windows and Android a table the browser builds once, which
`local()` waits for) and keeps the faces it found per renderer process, by
name. Protected contexts never make that lookup. They read the name as a
family they may name in CSS anyway (a listed family; in a normal window
also a name in the site's sample) followed by style words, and ask for the
face the way CSS would (`font-family`, `font-weight`, `font-style`):

* Style words: one weight (`Thin`, `Hairline`, `ExtraLight`, `Light`,
  `SemiLight`, `Book`, `Regular`, `Normal`, `Roman`, `Plain`, `Medium`,
  `SemiBold`, `Demi`, `Bold`, `ExtraBold`, `Heavy`, `Black`, `ExtraBlack`,
  and their `Ultra`/`Demi` spellings), one slope (`Italic`, `It`,
  `Oblique`), and PostScript's `MT` and `PS` suffixes (`Arial-BoldMT`), in
  any order, with spaces, hyphens or underscores between them. Width words
  are not style words: a condensed face is found through a listed family
  that names it (DejaVu Sans Condensed). A name longer than 128 characters
  names no face, and at most four ways of splitting a name are tried, the
  longest family first.
* The face is used only if the family the platform resolved is visible
  (for a name in the sample: that very family) and, when the name has
  style words, the face itself has that weight and slope (no substitute,
  no synthetic bold or italic).
* Aliases (Arial on Linux, which CSS looks up as Liberation Sans) name no
  face, except a name in the site's sample whose own family is installed.

So `local()` shows nothing `font-family` doesn't. A hidden face is never
looked up, installed or not: the decision is made from the name alone,
before any platform call, so it costs exactly what a missing face costs.
Protected contexts don't use the unique-name caches either (Blink's
typeface cache, the platform's table and the faces found through it), so
one document can't tell, by timing them, whether another used a face;
keying those caches by visibility instead would not separate documents
that see the same fonts (every MoltNet page). Nor do they wait for the
platform's table. Pages that name faces of the listed families keep
working (`local("Arial Bold")`, `local("Arial-BoldMT")` on Windows and
macOS); any other name falls back to the next `src` or family, as on a
machine without the font. Unprotected contexts (Shields off, protection
off) keep 156's lookup.

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
(no other browser's list or code was used). They are checked against fresh
installs of each supported version with `tools/fonts/molt_font_inventory.py`
([below](#checking-the-allow-lists-against-fresh-installs)).

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
| `@font-face` / `FontFace` `src: local()` (and `FontFace.load()`'s status) | `local_font_face_source.cc`: `IsLocalNonBlocking`, `IsLocalFontAvailable`, `CreateFontData` (protected contexts find the face through a visible family, [above](#fonts)) |
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
reads the typeface's family from a 256-entry cache. A `local()` name is
read in one pass over at most 128 characters and costs at most four family
lookups, each one CSS could make. Toggling Shields applies from the next
document (MoltShield reloads the page).

**Known gaps (fonts)**

* **Per-character system fallback** (emoji, CJK, rare scripts) outside
  MoltNet: a page that renders characters no listed font has gets them
  from whatever installed font covers them, and can measure that. MoltNet
  falls back within the bundled set
  ([below](#moltnet-the-bundled-font-set)).
* **Text metrics and rendering**: in MoltNet windows glyphs come from the
  bundled set and are measured alike on every machine, and `measureText()`
  is rounded ([Text metrics](#text-metrics-moltnet)). In normal windows
  `measureText()` values and glyph pixels are not noised beyond the canvas
  noise; hinting, anti-aliasing and font versions differ between machines.
* **Font settings**: the fonts the user chose for the generic families
  (and the minimum font size) stay visible through generic families and
  unstyled text; a page that ships candidate fonts as web fonts can compare
  metrics and identify them. On Linux, `system-ui` shows the desktop's UI
  font, and Chromium's default `sans-serif` setting ("Arial") resolves to
  the real Arial or to Liberation Sans depending on the machine (the same
  metrics, different glyphs). Some default settings (per-script CJK
  fonts, `math`) are lists from which Chromium takes the first installed
  family, so those generics also tell which candidate is installed.
  MoltNet windows don't use the user's settings ([settings from the
  regular profile](#moltnet-settings-from-the-regular-profile)) nor these
  defaults, which also follow the UI language and, for the fixed font on
  Windows, the ClearType setting: they use the bundled set's families, the
  same on every OS ([below](#moltnet-the-bundled-font-set)); a build
  without the set uses fixed per-OS families, all on the allow-list
  ([default fonts](#default-fonts-font-size-and-encoding)).
* Outside MoltNet, users whose machine lacks an allow-listed font (removed
  by an administrator, minimal Linux installs) remain distinguishable by
  that.
* In normal windows, a sampled name that resolves to a hidden family costs a
  platform lookup that a page might time.
* Code paths that build a `Font` without a font selector use `FontCache`
  directly and aren't filtered; none is known to be reachable from web
  content (frameless documents refuse canvas fonts).

**Verification.** Standalone (no Chromium checkout): `fonts_unittest.cc`
(19 tests: normalization in all three encodings, malformed UTF-8/UTF-16,
list integrity, the sample's rate, stability and cross-site independence,
uniform mode; `local()` names: listed and sampled families, every style
word, lengths and encodings, `system-ui`) passes with g++ (ASan/UBSan)
and clang (UBSan trap, `-Wunsafe-buffer-usage -Wshorten-64-to-32
-Wimplicit-int-conversion -Wexit-time-destructors -Wglobal-constructors
-fno-exceptions`), together
with `farbling_unittest.cc`; `system_fonts.cc` and `molt_fingerprinting.cc`
type-check with the strict clang flags against stand-ins of the pinned
Blink/Skia signatures, and a 12-test harness runs the real glue against a
fake `FontCache` (per-mode visibility, aliases on two simulated machines,
name tricks, `check()`, `local()`, `queryLocalFonts()` filtering, Shields
off, random keys, workers, caching); the upstream hook statements
type-check against stand-ins of their classes. For `local()`, a harness
runs the real glue and the hooked `local_font_face_source.cc` code
extracted verbatim against a fake `FontCache` that records every lookup (8
tests: unprotected contexts keep unique names; in MoltNet an installed
hidden face and a missing one make the same lookups, none; listed faces
with their styles; the site's sample; sampled aliases; substitutes of
listed families refused; waiting for the platform's table; null contexts);
14 mutants of the matcher, glue and hooks each fail it or the pure tests,
and the farbling fuzzer also feeds `local()` names (120 s, no findings).
Every changed C++ file compiles without errors against Chromium 156's
real headers (`tools/compile_check`). Needs the build machine:
compiling Blink with the hooks, `blink_unittests` (`MoltSystemFontsTest.*`,
`MoltFontNameTest.*`, `MoltFontListTest.*`, `MoltFontVisibilityTest.*`,
`MoltLocalFaceTest.*`), the checks tagged "fp-probes" in
`docs/BUILD_MACHINE_CHECKLIST.md`, and in a running browser on each OS:
font-probing pages and suites (FingerprintJS, CreepJS font tests) in
normal, Incognito and MoltNet windows, with Shields on and off; East Asian
sites; design tools with `queryLocalFonts()`.

[`tools/leak_test`](LEAK_TESTING.md) probes 68 family names with
`document.fonts.check()` in a MoltNet window and fails any visible family
outside the bundled set, on every OS; it fails unless every name of the
set is visible and every generic keyword measures like the set's family
for it, and `diff` compares the per-script fallback widths and the set's
faces `local()` finds between runs, across machines and OSes. It also
fails any `local()` face of a family outside every allow-list that loads,
and compares the listed faces `local()` finds between runs.

### MoltNet: the bundled font set

Allow-lists keep MoltNet users of one OS alike only as far as their fonts
are alike, and they aren't: font versions differ per OS release, and
whatever a page renders that no listed font has (CJK, rare scripts,
symbols, emoji) comes from whatever installed font covers it. So
MoltBrowser ships one font set, and MoltNet renderers take every font from
it, on Linux, Windows and macOS alike: families a page names, generic
families, `system-ui`, per-character fallback, the last-resort font,
`src: local()` and `queryLocalFonts()`. Nothing installed on the machine is
used or looked up. Normal and Incognito windows are unchanged.

Tor Browser takes the same approach on Linux (bundled Noto families and
the metric-compatible Arimo, Tinos and Cousine). Its current list
(`tor-browser-build`, `projects/fonts`) could not be retrieved when this
set was put together, so the set was chosen from the Noto project's script
coverage instead; comparing the two is a check for the next font update.

**The set** (`tools/fonts/moltnet_fonts.json`, version 2026.10.1), all
under the SIL Open Font License 1.1:

| Files | Families | Size |
|---|---|---|
| Arimo, Tinos, Cousine (google/fonts, googlefonts/tinos) | metric-compatible with Arial, Times New Roman and Courier New: Latin, Greek, Cyrillic, Hebrew | 4.5 MB |
| Noto (notofonts.github.io), unhinted, variable where Noto ships one | Noto Sans; Noto Naskh Arabic; Noto Sans Hebrew, Armenian, Georgian, Ethiopic, Devanagari, Bengali, Gujarati, Gurmukhi, Kannada, Malayalam, Oriya, Tamil, Telugu, Sinhala, Thai, Lao, Khmer, Myanmar, Thaana, Syriac, Cherokee, Canadian Aboriginal, Javanese, Ol Chiki, Adlam, Mongolian, NKo, Tifinagh, Yi; Noto Serif Tibetan; Noto Sans Symbols, Symbols 2 and Math | 7.8 MB |
| Noto Sans CJK (noto-cjk), one variable collection, weights 100 to 900 | Noto Sans CJK JP, KR, SC, TC, HK | 32.7 MB |
| Noto Color Emoji (noto-emoji), COLRv1 | Noto Color Emoji | 5.0 MB |

47 files, 49,992,556 bytes (47.7 MiB); 27.2 MiB gzip -9, 18.7 MiB xz, as
the installers carry it. Each file is pinned by repository, commit, path,
size and SHA-256, with its faces (family, weight range, slope, names) and
its licence; fonts are never committed to this repository.
`scripts/bundle-fonts.sh` fetches them by those pins at build time
(`tools/fonts/fetch_fonts.py`, cached by hash; a release build refuses a
manifest with unpinned entries) and installs them with their licences:

| Platform | Where |
|---|---|
| macOS | `MoltBrowser.app/Contents/Resources/molt_fonts/`; data, sealed by the app's signature like the other resources (no code signature of their own, nothing for notarization to check) |
| Linux (.deb, .rpm, AppImage, Flatpak) | `/opt/moltbrowser/molt_fonts/` (the app directory in the AppImage and Flatpak) |
| Windows (installer, portable ZIP) | `molt_fonts\` next to `moltbrowser.exe` |

`release.sh` verifies the installed set against the manifest (missing,
extra or changed files fail the release), and the SBOM lists the set and
each font with its download location and SHA-256 ([SBOM](SBOM.md)).

**What a MoltNet page sees.** The set's families, their other names, and
aliases that keep pages written for the common families working:

| Names | Family |
|---|---|
| Arial, Helvetica, Liberation Sans | Arimo |
| Times New Roman, Times, MS Serif, Liberation Serif | Tinos |
| Courier New, Courier, Liberation Mono | Cousine |

| Generic | Family |
|---|---|
| text without `font-family`, `serif`, `cursive`, `fantasy`, `-webkit-body`, `-webkit-standard` | Tinos |
| `sans-serif` | Arimo |
| `monospace` | Cousine |
| `system-ui`, `BlinkMacSystemFont` | Noto Sans |
| `math` | Noto Sans Math |
| any generic, for Han, kana, hangul and bopomofo text | Noto Sans CJK, by region: kana JP, hangul KR, bopomofo TC; Han by the content language (`ja` JP, `ko` KR, `zh-Hant`, `zh-TW` TC, `zh-HK`, `zh-MO`, `yue` HK, other `zh` SC; SC when the language names none) |

`font: caption`, `menu`, `small-caption`, `status-bar` and the other system
font keywords resolve as `caption` does (Windows' per-keyword fonts are not
used), so they too come out alike. A character the requested fonts lack
falls back through its script's list: the script's Noto family first; for
punctuation, digits and symbols Arimo, Noto Sans, Symbols, Symbols 2, Math,
the CJK family of the text's region, then Noto Color Emoji; emoji
presentation takes Noto Color Emoji first, text presentation the symbol
families first. Every list ends with every other family of the set, so a
character the set has is always found and one it lacks is drawn as the
last-resort font's missing glyph, on every machine the same.

Faces are matched as CSS Fonts 4 says (the slope first, then the nearest
weight in the documented direction); variable faces are instantiated at
the requested weight, static ones synthesized bold or oblique the way Blink
does on Linux. Every face is created from the file's bytes with Skia's own
font backend (Fontations), on every OS, and laid out unhinted with
subpixel positioning and anti-aliasing, without embedded bitmaps: the same
scaler and settings whatever the OS, its version or the desktop's
settings. `queryLocalFonts()` lists the set's faces.

**Metrics.** What Blink derives from a font (ascent and descent, x-height,
cap height, line gap, underline, average and maximum character width, glyph
bounds and the glyph extents HarfBuzz positions marks with) is computed
partly per OS upstream. With the set, MoltNet renderers take one path on
every OS, the one Linux takes: the descent rule for subpixel positioning
everywhere, the font's own x-height and average width and the maximum
width from its bounds also on macOS and Windows, Skia's glyph bounds rather
than path bounds on macOS, and no macOS ascent adjustment for Times,
Helvetica and Courier. macOS takes HarfBuzz's advances for fonts with a
`trak` table and no `sbix`; no font of the set has either
(`tools/fonts/fonts_test.py` checks the files).
`tools/fonts/metrics_check/run.py` runs that code, copied out of `src/`,
built as Linux, Windows and macOS, over what Skia's Fontations backend
reports for every face of the set (the `skrifa` version Chromium vendors,
at 9 sizes and the faces' weights): the three OSes compute the same values
in all 828 cases, and Linux the same as without the set.

**How the files reach the renderer.** The browser opens the `molt_fonts`
files once, on a background thread, when startup-critical work is done (or
at the first renderer launch, if that comes first), checking each file's
size and sfnt header against the table compiled into the browser
(`molt_net_fonts_table.h`, generated from the manifest; the whole set or
none). Every MoltNet renderer gets `--molt-net-fonts` on its command line,
unless the browser knows the installation has no `molt_fonts` folder, and
the files as read-only handles over the fingerprinting configuration
channel (`FingerprintingConfiguration.SetMoltNetFonts`): before any
document commits when the set is open, else as soon as it is (MoltNet's
first window, a window restored at startup). The renderer maps them
read-only (shared page cache: only the pages a renderer touches become
resident) and checks each typeface's family against its own table
(`moltnet_font_set_table.cc`, the same order; `molt_net_fonts_unittest.cc`
checks the two agree). From the switch on, no font lookup reaches the
system: until the files arrive, lookups find nothing and text uses an
empty last-resort face; when they arrive, every thread that looked a font
up (the main thread and workers, which have font caches of their own)
invalidates its font cache, so documents and canvases look their fonts up
again. Files that don't match the table (count, size, header) are refused
together, leaving the set empty rather than partial, and a typeface whose
family isn't the table's is not used. An installation without the set uses
the per-OS allow-list as before.

**Hooks** (each marked `MoltBrowser:`):

| Upstream file | Function | What |
|---|---|---|
| `platform/fonts/font_platform_data.{h,cc}` | new `UniformFontSet` interface, `CreateUniformFontPlatformData`, `UniformFallbackFontForCharacter`, `UniformLastResortFontData`, `UniformRenderStyle`, `ApplyUniformRendering`; constructor and `CreateSkFont` (Linux) | the platform layer's entry to the set, and the fixed rendering |
| `platform/fonts/skia/font_cache_skia.cc` | `CreateFontPlatformData` (not Windows), `GetLastResortFallbackFont` | families from the set; the last resort from the set |
| `platform/fonts/linux/font_cache_linux.cc` | `PlatformFallbackFontForCharacter` | fallback from the set, never fontconfig |
| `platform/fonts/win/font_cache_skia_win.cc` | `CreateFontPlatformData`, `PlatformFallbackFontForCharacter` | no DirectWrite matching or fallback |
| `platform/fonts/win/font_platform_data_win.cc` | `CreateSkFont` | the fixed rendering |
| `platform/fonts/mac/font_cache_mac.mm` | `CreateFontPlatformData`, `PlatformFallbackFontForCharacter`, `GetLastResortFallbackFont` | no Core Text matching or cascade |
| `platform/fonts/mac/font_platform_data_mac.mm` | `CreateSkFont` | the fixed rendering |
| `platform/fonts/font_cache.cc` | `GetFontPlatformData` | `system-ui` maps through the set, not the platform UI font |
| `platform/fonts/font_selector.cc` | `FamilyNameFromSettings` | generic families from the set, not from settings |
| `core/css/css_pending_system_font_value.cc` | `ResolveFontFamily`, `ResolveFontSize` | system font keywords alike on every OS |
| `modules/font_access/font_access.cc` | `DidGetEnumerationResponse` | `queryLocalFonts()` lists the set |
| `platform/fonts/font_metrics.cc` | `AscentDescentWithHacks` | the descent rule on every OS; no macOS ascent adjustment |
| `platform/fonts/simple_font_data.cc` | `PlatformInit` | x-height, average and maximum width alike on every OS |
| `platform/fonts/skia/skia_text_metrics.cc` | `SkFontGetGlyphExtentsForHarfBuzz`, `SkFontGetBoundsForGlyph`, `SkFontGetBoundsForGlyphs` | glyph bounds alike on every OS |
| `chrome/browser/chrome_content_browser_client.cc` | `InitOnUIThread` | the browser opens the set after startup |

MoltBrowser's own code: `core/molt_fingerprinting/moltnet_font_set.{h,cc}`
(the rules, pure, with the generated `moltnet_font_set_table.cc`),
`moltnet_fonts.{h,cc}` (the set in a renderer: mapped files, typefaces,
`UniformFontSet`), `system_fonts.{h,cc}` (the set's names as the
visibility list, `FontPlatform::kMoltNetBundled`); in the browser
`molt_ai/fingerprinting/molt_net_fonts.{h,cc}`, the
`--molt-net-fonts` switch and `SetMoltNetFonts` in
`molt_fingerprinting.mojom`.

**What it costs MoltNet users.** About 19 to 27 MiB more per download and
48 MiB on disk. Text in MoltNet windows is drawn from the same fonts with
the same settings on every OS, so it doesn't look native on Windows and
macOS (no DirectWrite or Core Text rendering, no ClearType) nor follows
the Linux desktop's hinting; it is unhinted, as on high-density screens. Pages that name an installed font
the set lacks get the set's family for their generic fallback; scripts
outside the set are drawn as missing glyphs unless the page brings a web
font.

**Verification.** No Chromium checkout needed: `tools/fonts/fonts_test.py`
(39 tests: the font reader against hostile input, the manifest's rules,
the generated tables up to date, fetching and installing from a fake
server, pinning, the fresh-install inventory; with the fonts cached, every
pinned face against the real files) and, with the C++ tests built
standalone with g++ (ASan/UBSan) and clang, `moltnet_font_set_unittest.cc`
(generic mapping, CJK regions, face matching, fallback order, unique
names); `molt_moltnet_font_set_fuzzer` (names, language tags, scripts,
weights; [FUZZING](FUZZING.md)); `tools/fonts/metrics_check/run.py` (the
metric code of the three OSes on the real fonts, above; each of its six
hooks, removed, makes the OSes disagree). Every changed C++ file compiles
against Chromium 156's headers (`tools/compile_check`; the Windows and
macOS files need those builds). Needs the build machine: `blink_unittests`
(`MoltNetFontsTest.*`, with a worker that looked fonts up before the
files came, `MoltNetFontSetTest.*`), `molt_ai_unittests`
(`MoltNetFontsTest.*`, `MoltNetFontsDeliveryTest.*`,
`MoltNetFontSetTest.*`), the checks tagged "fonts" in
`docs/BUILD_MACHINE_CHECKLIST.md`, and the leak kit on each OS.

### Checking the allow-lists against fresh installs

`tools/fonts/molt_font_inventory.py` (Python 3.8+, standard library only,
nothing installed) lists every font file of the machine it runs on, with
its faces, family names (a variable font's per-instance names too),
versions and SHA-256, compares them with that OS's allow-list (a copy of
`font_allowlists.cc` generated into `tools/fonts/font_allowlists.json`)
and writes `molt-fonts-<os>-<version>.json` and `.md`: listed families
the install lacks, families it has that aren't listed, and the versions
of the listed ones. It records no user or host name. A fresh Windows or
macOS has no Python: there `collect_windows.ps1` (Windows PowerShell 5.1)
or `collect_fonts.pl` (macOS's `/usr/bin/perl`) collects the same data
and `molt_font_inventory.py import` analyses it on any other machine;
`merge` sums up the installs of one OS. Run it on a fresh VM of every
supported version (Windows 10 22H2 and 11, macOS 13, 14 and 15, the
common Linux distributions) before installing anything else
(`docs/BUILD_MACHINE_CHECKLIST.md`, "fonts").

## Text metrics (MoltNet)

How wide and tall text comes out is one of the strongest fingerprints left
once the installed fonts are limited: it depends on the font files, on how
the machine rasterises them and on which fonts it falls back to. MoltNet
windows now measure text the same way on every machine that has the same
fonts, and round what canvas `measureText()` reports. Normal and Incognito
windows are unchanged.

### What a page can measure (152)

| Channel | What it gives |
|---|---|
| Canvas `measureText()` | `width`, `actualBoundingBoxLeft/Right/Ascent/Descent`, `fontBoundingBoxAscent/Descent`, `alphabeticBaseline`, `hangingBaseline`, `ideographicBaseline` (shipped); `emHeightAscent/Descent`, `getSelectionRects()`, `getActualBoundingBox()`, `getIndexFromOffset()`, `getTextClusters()` (`ExtendedTextMetrics`: experimental, and an origin trial that accepts third-party tokens). Workers too (`OffscreenCanvas`) |
| Element geometry | `offsetWidth`/`clientWidth`/`scrollWidth` (whole pixels); `getBoundingClientRect()`, `getClientRects()` and `Range` rects (64ths of a device pixel, scaled up by any transform); `ResizeObserver`, `IntersectionObserver`; `getComputedStyle()` of shrink-to-fit boxes; SVG `getComputedTextLength()`, `getSubStringLength()`, `getExtentOfChar()`, `getBBox()`; hit testing at fractional points (`elementFromPoint()`, `caretPositionFromPoint()`); where lines break; container queries on a box sized by its text |
| `document.fonts.check()`, `local()`, `queryLocalFonts()` | which families exist: the bundled set in MoltNet ([Fonts](#fonts)) |
| Canvas pixels of text | farbled per site ([The noise](#the-noise)); a script that rounds or hashes coarsely sees through it |
| Emoji | which code points come out as one colour glyph, and their advances and ink boxes |

### What already was uniform, and what still varied

Before this change MoltNet already fixed the families pages can name
(the per-OS allow-list), the fonts of the generic families, the minimum
font size and the default text locale
([default fonts](#default-fonts-font-size-and-encoding)), the reported
pixel ratio and the zoom. So two MoltNet machines of one OS ask for the
same fonts. What still differed:

| Source | Across OSes | Machines of one OS |
|---|---|---|
| The font files | different fonts (Arial on Windows, Helvetica on macOS, Liberation Sans on Linux) | different versions per OS release (Windows 10 and 11, macOS 13 to 15) and per Linux distribution release (Liberation, Noto; DejaVu has been 2.37 everywhere for years) |
| How glyphs are measured | Core Text (unhinted), DirectWrite, FreeType | **Linux:** the desktop's hinting style, anti-aliasing, autohinter and per-font fontconfig rules, and the screen's scale factor: at 1x with the usual slight hinting Skia measures glyphs hinted at their pixel size (ink edges move by up to a pixel); on a screen scaled above 1x (1.25, 1.5, 2x, ...) the browser turns hinting off and subpixel positioning on (`gfx::GetFontRenderParams`), and Skia measures unhinted outlines (linear metrics). Full or medium hinting also hints the advances (whole pixels) and takes ascent and descent from the font's VDMX table, and the font's descent borrows a pixel from its ascent only with subpixel positioning. So `width`, `actualBoundingBox*` and `fontBoundingBox*` differed between a 1x and a 2x screen and between desktop settings, by fractions of a pixel and by whole pixels. **Windows:** subpixel positioning only with font smoothing on (the default); whether ClearType settings change DirectWrite's metrics within one release is unverified. **macOS:** no setting known to change metrics |
| Per-character fallback | different fonts | characters the requested font lacks (CJK, rare scripts, symbols) come from whatever installed font has them. **Emoji:** Segoe UI Emoji on Windows and Apple Color Emoji on macOS, in their OS release's version; on Linux fontconfig's first colour emoji font, Noto Color Emoji on most desktops but Twemoji, JoyPixels, Symbola, ... where installed and preferred |
| Layout in device pixels | - | element geometry carries the device scale factor and the zoom in 64ths of a pixel ([Zoom](#zoom)) |

### What MoltNet does now

**1. One way of measuring on Linux.** MoltNet renderers lay every font out
as upstream does for `text-rendering: geometricPrecision`
(`FontPlatformData::QuerySystemRenderStyle`): no hinting and subpixel
positioning, which makes Skia measure advances and ink bounds from the
unhinted outlines (linear metrics), never reads VDMX, and applies the
descent rule alike for everyone. Metrics then depend on the font files and
on HarfBuzz and Skia, which ship inside MoltBrowser; not on desktop
settings, FreeType's hinting engine or the screen's scale factor. It
covers canvas text and element geometry alike, in documents and workers.
It is process-wide in MoltNet renderers (fonts are shared by every frame
and worker of a renderer), like the time zone and locale, so it also
applies to the few internal pages a MoltNet window shows. Upstream applies
geometricPrecision only to anti-aliased text; MoltNet applies it whatever
fontconfig says, and draws text anti-aliased even where the desktop turns
anti-aliasing off (for every font, or for some fonts or sizes), since
unhinted text without it would be unreadable (turning it on is only about
looks: unhinted, Skia measures the same with or without it). One
fontconfig setting can still change metrics: embedded bitmaps, for fonts
that carry bitmap strikes (some CJK fallback fonts; none known on the Linux
allow-list), whose ink boxes then come from the bitmaps at those sizes.
Windows and macOS are left as they are:
DirectWrite and Core Text take no hinting style from the desktop. On
Windows, turning font smoothing off (on by default) puts glyphs on whole
pixels; that is left alone for the same reason.

**2. Canvas `measureText()` rounded to a grid.** In MoltNet web content,
every length `measureText()` reports is rounded to a grid: whole CSS
pixels up to 127px text, then the largest power of two that is at most a
64th of the font size (2px at 128 to 255px, 4px at 256 to 511px, ..., 16px
at 1024px). Sub-pixel noise between machines that draw the same font (a
different rasteriser build, float rounding, a font revision that moves a
glyph by a few units) no longer shows, and drawing text larger doesn't
buy precision back: the grid stays between a 64th and a 128th of an em.

* **Lengths, not attributes.** The attributes are differences of a few
  lengths: the advance, the ink box, the font's ascent, descent and em
  box, its baselines and the offset of the chosen `textBaseline`, and the
  alignment point (0, half or all of the width). MoltNet rounds those
  lengths and derives the attributes from them exactly as upstream does.
  Rounding the attributes instead would let a page measure one text under
  several `textAlign` and `textBaseline` values and recover sub-grid
  digits from the differences; this way every relation holds exactly
  (`actualBoundingBoxLeft + actualBoundingBoxRight` is the same for every
  alignment, centring moves the box by exactly half the width, every
  baseline measured against itself is 0) and nothing below the grid is
  left. Values are on the grid, or on its half around a centred
  alignment point.
* **The ink box is rounded outwards**, so a page that sizes a canvas from
  `actualBoundingBox*` still holds every glyph; everything else rounds to
  the nearest grid point, halves away from zero, and zero is always +0
  (`Object.is(-0, 0)` is false). Values are first settled on a 1/1024-step
  lattice so float noise neither moves an edge a step nor decides a tie.
* **The `ExtendedTextMetrics` methods** use the rounded alignment and
  baseline offsets: selection rects and clusters have their ends on the
  grid, `getActualBoundingBox()` is rounded outwards like the ink box,
  and `getIndexFromOffset()` answers for the grid point nearest the
  offset, so a page can't bisect its way to exact caret positions.
* **Which contexts:** MoltNet web content, documents and workers; a
  `measureText()` without a context is rounded (fail closed);
  `chrome://` and other non-http(s) top-level pages are exact. Shields
  off doesn't lift it, as elsewhere in MoltNet.
* **Drawing is unchanged.** `fillText()` and `strokeText()` draw exactly
  as before; only what scripts read changes.

**3. One emoji font on Linux.** MoltNet renderers on Linux take emoji
(emoji presentation) from Noto Color Emoji whenever it is installed, as it
is on most desktop distributions, instead of fontconfig's first colour
emoji font; Noto Color Emoji is on the Linux allow-list, so pages could
name it anyway. Where it isn't installed, upstream's fallback decides.
Windows and macOS have one emoji font per release already.

**4. One font set on every OS.** With the [bundled font
set](#moltnet-the-bundled-font-set), MoltNet renderers take every font,
emoji and fallback included, from the fonts MoltBrowser ships, render
them with the same scaler (Skia's Fontations) and the same settings
(unhinted, subpixel positioning, anti-aliased, no embedded bitmaps), and
derive their metrics the same way ([Metrics](#moltnet-the-bundled-font-set))
on Linux, Windows and macOS; 1 and 3 above then only matter for a build
without the set.

### What is left

* **Without the bundled set** (a build that doesn't ship it), text
  differs per OS (fonts, emoji, rasteriser), per OS release (font
  versions, and with them metrics and which emoji exist) and per machine
  of one release: fonts missing from the allow-list, Windows with font
  smoothing off, on Linux no Noto Color Emoji or fontconfig's
  embedded-bitmap setting, and installed fallback fonts for CJK, rare
  scripts and symbols. The [bundled font
  set](#moltnet-the-bundled-font-set) takes all of these out of MoltNet
  windows; the leak kit's `diff` across OSes checks the result on the
  build machines.
* **Element geometry is not rounded.** Layout reaches scripts through too
  many paths for rounding some of them to hold: rounding
  `getBoundingClientRect()` and `Range` rects would leave
  `ResizeObserver`, hit testing, line breaks, container queries and SVG
  text exact, would make them disagree with each other (which breaks
  pages and is itself detectable), and a `transform: scale(64)` scales
  sub-pixel widths past any grid. Changing layout itself is out of
  bounds. Element geometry does benefit from 1 (on Linux, metrics no
  longer depend on settings or the scale factor); it still carries the
  scale factor and zoom in 64ths of a pixel ([Zoom](#zoom)).
* **`measureText()` is only rounded.** The same lengths can be measured
  through element geometry, exactly. The rounding stops scripts that
  read `measureText()` (what fingerprinting libraries do), not ones
  written against MoltBrowser.

### What it costs MoltNet users

* On Linux, text is unhinted at every scale factor, as Chrome draws it on
  high-density screens and as macOS does: on 1x screens a little softer
  than with the desktop's hinting. A desktop that turns anti-aliasing off
  gets anti-aliased text in MoltNet windows.
* Canvas apps that lay text out from `measureText()` get widths within
  half a pixel: a canvas editor that places each word at the sum of
  rounded widths can drift by a pixel or two across a line, centred text
  moves by up to a quarter of a pixel, and ink boxes are up to a pixel
  larger on each side (never smaller). Above 127px the error grows with
  the size (half a 64th of the font size at most).
* `getTextClusters()` (origin trial) positions are rounded, so text drawn
  cluster by cluster can be up to half a grid step off.

### Code

| Where | What |
|---|---|
| `core/molt_fingerprinting/moltnet_text_values.{h,cc}` | the grid, the rounding, the rounding of `TextMetrics` (lengths, then attributes), spans and ink boxes, the emoji family per platform (pure, + `moltnet_text_values_unittest.cc` in `:unit_tests`) |
| `core/molt_fingerprinting/moltnet_text.{h,cc}` | which contexts, `UniformTextMetrics()` (through `TextMetricsAccess`, a friend of `TextMetrics`), the `ExtendedTextMetrics` helpers, `PinUniformTextRendering()` from `Configure(kUniform)` (+ `moltnet_text_test.cc`) |

Upstream hooks (each marked `MoltBrowser:`):
`modules/canvas/canvas2d/base_rendering_context_2d.cc` (`measureText`),
`core/html/canvas/text_metrics.h` (the friend and the grid member) and
`text_metrics.cc` (`getSelectionRects`, `getActualBoundingBox`,
`getTextClustersImpl`, `getIndexFromOffset`),
`platform/fonts/font_platform_data.{h,cc}` (`SetUniformTextRendering`,
`UniformEmojiFontData`, `QuerySystemRenderStyle`) and
`platform/fonts/linux/font_cache_linux.cc`
(`PlatformFallbackFontForCharacter`).

**Verified without a Chromium checkout** (scratch harnesses):

* `moltnet_text_values_unittest.cc` (16 tests) passes standalone with g++
  (ASan/UBSan) and clang (UBSan trap; the new file also with
  `-fno-exceptions -Wshorten-64-to-32 -Wimplicit-int-conversion
  -Wunsafe-buffer-usage -Wimplicit-float-conversion -Wfloat-conversion
  -Wexit-time-destructors -Wglobal-constructors`): against a model of
  `TextMetrics::Update()`, rounding what it computes equals running it on
  the rounded lengths for every alignment, baseline and baseline table,
  at 6 sizes (43,200 cases); attributes on the grid or its half, never
  -0; the relations above; idempotent; the ink box never shrinks;
  measurements that differ below the grid read the same; a 256th of an
  em hidden at 16px and mostly at 1000px; float noise; the emoji family
  on the Linux allow-list.
* The real `text_metrics.cc` (152 + the hooks) and `text_cluster.cc`,
  `moltnet_text.cc`, `molt_fingerprinting.cc` and verbatim extracts of the
  MoltBrowser parts of `font_platform_data.cc` (its `QuerySystemRenderStyle`
  included) type-check with the strict clang flags against stand-ins of
  the 152 signatures; the `measureText` and `font_cache_linux.cc` hook
  statements, checked verbatim, type-check in place. A harness runs all
  of it with a fake shaper (fractional advances and ink boxes, per-word
  shape results as Blink's), 152's `web_font_render_style.cc` and a fake
  fontconfig, under ASan/UBSan, in kOff, kFarble and real kUniform (8
  tests): `measureText()` through `UniformTextMetrics()` equals the model
  for 5 sizes x 5 alignments x 6 baselines, internal pages and other
  modes exact; the `ExtendedTextMetrics` methods agree with the rounded
  metrics; a 1x and a 2x screen and slight, medium and full hinting give
  the same Skia settings once pinned (and differ without); the emoji
  family only when installed. `moltnet_text_test.cc` itself (10 tests)
  builds and passes against those stand-ins plus `DummyPageHolder` and
  friends.
* 34 mutants (the ink box to the nearest point or inwards, the alignment
  offset from the exact width, the baseline offset or the hanging
  baseline unrounded, attributes rounded instead of lengths, halves to
  even, no settling, -0 kept, a grid that isn't a power of two or doesn't
  grow, no emoji family, span ends unrounded, every context or internal
  pages rounded, the rounded offsets or the grid not stored, right
  alignment taken for left, selections rounded in the page's frame, the
  ink bounds to the nearest point, carets unrounded, cluster baselines
  unrounded, the rendering not pinned, no font size, each of the five
  `text_metrics.cc` hooks removed, the geometricPrecision hook removed,
  the emoji family never looked up or not stored, `Configure()` not
  pinning) each fail one of them.

**Needs the build machine:** compiling the hooks; `blink_unittests`
(`MoltNetTextTest.*` with the real Roboto test font and shaper,
`MoltNetTextValuesTest.*`); `molt_ai_unittests` (`MoltNetTextValuesTest.*`).
In a running browser, MoltNet windows: on two Linux machines of one
distribution release, one at 1x with full hinting and one at 2x, a page
printing `measureText()` of a few strings and emoji widths must read the
same, and a span's `getBoundingClientRect().width` within a 64th of a
pixel (layout's own rounding at each scale factor), where regular windows
differ by whole pixels; text on 1x Linux must still look acceptable;
Windows 10 and 11 and macOS 13 to 15, two machines each (on Windows one
with ClearType tuned differently): the same per release; canvas apps that
lay text out from `measureText()` (a chart library, a canvas editor, a
game's HUD) still look right; an `ExtendedTextMetrics` page with an
origin-trial token.

[`tools/leak_test`](LEAK_TESTING.md) records `text.metrics` (all
attributes of one text in `sans-serif`, `serif` and `monospace` at 16 and
48px), `text.emoji` and `text.dom_width` (a span's width), fails the run
unless every `measureText()` value is a whole pixel
(`text.metrics_whole`), and `leaktest.sh diff` fails when two runs on one
OS release disagree on any of the three. It doesn't compare a run's 1x
and 2x records with each other; their `text.metrics` and `text.emoji`
should be equal (the Linux pin), which the report shows.

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
| `validationMessage`, the file input's labels and the other strings pages can read; text without `lang` | **en-US** ([details](#moltnet-the-browsers-ui-language)) | renderer `ResourceBundle` overrides; `language.cc`: `DefaultLanguage` |

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
Whether there is a touchscreen at all is hidden too
([Touchscreens](#touchscreens)).

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
dropped, so it never fires "change". Other permissions inherited blocks the
same way; MoltNet no longer inherits any permission state from the regular
profile ([settings from the regular
profile](#moltnet-settings-from-the-regular-profile)), and this override
stays so the query keeps matching `getScreenDetails()`.

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
than tall. That holds for every page at least 200 CSS pixels wider than tall,
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
can't read; they now use en-US too (their labels are pinned with the
strings, and the date picker's language is `DefaultLanguage()`, see
below), so they match the fields.

**The strings and the default text locale** (left as a design here
before) are now en-US too, see [MoltNet: the browser's UI
language](#moltnet-the-browsers-ui-language): the strings pages can read
are overridden with their en-US text in the renderer's resource bundle
(the recommended option of the design), and `DefaultLanguage()`, the
default text locale, is pinned to en-US. `--lang=en-US` for MoltNet
renderers was not taken: content appends `--lang` after the embedder's
switches, Linux renderers fork from a zygote that loaded its pak before
any profile existed, and every renderer-made text in MoltNet tabs (error
pages, media controls, screen-reader names) would turn English.

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

**Automated** by [`tools/leak_test`](LEAK_TESTING.md): `getScreenDetails()`, the `window-management`
permission, the orientation of the uniform screen at five window sizes,
mouse `screenX/Y`, the en-US validation messages, and the form controls'
sizes (compared between two runs with `--locale`). Touch, several monitors
and a pivoted monitor stay manual.

## MoltNet: settings from the regular profile

MoltNet's profile is an off-the-record profile of the user's regular one,
and Chromium builds an off-the-record profile's prefs as an in-memory
overlay that reads every pref it hasn't written itself from the parent.
Content settings are prefs too, so this is also how Incognito inherits
site permissions and the content-setting defaults
(`INHERIT_IN_INCOGNITO`, `INHERIT_IF_LESS_PERMISSIVE`). In MoltNet that
let pages see the user's own settings: the same in every MoltNet session,
after every New Identity, and the same as in the user's regular windows.
At 152 this included, with how a page sees it:

* **Permissions and content settings, every type.** A site's block (or the
  user's default) for camera, microphone, location, notifications and the
  rest showed in `navigator.permissions.query` and in what a request
  returns; blocked JavaScript, images, pop-ups or sound showed directly;
  permission **embargo** (`PERMISSION_AUTOBLOCKER_DATA`, inherited, read by
  `PermissionDecisionAutoBlocker` from MoltNet's own map) turned a "prompt"
  into "denied"; engagement data and MoltShield's per-site *Shields off*
  (`ADS`) were inherited too.
* **WebPreferences**: the font family of every generic family for every
  script, the default and minimum font sizes (`ChromeContentBrowserClient::
  OverrideWebPreferences`, `FontFamilyCache`), the default encoding of
  pages that don't declare one (`document.characterSet`), Tab-to-links,
  force-dark, Encrypted Media, the animation policy, focus rings on mouse
  focus (`:focus-visible`), forced-colors exceptions, default scrollbar
  styles.
* **RendererPreferences** (`renderer_preferences_util.cc`): Do Not Track
  (`navigator.doNotTrack` and the `DNT` header), caret browsing, the
  Accept-Language list.
* **Third-party cookies.** `CookieSettings` and `PrivacySandboxSettings`
  only treat the *primary* Incognito profile as Incognito, so in MoltNet
  the third-party cookie mode (which Incognito even writes back into the
  regular profile) and the Privacy Sandbox switches (Protected Audience,
  Attribution Reporting, Topics) came straight from the user's settings;
  so did `canMakePayment()`'s switch and MoltShield's "Strip tracking
  parameters".
* **New at 156**: whether pages that use XSLT show Blink's deprecation
  banner (WebPreferences; "Never show again" sets
  `profile.xslt_deprecation_banner_suppressed`, and the banner is part of
  the page's DOM), the Global Privacy Control setting behind 156's flags
  (RendererPreferences; MoltBrowser's forced GPC is unaffected), and the
  AtMemory keyboard shortcut (RendererPreferences; the renderer cancels
  that key's keydown in text fields).

**What MoltNet does now.** MoltNet's profile gets its own pref service
(`molt_net_pref_isolation.{h,cc}`, one hook in `OffTheRecordProfileImpl`'s
constructor):

* every pref `molt_net_pref_isolation_policy.h` lists (the groups above, by
  prefix for fonts and content settings, so new scripts and new content
  types are covered) is set to MoltBrowser's registered default in
  MoltNet's own store before anything reads it, so the overlay never reads
  the regular profile's value, and later changes there don't reach MoltNet
  either;
* nothing is written through to the regular profile (DevTools' throwaway
  browser contexts work the same way), so nothing done in a MoltNet window
  changes the user's settings.

Because it works at the pref layer, everything downstream follows without
hooks of its own: `HostContentSettingsMap`'s providers (and so
`PermissionContextBase`, `navigator.permissions.query`, embargo, Shields
off), WebPreferences, RendererPreferences, `CookieSettings`,
`PrivacySandboxSettings` and the network context's Accept-Language.

**Kept:** changes made inside the MoltNet session (a permission granted,
Shields turned off for a site; in memory, gone at New Identity), MoltNet's
own values (proxy, WebRTC policy, prediction, HTTPS-Only, Safe Browsing
off, en-US), values an administrator sets by policy, and prefs pages can't
observe (bookmarks bar, download folder, spell checking, the user's
choices about quiet permission prompts). Safe Browsing is off in MoltNet
because its pings would leave outside Tor (docs/MOLTNET_COMPLIANCE.md §2,
"Requests outside Tor"); that isn't something pages can see.

The `window-management` override in `permission_status_listener.cc` is no
longer what stops the regular profile's state from showing, but it stays:
it keeps `navigator.permissions.query` consistent with
`getScreenDetails()`, which rejects without asking.

**Costs for MoltNet users.** Their own font and font-size settings don't
apply in MoltNet windows (zoom still works, per site, for the session;
MoltNet's default fonts, minimum logical font size and encoding are fixed
per OS, not by the UI language: [default
fonts](#default-fonts-font-size-and-encoding));
nor do blocked-by-default JavaScript or images, their site permissions
(sites ask again in every MoltNet session), their Shields-off sites, Do Not
Track, or allowed third-party cookies. Accessibility settings that pages
can detect (caret browsing, focus highlight, animation policy, default
scrollbars) are at their defaults in MoltNet windows; caret browsing can
be turned on there with F7, for the session.

**Verification.** `molt_net_isolation_policy_unittest.cc` (6 tests) passes
standalone with g++ (ASan/UBSan) and clang (`-Wunsafe-buffer-usage
-Wshorten-64-to-32 -Wimplicit-int-conversion -Wexit-time-destructors
-Wglobal-constructors -fno-exceptions`). `molt_net_isolation.cc` and the
pref-service cases of `molt_net_isolation_unittest.cc` run against
stand-ins of the pinned `PrefService`, `PrefServiceSyncable` and
`OverlayUserPrefStore` (the regular profile's values and later changes
don't show, policy still wins, nothing is written through, while an
Incognito-style service with the persistent list leaks both ways); a
mutation that drops the pinning fails them. The constructor hook compiles
against stand-ins of the pinned `Profile::OTRProfileID` and
`pref_service_syncable_util.h` and picks MoltNet's service only for
MoltNet ids. **Needs the build machine:** `molt_ai_unittests`
(`MoltNetIsolationTest.*`, `MoltNetPrefServiceTest.*`,
`MoltNetContentSettingsTest.*` with a real `HostContentSettingsMap`:
blocked JavaScript, a window-management block, Shields off and an embargo
in the regular profile, none of them in MoltNet, while an Incognito-style
profile inherits all four), and in a running browser the checks in
`docs/MOLTNET_COMPLIANCE.md` §4.

[`tools/leak_test`](LEAK_TESTING.md) seeds the regular profile (Do Not Track, font size and family,
language, third-party cookies, JavaScript and location blocked for the test
site) and checks that a MoltNet window shows none of it.

## MoltNet: the browser's UI language

MoltNet already showed en-US where pages ask for a language directly
(`Accept-Language`, `navigator.language(s)`, `Intl`, `Date`) and in the
formats of form controls. Three things still followed the language of
MoltBrowser's interface (the renderer's `--lang`), so a MoltNet user with
a Japanese, German or Russian interface stood out, in every session and
after every New Identity:

| What a page could see | Where it came from at 152 | MoltNet now |
|---|---|---|
| Text the renderer makes and pages read or measure: `validationMessage`, the file input's labels, the default `<details>` summary and button labels, date-field placeholders, the permission elements' labels, ... | `Platform::QueryLocalizedString` -> `ChromeContentClient::GetLocalizedString` -> the renderer's `ui::ResourceBundle`, i.e. the locale pak of `--lang` (on Linux, the one the zygote loaded) | **en-US**, the same text on every OS |
| The default text locale: text without a `lang` attribute (the Han glyph variants of CJK text, hyphenation, line breaking, case mapping, the quotes of `<q>`), the date picker's language, the default caption track | `DefaultLanguage()` and `UserPreferredLanguages()` (`platform/language.cc`): `--lang` | **en-US** |
| The fonts of the generic families, the minimum logical font size, the default encoding (`document.characterSet` of pages that don't declare one) | Chromium registers their defaults from the UI language's resources (`PrefsTabHelper::RegisterProfilePrefs`), and MoltNet's settings isolation pinned the registered default | **fixed**: the bundled font set's families on every OS (in a build without the set, the OS's en-US defaults, each font resolved on the font allow-list); the OS's en-US minimum size; windows-1252 |

Normal and Incognito windows are unchanged: the UI language is the same on
every site, so fixing it there would only make users look like other
MoltBrowser users.

**Kill switch.** The strings and the default text locale are kUniform,
like the time zone and the `Intl` locale: with
`#molt-fingerprinting-protection` off, MoltNet's `Accept-Language` and
`navigator.languages` go back to the UI language's default list (its first
entry is the UI language), so English strings would hide nothing and only
make such users stand out more. The `--molt-net-renderer` switch, which
marks MoltNet renderers whatever the mode, was therefore not needed. The
font, font-size and encoding defaults are part of MoltNet's settings
isolation, which doesn't depend on the kill switch.

### Renderer strings

In a MoltNet renderer, `ConfigureRendererProcess()` (from
`ChromeContentRendererClient::RenderThreadStarted`, before any frame or
worker exists) overrides each page-readable string with its en-US text in
the process's `ui::ResourceBundle`
(`ResourceBundle::OverrideLocaleStringResource`, checked before the locale
paks by every `GetLocalizedString`). So every Blink caller, on every
thread, gets the en-US text; `$1`-style placeholders stay and Blink fills
them in as before (with en-US number formats). It works the same on every
OS, after the Linux zygote fork too, and nothing else changes: the browser
UI, error pages, media controls and accessibility names keep the user's
language. (`OverrideLocaleStringResource` isn't thread-safe; at that point
no other thread reads strings yet.)

**Pinned** (pages can read or measure them):

* `validationMessage`, and the validation bubble that shows the same
  text: all 34 `IDS_FORM_VALIDATION_*` messages (missing values of text
  fields, checkboxes, file inputs, radio groups and selects; type
  mismatches, including the detailed email ones; patterns; ranges and
  steps; bad dates and numbers; too long, too short);
* text laid out in the page, whose size a page can measure: `<details>`
  without `<summary>` ("Details"); `<input type=submit>` and `reset`
  without a value ("Submit", "Reset"); the alt text of an
  `<input type=image>` without one ("Submit"); the file input ("Choose
  File", "Choose Files", "No file chosen", "3 files"); the date-field
  placeholders ("mm", "dd", "yyyy"); the week field's layout ("Week 51,
  2012"); a `<select multiple>` drawn as a menu list ("3 selected");
* the labels of the permission elements, all 14 ("Use location", "Use
  camera", "Install", ...; `<geolocation>` and `<usermedia>` are on by
  default at 152, `<install>` is an origin trial): an element with a
  `lang` attribute gets per-language strings that are the same for
  everybody, one without (most) got the UI language's;
* the JSON viewer's "Pretty-print" label, in the document of a
  same-origin frame;
* the XSLT deprecation banner (new at 156), all six
  `IDS_XSLT_DEPRECATION_BANNER_*` strings (its text, the removal and
  extension links, the alert variants and "Never show again"): Blink
  inserts the banner into the transformed page's own DOM, in an open
  shadow root, so the page can read every text node of it. Whether it
  shows at all follows MoltNet's own setting, not "Never show again" in a
  regular window (`profile.xslt_deprecation_banner_suppressed`,
  [MOLTNET_COMPLIANCE.md §2](MOLTNET_COMPLIANCE.md#2-how-it-is-built));
* the byte units (`IDS_UNITS_*`): no page-readable use was found at 152,
  pinned anyway since English units cost nothing.

**Pinned although pages can't read them:** the labels of the date, month,
week and colour picker popups ("Today", "Clear", "This month", "This
week", "Week", "Other..."), so the popup, whose month and weekday names
now follow the en-US default language, isn't half English.

**Left in the UI language** (only the user sees them): the media
controls (their overflow and captions menus, cast and picture-in-picture
texts, errors; in closed user-agent shadow trees, which don't change the
media element's size), the media session's source name (browser UI), the
plugin placeholder, DevTools' "Debugger paused in another tab", the
view-source page's "Line wrap", and the accessibility names (`IDS_AX_*`,
from `ui/strings`, never laid out).

**The table** (`moltnet_locale_strings_table.cc`) is generated from
`blink_strings.grd` and `permission_element_strings.grd` at the pin by
`tools/moltnet_locale/generate_tables.py`, which reads them from the
upstream cache (no checkout needed) and lists every message of both files
as pinned or left localized, with the reason. A message that a rebase
adds, removes or makes platform-dependent fails the script until someone
decides, and `molt_ai_unittests` compares the table with the en-US paks
grit builds. Run the script (`--write` to regenerate) after every
Chromium rebase.

### Default text locale

`DefaultLanguage()` is the renderer's UI language, kept in a static string
`InitializePlatformLanguage()` creates during Blink's start-up, before the
embedder configures the process; by then the static string table is
frozen. So the hook in `language.cc` is a pointer to a string literal
(`SetPlatformLanguageOverride`, an atomic pointer) that `DefaultLanguage()`
turns into an `AtomicString` on the reading thread, and
`UserPreferredLanguages()` now goes through `DefaultLanguage()`.
`Configure(kUniform)` sets it to en-US (`moltnet_locale.cc`) before any
document or worker exists and before any text is laid out (`LayoutLocale`
caches the default per thread). The testing override still wins.

What changes for a MoltNet page: text without a `lang` attribute is laid
out as en-US. For CJK text that matters most: `LayoutLocale::LocaleForHan`
takes the first Han-capable language of Accept-Language (none: en-US),
then the default locale (was a Japanese or Chinese UI, now en-US: none),
then ICU's default (already en-US in MoltNet), so unlabelled Han text gets
the same glyph variants and fallback fonts for everyone. Hyphenation
(`hyphens: auto`), line breaking, case mapping and `<q>` quotes of
unlabelled text use en-US; the date picker uses en-US names; and for
users who asked for captions, a video's default caption track is picked
for English.

Not covered: code that reads `Platform::DefaultLocale()` itself rather
than `DefaultLanguage()`; none was found that pages can observe, but
without a checkout the search wasn't exhaustive.

### Default fonts, font size and encoding

At 152 Chromium registers these defaults from the UI language's resources
(`chrome/browser/ui/prefs/prefs_tab_helper.cc`):

* `webkit.webprefs.fonts.<generic>.<script>`, which `FontFamilyCache` reads
  into WebPreferences: `IDS_*_FONT_FAMILY*` of
  `locale_settings_{win,mac,linux}.grd`, translated per UI language (with
  a Japanese UI the script-independent fonts are Japanese ones); the
  per-script defaults for the UI language's own script are left out;
  `",A,B,C"` lists resolve to the first installed family; on Windows the
  fixed font is Consolas or Courier New by the ClearType setting;
* `webkit.webprefs.minimum_logical_font_size`: 6 (Windows, Linux) or 9
  (macOS) in English, 10 or 12 for some Japanese, Korean and Chinese UIs;
* `intl.charset_default`: windows-1252 in English, Shift_JIS in Japanese,
  GBK in Chinese, ...

MoltNet's settings isolation pinned all of them to the registered default,
so the user's own settings no longer showed, but those defaults did. Now
`PinMoltNetPrefsToDefaults` (no new hook) gives these prefs fixed per-OS
values (`molt_net_locale_prefs.{h,cc}`):

* each font: the OS's en-US default, resolved the way a default install
  resolves it: the first family of the default (or of its list) that is on
  the OS's font allow-list ([Fonts](#fonts): installed on every supported
  version), an alias replaced by the family it stands for (Linux: Times
  New Roman -> Liberation Serif, Arial -> Liberation Sans), none when no
  family is listed. Two exceptions: Windows' fixed font is always Consolas
  (ClearType is on by default) and Linux's is DejaVu Sans Mono instead of
  fontconfig's "Monospace", which distributions resolve to different
  fonts. Every other font pref (most scripts) has no family, as in
  Chromium; Blink then uses the script-independent one and, for a generic
  family without one, the standard font;
* the minimum logical font size: the OS's en-US value;
* the default encoding: windows-1252, on every OS.

| | standard / serif | sans-serif | monospace | cursive / fantasy / math | per-script (examples) |
|---|---|---|---|---|---|
| Windows | Times New Roman | Arial | Consolas | Comic Sans MS / Impact / Cambria Math | Japanese Yu Gothic (no serif: Yu Mincho and MS PMincho are optional features), Korean Malgun Gothic, Chinese Microsoft YaHei / JhengHei |
| macOS | Times | Helvetica | Menlo | Apple Chancery / Papyrus / STIX Two Math | Hiragino, Apple SD Gothic Neo, PingFang, Songti |
| Linux | Liberation Serif | Liberation Sans | DejaVu Sans Mono | none (Comic Sans MS, Impact and Latin Modern Math aren't on every distribution) | CJK: none listed, so per-character fallback |

The table (`molt_net_locale_prefs_table.h`) is generated by the same
script from the three `.grd` files, `components_locale_settings.grd` and
`font_allowlists.cc` (so the script must be rerun after an allow-list
edit); its en-US column is checked against the paks grit builds on each
OS's build machine.

With the [bundled font set](#moltnet-the-bundled-font-set), MoltNet
renderers answer generic families from the set before these prefs are
read (`FontSelector::FamilyNameFromSettings`), so the fonts in this table
apply only to a build without the set; the minimum font size and the
encoding apply either way.

### What a fixed language costs MoltNet users

* Form validation bubbles, file inputs, the default `<details>` and button
  labels and the date picker are in English in MoltNet windows.
* For users whose captions setting asks for subtitles, videos pick English
  caption tracks by default.
* CJK text without a `lang` attribute no longer gets the glyph variants
  of the user's language; pages that declare their language are
  unaffected.
* Old pages in a legacy CJK or Cyrillic encoding that don't declare it
  rely on Chromium's encoding detector, as they do for English-language
  users; the rare page it misses shows garbled text.
* CJK users get the English minimum logical font size, so very small
  relative font sizes stay smaller than in their regular windows.

### Code and verification

| Where | What |
|---|---|
| `src/chrome/renderer/molt_ai/fingerprinting/moltnet_locale_strings.{h,cc}`, `moltnet_locale_strings_table.cc` (generated) | the page-readable strings, pinned in kUniform renderers (+ `moltnet_locale_strings_unittest.cc`) |
| `core/molt_fingerprinting/moltnet_locale.{h,cc}` | pins `DefaultLanguage()` in `Configure(kUniform)` (+ `moltnet_locale_test.cc`) |
| `src/chrome/browser/molt_ai/tor/molt_net_locale_prefs.{h,cc}`, `molt_net_locale_prefs_table.h`, `molt_net_locale_prefs_resource_ids.h` (generated) | MoltNet's fixed fonts, minimum logical font size and encoding (+ `molt_net_locale_prefs_unittest.cc`); applied in `molt_net_pref_isolation.cc` |
| `tools/moltnet_locale/generate_tables.py` | generates and checks the tables from the pinned `.grd` files |

Upstream hooks: `third_party/blink/renderer/platform/language.{h,cc}`
(`SetPlatformLanguageOverride`, `DefaultLanguage`,
`UserPreferredLanguages`). The renderer strings and the font defaults need
no upstream hook (`ConfigureRendererProcess` and `PinMoltNetPrefsToDefaults`
are MoltBrowser's).

**Verified without a Chromium checkout:** the script regenerates both
tables identically from the pinned `.grd` files, read both from the
upstream cache and from files, and fails as it should on a new, renamed
or changed message, a changed font default and a changed encoding. The
renderer-strings code and its real unit test build and pass with g++
(ASan/UBSan) and clang (UBSan trap, `-fno-exceptions
-Wunsafe-buffer-usage -Wshorten-64-to-32 -Wimplicit-int-conversion
-Wexit-time-destructors -Wglobal-constructors`) against stand-ins of the
pinned `ResourceBundle` and `DataPack` (fake en-US and "French" paks made
from the `.grd` files); 4 mutants all fail it. `molt_net_locale_prefs.cc`,
its real unit test (against the real Blink allow-lists and fonts matcher)
and the real `molt_net_pref_isolation.cc` (against a `PrefService`
stand-in, with a Japanese UI's registry) pass with the same compilers and
flags; 9 mutants all fail them. The `language.cc` hook and
`moltnet_locale.cc` compile against stand-ins of the pinned WTF types and
pass a harness (pinned on the main and another thread, unpinned, the
testing override first). **Needs the build machine:** compiling all of
it; `molt_ai_unittests` (`MoltNetLocaleStringsTest.*`, against the real
en-US and French paks; `MoltNetLocalePrefsTest.*` with the real paks of
each OS; `MoltNetLocalePinTest.*`); `blink_unittests`
(`MoltNetLocaleTest.*`); and, in MoltNet windows of builds with a
Japanese, German and Russian interface on each OS: `validationMessage` of
an empty required field, the size of `<input type=file>`, `<details>` and
`<input type=date>`, `document.characterSet` of an undeclared page, the
measured width of unstyled, `serif` and `monospace` text, and CJK text
without `lang` (glyph variants), all identical to an English-interface
MoltNet window on the same OS.

With [`tools/leak_test`](LEAK_TESTING.md): run once with `--locale ja_JP.UTF-8` (on Linux the OS locale
also sets the browser's interface language) and once without, then
`leaktest.sh diff` the two reports; it compares exactly these values.

## Built-in AI APIs

`LanguageModel`, `Summarizer`, `Writer`, `Rewriter` and `Translator` run
on the user's on-device model ([design](design/WEB_AI_APIS.md), §8,
§10). What they expose:

* **Presence:** all five interfaces exist on every http(s) page in every
  window (MoltBrowser turns `Writer` and `Rewriter` on), so their presence
  says nothing about the user.
* **`availability()`:** `"downloadable"` to every site the user hasn't
  allowed, on every capable machine, whether a model is installed or not;
  `"available"` only to sites the user allowed (remembered per profile and
  top-level origin until removed in `molt://ai-settings` or cleared with
  "Site settings"; Incognito starts empty and keeps its own in memory);
  `"unavailable"` with a policy, a switch, a block the user chose, a
  prompt paused after three closes (for a week; it looks exactly like a
  block), or on a machine that can't run any catalog model (about one
  bit). Never `"downloading"`. What each state tells a site:
  [design §4.3](design/WEB_AI_APIS.md).
* **Limits:** `inputQuota` is 1024 or 4096 tokens for `LanguageModel`
  (about one bit, allowed sites only) and 9216 for the writing APIs; token
  counts come from a model-independent estimator; the default and maximum
  sampling parameters are the same for everyone. No model name, size or
  exact context window.
* **Not hidden, allowed sites only:** generation speed (hardware and model
  class), the model's behaviour, and whether the first call had to load
  the model.
* **MoltNet:** every API reports `"unavailable"`, the same for every
  MoltNet user. **Shields** are not consulted: the APIs are granted per
  site, and with Shields off a site learns nothing more.
* **Translator:** the supported pairs are a fixed table of 20 languages,
  the same for every user and model; any other pair, or a language with
  itself, is `"unavailable"`. For a supported pair the answer is exactly
  the site's `LanguageModel` answer above, so the Translator adds nothing
  but whether a pair is in a public table. It never depends on the
  installed models, the OS language or Accept-Language, and keeps no
  per-pair or per-site state. Upstream's answer did: `"available"` only
  for pairs whose TranslateKit language packs were installed (from what
  the user translated anywhere) when both languages were in Accept-Language
  or English, or the origin had created that pair before (a stored site
  setting). The codes a Translator reports back are the table's best fit
  (`en` for `en-US`). `inputQuota` (Infinity) and `measureInputUsage()`
  (0) are renderer constants; translations come piece by piece from the
  same model, so their speed and wording are visible to allowed sites like
  the other APIs' ([design §10](design/WEB_AI_APIS.md)).
* **LanguageDetector** is `"unavailable"` in every window for every user
  (its upstream model needs a Google API key, so it used to say
  `"downloadable"` in a keyless build and `"available"` with the user's own
  key: about one bit).

## Re-checked at Chromium 156

The guarantees above were built against Chromium 152. After the rebase to
156.0.8078.48 every page-observable surface that shipped or changed in the
four milestones between was looked at again, and so was every upstream
function MoltBrowser hooks.

**How.** Without a checkout: the 152 and 156 copies of
`runtime_enabled_features.json5` (every feature that became `stable`, was
removed or renamed, or gained an origin trial), the `BASE_FEATURE` defaults
of `third_party/blink/common/features.cc`, `content_features.cc` and of the
media, UI, GPU, permissions, storage and `chrome` feature lists (every flip
to enabled-by-default), the Web IDL of all of Blink (every new interface,
attribute, method and enum value, with the flag that gates it), the CSS
media features and `env()` variables, the client hint and permission
enums, the WebGL extension list, `ScreenInfo`, `WebPreferences` and
`RendererPreferences`, and the consumers in Blink of the device pixel
ratio, the screen, the locale and the CPU tier. For each hooked upstream
function, its 152 and 156 bodies were compared and the 156 body read for
new early returns or paths around the hook (none found).

### What changed, and what MoltBrowser does about it

| At 156 | What a page can see | MoltNet windows | Normal and Incognito | Decision |
|---|---|---|---|---|
| `color-gamut` is computed from how much of the P3 and Rec. 2020 gamuts the display covers (90%); with `dynamic-range`, `video-dynamic-range`, `color` and `monochrome` the media queries describe the display | the display's gamut, HDR support and bits per component | **srgb, standard, 8 bits, colour**, the same for everyone (new) | real | made uniform in MoltNet ([below](#moltnets-display-colour-and-scrollbar-style)) |
| macOS takes overlay or classic scrollbars from `RendererPreferences`, like Windows, Linux and ChromeOS | whether scrollbars take room, and when that changes (a mouse plugged in) | **the platform's default style** (new) | the OS setting | made uniform in MoltNet ([below](#moltnets-display-colour-and-scrollbar-style)) |
| Off-the-record profiles report a static storage quota (`kIncognitoStaticStorageQuota` on by default) | `navigator.storage.estimate()` | the usage + 10 GiB for everyone | Incognito the same; a regular profile the usage + 10 GiB, or its disk where that is smaller | upstream; the leak kit checks it. The limits writes reach are fixed in MoltNet too ([storage limits](#moltnet-storage-limits)) |
| `renderSizeHint` / `renderQuantumSize` (`WebAudioConfigurableRenderQuantum`, stable) | the render quantum a page asked for; `"hardware"` | 128 for `"hardware"` (no hardware value is read at 156), now pinned ([audio](#moltnet-audio-drm-and-media-capabilities)) | same (not pinned) | the leak kit checks it. Audio read back from any quantum is farbled as before |
| `<camera>` and `<microphone>` elements (`CameraAndMicrophoneElements`, stable) | permission elements like `<usermedia>` | labels in en-US (the generated table covers them), the session's own permission state, a track only after the user grants access | the profile's permission state | none |
| CSS `random()` (`CSSRandomFunction`, stable) | random values, cached per document and element from a CSPRNG | nothing about the device | same | none |
| `RTCPeerConnection.startDiagnosticLogging()` (`RTCDiagnosticLogging`, stable on desktop) | a random session id | the browser collects no logs (MoltNet keeps no WebRTC text logs) | logs only for origins an enterprise policy lists | none |
| `PerformanceEntry.navigationId` always exposed; Event Timing dispatch as HTML specifies (`EventTimingMatchingHTML`) | ids per document, starting at a random 100 to 10000, steps of 7 | same | same | none |
| WebGPU: compatibility mode, new features, limits and texture usage | the GPU | no adapter | real (not covered, see [known gaps](#known-gaps-and-attacks)) | none |
| `local()` faces memoised per process (`FontUniqueNameTypefaceCache`); `queryLocalFonts()` refused in inactive frames | installed fonts | the bundled set (a build without it: the allow-list); `local()` is a lookup of a visible family and never uses the cache | allow-list + the site's sample, the same way | `local()` resolved through visible families ([Fonts](#fonts)) |
| New audio decoders (Symphonia, direct Opus) | `decodeAudioData()` results | the same for every user of a build; farbled when read | same | none |
| Media clock coarsening (`kCoarseWallClockTimeSource`, on) | the media clock's precision | coarser | coarser | upstream hardening |
| `drawElementImageToTexture` (WebGPU, HTML-in-canvas) | page content in a GPU texture | **off**: behind `CanvasDrawElement` (experimental); the `HTMLInCanvas` origin trial's tokens stay refused | same | none |
| `system_color_chooser_is_modal` (`RendererPreferences`, Linux) | on Wayland the colour picker waits while the system eyedropper has focus, which makes it behave as on X11 | no value a page reads changes | same | none |
| Linux takes the desktop's accent colour from the XDG portal (`kUsePortalAccentColor`) | the `AccentColor` keyword shows it to installed web apps of the default profile only; form controls paint with it, which pages can't read back | the default accent | the default accent (the OS's only in installed web apps of the default profile) | none |
| Media Capabilities: `encryptionScheme` in key-system queries (`KeySystemTrackConfigurationEncryptionScheme`) | the answer EME's `encryptionScheme` already gives | no key systems at all ([DRM](#moltnet-audio-drm-and-media-capabilities)) | unchanged | none |
| Unbounded elements (`HTMLElement.unbounded`, `showUnboundedElement()`; `UnboundedElement`, stable) | - | **not on the web**: enabled per context, and the open web needs `UnsafeUnboundedElementOnTheOpenWeb` (off) | same | none |
| The Global Privacy Control setting, the XSLT banner's "Never show again" and the AtMemory shortcut in the preferences the browser sends (`RendererPreferences`, `WebPreferences`) | user settings | MoltBrowser's (pinned at the rebase, [settings](#moltnet-settings-from-the-regular-profile)) | the user's | done at the rebase |
| Behind flags, not shipped: `GeometryUtils` (`getBoxQuads()`), HTML-in-canvas, `navigator.playHaptics()`, persistent widgets, `window.alwaysOnTop`, the WebTransport additions, `FractionalScrollOffsets` | - | - | - | re-check when they ship |

No client hint, permission name, WebGL extension, `env()` variable, media
feature or `ScreenInfo` field was added; Topics' document API moved to the
`TopicsAPI` flag and Shared Storage was removed.

### MoltNet's display: colour and scrollbar style

**Colour.** The media queries that describe the display's colour told a
wide-gamut or HDR screen (most recent Macs, HDR monitors, OLED laptops)
from a standard one, and its bits per component from 8, while
`screen.colorDepth` already said 24 for everybody. At 156 the colour gamut
is decided by how much of the P3 and Rec. 2020 gamuts the display covers,
which moves some displays from one answer to another. MoltNet pages now
see one display: `(color-gamut: srgb)` matches and `p3` and `rec2020`
don't, `(dynamic-range: high)` and `(video-dynamic-range: high)` don't
match, `(color: 8)` does and `(monochrome)` doesn't, consistent with a
colour depth of 24. sRGB at 8 bits is the most common desktop display and
what Chromium reports for a screen whose colour space it can't read. The
values come from four `MediaValues::Calculate*` functions, the one place
media queries, `matchMedia`, `<source media>` and the preload scanner get
them from, so they all agree. DevTools' colour-gamut emulation still wins.
Only what pages read changes: Blink still renders for the real display, so
wide-gamut and HDR content still looks right where a page shows it anyway.
*Cost:* pages that pick HDR video or P3 images by media query serve their
standard versions in MoltNet windows.

**Scrollbar style.** Whether scrollbars take room in the layout (classic)
or are drawn over the content (overlay) follows an OS setting: on macOS
"Show scroll bars", whose default ("automatically") switches to classic
scrollbars while a mouse is connected; on Windows and Linux the
overlay-scrollbars flag. A page reads it as `innerWidth -
document.documentElement.clientWidth` (0 or 16 in MoltNet) and sees it
change when the user plugs in a mouse. At 156 macOS sends it to the
renderer in `RendererPreferences` like the other desktop platforms, so one
renderer-side switch covers all of them: MoltNet renderers apply their
platform's default style (`moltnet::UsesOverlayScrollbars()`: overlay on
macOS, classic on Windows and Linux) whatever the browser sends, in
`WebViewImpl::UpdateRendererPreferences`, where the renderer takes the
setting for Blink and for the web theme. A renderer has one scrollbar style
for all its pages, and MoltNet's renderers only host MoltNet pages. Classic
scrollbars are 16 CSS pixels wide as before ([Zoom](#zoom)). *Cost:* on
macOS, MoltNet pages show overlay scrollbars even when the user asked for
scroll bars to be always shown (they appear while scrolling and on hover,
and can still be dragged); on Windows and Linux the overlay-scrollbars flag
doesn't apply to MoltNet windows.

Which contexts: the whole MoltNet renderer process, like
`screen.colorDepth`. Normal and Incognito windows are unchanged: none of
these values can vary per site in a useful way. The kill switch turns both
off with the rest.

### The uniform values at 156

Read again in the 156 sources, with the result:

* **Screen and letterbox:** `Screen` and the hooked `LocalDOMWindow`,
  `MediaValues` and `DOMVisualViewport` functions are unchanged upstream;
  no new page-facing reader of `ScreenInfo`.
* **Device pixel ratio:** no new reader of the ratio; `image-set()` now
  caches its choice per ratio and still gets MoltNet's 1 from
  `StyleResolverState`, which builds every `ElementStyleResources`.
* **Scrollbars:** the thickness hooks hold (`Scrollbar::ScrollbarThickness`
  only reaches its theme through a pointer at 156); the style is new above.
* **Fonts and text metrics:** the family lookup still goes through
  `FontFallbackList::GetFontData` and the selector; `local()` and
  `queryLocalFonts()` as in the table; `text_metrics.cc`,
  `font_cache_linux.cc` and the hooked `font_platform_data.cc` functions
  unchanged; the tab-size fallback (`FontFallbackForTabSize`) is now
  permanent and picks among the fonts the hooked lookup returned.
* **GPU and WebGL:** every hooked WebGL function unchanged, no new
  extension; `GPU::RequestAdapterImpl` lost its compatibility-mode warning
  and still returns no adapter first.
* **Locale and language:** `language.cc` unchanged; `platform_locale.cc`
  only renamed types; the new `Locale::DefaultLocale()` caller (the XSLT
  banner) uses strings MoltNet pins; `tools/moltnet_locale/generate_tables.py`
  passes at 156.
* **Client hints, user agent and brands:** no new hint; content's
  `client_hints.cc` and `user_agent_utils.cc` only refactored around the
  hooks (`IsClientHintEnabled`, `GenerateBrandVersionList`).
* **Touch, pointer and hover:** the six `WebPreferences` fields unchanged;
  `OverrideWebPreferences` still ends with MoltNet's override (156 sets the
  clipboard exemption and the XSLT banner before it).
* **Orientation, screen events, `getScreenDetails()`, the
  `window-management` permission:** the hooked files are unchanged.
* **Network quality:** `NetworkStateNotifier` only changed a type.
* **Canvas, WebGL, audio and video farbling:** no new readback API in the
  IDL; `toBlob()`'s new colour conversion runs on the farbled copy;
  `copyExternalImageToTexture()` gained an HDR-headroom check before the
  farbled upload; the other hooked functions only changed error messages
  or a helper's signature.
* **CPU tier, memory, `hardwareConcurrency`:** `navigator.cpuPerformance`
  now applies DevTools' override on top of the browser's tier (0 in
  MoltNet); the others are unchanged.
* **Workers and Shields decisions:** a dedicated worker now also inherits
  its creator's document policy; the Shields hand-over is unchanged.

### Code and verification

| Where | What |
|---|---|
| `core/molt_fingerprinting/moltnet_values.{h,cc}` | the display's values and `UsesOverlayScrollbars()` (pure, + `moltnet_values_unittest.cc`, in `:unit_tests`) |
| `core/molt_fingerprinting/moltnet_display.{h,cc}` | which processes, the values as Blink types (+ `moltnet_display_test.cc`) |

Upstream hooks (each marked `MoltBrowser:`): `core/css/media_values.cc`
(`CalculateColorGamut`, `CalculateDeviceSupportsHDR`,
`CalculateColorBitsPerComponent`, `CalculateMonochromeBitsPerComponent`)
and `core/exported/web_view_impl.cc` (`UpdateRendererPreferences`; the
file is new in `src/`).

**Verified without a Chromium checkout:** `moltnet_values_unittest.cc` (7
tests, 2 new) passes standalone with g++ (ASan/UBSan) and clang (UBSan
trap, `-fno-exceptions` and the strict warning set), with
`farbling_unittest.cc` and `fonts_unittest.cc` still passing. A harness
runs the real `moltnet_display.cc` with the hooked code extracted verbatim
from `media_values.cc` and `web_view_impl.cc`, against stand-ins of the
types they touch, in kOff, kFarble and real kUniform, built as Linux and as
macOS (8 tests: the real display outside MoltNet, the uniform one inside,
a monochrome display, DevTools' gamut override, the scrollbar style from
the start and through OS changes without rebuilding the scrollbars); 15
mutants of the values, glue and hooks each fail it or the values test.
Every changed C++ file,
`moltnet_display_test.cc` included, compiles without errors against
Chromium 156's real headers (`tools/compile_check`). **Needs the build
machine:** compiling and linking, `blink_unittests`
(`MoltNetDisplayTest.*`), `molt_ai_unittests`, and the checks in
`docs/BUILD_MACHINE_CHECKLIST.md` tagged "fingerprinting 156" (a P3 or HDR
display; macOS with "Show scroll bars" set to always and with a mouse).
**Automated** by [`tools/leak_test`](LEAK_TESTING.md): the colour media
queries, `scrollbar.overlay` per OS, the quota over the usage and the
render quantum for `"hardware"`.

## Kill switch

`chrome://flags/#molt-fingerprinting-protection` (feature
`MoltFingerprintingProtection`, enabled by default). When disabled, new
renderers get no mode switch and no seed (Blink runs unmodified) and MoltNet
no longer forces en-US (languages, time zone, form controls, renderer
strings, default text locale); relaunch to apply it to every renderer. It is
independent of `--disable-molt-shield` on purpose: that switch is an
ad-blocking escape hatch and must not silently drop MoltNet uniformity. The
per-site escape hatch is [Shields off](#shields-off) (normal and Incognito
windows only). MoltNet's [settings
isolation](#moltnet-settings-from-the-regular-profile), including its
fixed default fonts, font size and encoding, doesn't depend on the switch.

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
* `copyElementImageToTexture`, `drawElementImageToTexture` / canvas
  `drawElement` (HTML-in-canvas) are behind the `CanvasDrawElement` runtime
  flag (status "experimental" at the pin; its `HTMLInCanvas` origin trial's
  tokens are refused) and not hooked.
* **Encoder identity.** `VideoEncoder`/`MediaRecorder` output of a farbled
  frame no longer reveals the canvas pixels, but the encoder implementation
  itself (hardware encoders) is a device signal; not addressed in normal
  windows. MoltNet's WebCodecs and `MediaRecorder` only use the build's
  software encoders, with threads for MoltNet's 4 cores (see [audio, DRM
  and media capabilities](#moltnet-audio-drm-and-media-capabilities) and
  [MediaRecorder](#moltnet-mediarecorder)), which scale, resample and
  encode the same way on every CPU ([pixels and encoded
  bytes](#moltnet-pixels-and-encoded-bytes)).
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
* Text metrics: in normal windows `measureText()`/`TextMetrics` and
  `isPointInPath` are exact; installed fonts are limited (see
  [Fonts](#fonts)), but per-character fallback fonts, glyph rendering and
  the user's font settings still show. MoltNet measures glyphs alike on
  every machine of an OS and rounds `measureText()`, but element geometry
  is exact, font versions differ per OS release and per-character
  fallback fonts (other than emoji on Linux) show ([Text
  metrics](#text-metrics-moltnet)).

**Context coverage**

* Worklets other than threaded ones created by a document (e.g. shared
  storage worklets) get no key and are not farbled; they expose none of the
  farbled APIs.

**MoltNet uniformity not done**

* Letterboxing ([Letterboxing](#letterboxing-moltnet)): a zoomed page is
  letterboxed in CSS pixels, its media queries, `vw`/`vh` and
  `visualViewport` are whole CSS pixels and classic scrollbars 16 CSS
  pixels on every OS, but a script can catch the moment between a new zoom
  and the new size ([Zoom](#zoom)); the real device pixel ratio and the
  zoom show through device-pixel snapping, layout's sub-pixel values
  (including a scrollbar's at zooms where 16 CSS pixels aren't whole device
  pixels) and `devicePixelContentBoxSize`; overlay against classic
  scrollbars shows the OS only (each platform's default style since 156);
  cross-process iframes can report a screen one step narrower than a main
  frame with a classic scrollbar.
* (Closed: renderer strings, the default text locale and the default
  fonts, font size and encoding no longer follow the browser's UI
  language, see [the browser's UI
  language](#moltnet-the-browsers-ui-language).) What is left there:
  renderer strings pages can't read (media controls, accessibility names)
  stay in the UI language, and code reading `Platform::DefaultLocale()`
  directly isn't covered (none found that pages can observe).
* A touchscreen shows once the user touches it (`pointerType` "touch",
  touch events); `maxTouchPoints`, the pointer and hover media queries and
  touch-event feature detection no longer show it
  ([Touchscreens](#touchscreens)). Pen pressure, tilt and twist stay real
  (touch contact and coordinates are uniform, see [Touches](#touches)).
* (Closed: `navigator.connection`, its `change` event and what Blink and
  MoltNet's network context decide from the network quality no longer
  follow the browser's estimate, see [connection and network
  quality](#moltnet-connection-and-network-quality).) What is left there:
  `navigator.onLine` and the `online` and `offline` events still follow
  the operating system, on purpose.
* (Closed: permission and content-setting state, fonts and other user
  settings MoltNet inherited from the regular profile, see [settings from
  the regular profile](#moltnet-settings-from-the-regular-profile).) What
  is left there: browser-wide state isn't per profile and still shows in
  MoltNet (page colours and the system's
  forced-colors and contrast settings, MoltShield's global switch and
  filter lists, and `prefers-color-scheme`, which is dark for everyone
  only while MoltBrowser's dark toolbar, `kGlassToolbar`, is on and
  otherwise follows the regular profile's theme).
* (Closed: Web Audio's output device and when it renders, a machine
  without one, key systems, Media Capabilities and WebCodecs' hardware
  codecs, see [audio, DRM and media
  capabilities](#moltnet-audio-drm-and-media-capabilities).) What is left
  there: output device IDs once the user grants a media permission, and
  the speed of media playback.
* (Closed: when `<audio>` and `<video>` play, the device list and
  `getUserMedia()` before a permission, and `performance.memory`, see
  [media playback clock](#moltnet-media-playback-clock), [cameras,
  microphones and outputs](#moltnet-cameras-microphones-and-outputs) and
  [performance.memory](#moltnet-performancememory).)
* (Closed: in MoltNet, hardware video decoding, the GPU's colour
  conversion of read-back frames, and the CPU-specific kernels of libopus,
  libyuv's filtered scaling and the encoders' resampler, see [pixels and
  encoded bytes](#moltnet-pixels-and-encoded-bytes).)
* (Closed: in MoltNet, WebAssembly relaxed SIMD, the CPU-specific code of
  Skia, skcms and the audio decoders, `Math.pow` and `Math.tanh` from the
  C library, and V8's heap configured from the machine's memory, see [CPU
  features](#moltnet-cpu-features) and
  [performance.memory](#moltnet-performancememory).)
* DevTools' timezone/locale emulation can't be used in MoltNet windows (the
  overrides are held for the life of the renderer).
* WebRTC is off in MoltNet windows (`new RTCPeerConnection()` throws, see
  docs/MOLTNET_COMPLIANCE.md §2), which is the same for everyone, but
  `RTCRtpSender.getCapabilities()` and `RTCRtpReceiver.getCapabilities()`
  still list the codecs this machine supports, hardware ones included.
* Location is denied without a prompt in MoltNet windows, and the
  Presentation and Remote Playback APIs find no screens (both would leave
  Tor, docs/MOLTNET_COMPLIANCE.md §2). That is the same for everyone, but
  `navigator.permissions.query({name: 'geolocation'})` still says
  `prompt`, so a page can tell it is in a MoltNet window (as it can in
  other ways, e.g. WebRTC).

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
* Turning Shields off or on applies from the next navigation (MoltShield's
  toggle reloads the page) and the next start of a service or shared
  worker, not to documents and workers already running; pages restored
  from the back/forward cache keep the decision they ran with (see
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

* `farbling_unittest.cc` (27 tests: v1's 22 plus premultiplied, narrow
  `readPixels`, float32 and float16 noise, and golden digests of every
  entry point's output) passes standalone with g++
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
  decisions (and MoltNet ignoring them; the per-document harness is in
  [Shields off](#shields-off)), the seed arriving late or malformed,
  WebGL pack layouts for every farbled format, PIXEL_PACK buffer mapping
  (in-place result equal to a client-memory read; chunked mapping of a
  27 MiB read; no mapping when not farbled; failed maps), the snapshot and
  export pipelines (premultiplied alpha preserved, never above alpha) and
  the uniform screen math and UTC/en-US side effects. (Against Chromium
  152's real headers, `tools/compile_check` later found three of those
  stand-ins wrong: the GLES2 client has no `MapBufferRange`/`UnmapBuffer`,
  so the PIXEL_PACK buffer is now read back with `GetBufferSubDataCHROMIUM`
  and written with `BufferSubData`; `String::StartsWith` is `starts_with`;
  and a worker's `web_worker_fetch_context()` is protected, so a worker
  without a handed-over key now gets a random per-context key, as an
  opaque site does.)
* The chrome/{common,browser,renderer} glue (mode switch, mojo seed,
  `ConfigurationReceiver`, the per-commit Shields decision and its renderer
  agent) and
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
(`gles2_interface.h` in core), and the behaviour
in a running browser: test pages for every row of the table above (incl.
`getBufferSubData` after a PBO read, `VideoFrame.copyTo`, `captureStream` +
`MediaStreamTrackProcessor`, `copyExternalImageToTexture` + `mapAsync`,
`AudioWorklet`), in a normal window, an Incognito window and a MoltNet
window, with Shields on and off, the seed absent from `ps`/`chrome://
process-internals` command lines, plus fingerprinting suites (e.g. CreepJS,
FingerprintJS) across two sites and two sessions, and the frame-rate cost
on a canvas-effects video call.

[`tools/leak_test`](LEAK_TESTING.md) scripts the MoltNet part of this on Linux: every uniform value
above, the canvas readback differing between two sites and changing with
New Identity. Normal and Incognito windows, the per-API farbling pages,
Shields on and off, the seed on command lines and fingerprinting suites stay
manual.

## Code map

| Where | What |
|---|---|
| `src/third_party/blink/renderer/core/molt_fingerprinting/farbling.{h,cc}` | pure math (+ `farbling_unittest.cc`) |
| `src/third_party/blink/renderer/core/molt_fingerprinting/molt_fingerprinting.{h,cc}` | Blink glue (+ `molt_fingerprinting_test.cc`) |
| `src/third_party/blink/renderer/core/molt_fingerprinting/shared_worker_key.{h,cc}` | a shared worker's storage key, handed to its thread (+ `shared_worker_key_test.cc`) |
| `src/third_party/blink/renderer/core/molt_fingerprinting/shields_decision.{h,cc}` | the Shields decision of each window and worker: fail-safe, site check, inheritance (+ `shields_decision_test.cc`) |
| `src/third_party/blink/renderer/core/molt_fingerprinting/webgl_uniform.{h,cc}`, `gpu_uniformity.{h,cc}` | MoltNet WebGL baselines (pure, + `webgl_uniform_unittest.cc`) and their Blink glue, incl. WebGPU (+ `gpu_uniformity_test.cc`) |
| `src/third_party/blink/renderer/core/molt_fingerprinting/letterbox.{h,cc}`; `src/chrome/browser/molt_ai/fingerprinting/molt_letterbox.h`, `molt_letterbox_view.{h,cc}` | MoltNet screen size in the page's CSS pixels, pixel ratio, scrollbar thickness, whole viewport lengths (+ `letterbox_test.cc`); the browser-side letterbox geometry at any zoom and scale factor, margins, re-layout on zoom and page changes, background tabs (+ `molt_letterbox_unittest.cc`) |
| `src/third_party/blink/renderer/core/molt_fingerprinting/moltnet_device_values.{h,cc}`, `moltnet_device.{h,cc}`; `src/chrome/browser/molt_ai/fingerprinting/molt_net_device_values.h`, `molt_net_device.{h,cc}`, `molt_net_client_hints.{h,cc}`; `src/chrome/browser/molt_ai/tor/molt_net_window.cc` | MoltNet memory, client hints, `userAgentData` (pure, + `moltnet_device_values_unittest.cc`; glue, + `moltnet_device_test.cc`); touch / pointer / hover, CPU tier and navigation client hints in the browser (+ `molt_net_device_values_unittest.cc`); the size new windows open at |
| `src/third_party/blink/renderer/core/molt_fingerprinting/moltnet_network_values.h`, `moltnet_network.{h,cc}`; `src/chrome/browser/molt_ai/tor/molt_net_network_context_policy.h` | MoltNet's connection and network quality (pure, + `moltnet_network_values_unittest.cc`; glue, + `moltnet_network_test.cc`); no network-quality estimator in MoltNet's network context (+ `molt_net_network_context_policy_unittest.cc`) |
| `src/third_party/blink/renderer/core/molt_fingerprinting/moltnet_values.{h,cc}`, `moltnet_uniformity.{h,cc}` | MoltNet touches, screen details, orientation, en-US form locale (pure, + `moltnet_values_unittest.cc`; glue, + `moltnet_uniformity_test.cc`) |
| `src/third_party/blink/renderer/core/molt_fingerprinting/moltnet_display.{h,cc}` | MoltNet's display colour and scrollbar style (values in `moltnet_values.h`; + `moltnet_display_test.cc`) |
| `src/third_party/blink/renderer/core/molt_fingerprinting/moltnet_media_values.{h,cc}`, `moltnet_media.{h,cc}` | MoltNet's audio device, key systems, Media Capabilities table and WebCodecs codecs (pure, + `moltnet_media_values_unittest.cc`; glue, + `moltnet_media_test.cc`) |
| `src/third_party/blink/renderer/core/molt_fingerprinting/moltnet_audio_sink.{h,cc}`, `moltnet_audio_output.{h,cc}` | MoltNet's media playback clock and the ring it shares with Web Audio's (+ `moltnet_audio_sink_test.cc`) |
| `src/third_party/blink/renderer/core/molt_fingerprinting/moltnet_media_devices.{h,cc}` | MoltNet's device list, `devicechange` and `getUserMedia()` before a permission (rules in `moltnet_media_values.h`; + `moltnet_media_devices_test.cc`) |
| `src/third_party/blink/renderer/core/molt_fingerprinting/moltnet_memory_values.{h,cc}`, `moltnet_memory.{h,cc}`; `src/sandbox/policy/linux/molt_data_limit.h` | MoltNet's `performance.memory`, memory measurement and the 4 GiB machine V8 is configured as (pure, + `moltnet_memory_values_unittest.cc`; glue, + `moltnet_memory_test.cc`); the renderer's data limit on Linux (+ `chrome/common/molt_ai/fingerprinting/molt_data_limit_unittest.cc`) |
| `src/third_party/blink/renderer/core/molt_fingerprinting/moltnet_cpu.{h,cc}`; `src/third_party/skia/src/opts/SkOpts_moltbrowser.cpp` | MoltNet's CPU features, set before V8 starts: V8's flags, the heap's machine, skcms's and Skia's portable code (+ `moltnet_cpu_test.cc`); Skia's portable raster pipeline |
| `src/third_party/blink/renderer/core/molt_fingerprinting/moltnet_encoders.{h,cc}` | MoltNet's MediaRecorder encoders and software encoder threads (+ `moltnet_encoders_test.cc`) |
| `src/media/base/molt_portable_media.{h,cc}`, `molt_portable_scale.{h,cc}`; `src/third_party/blink/renderer/core/molt_fingerprinting/moltnet_pixels.{h,cc}` | MoltNet's media pixels and encoded bytes: the portable mode and the integer scaler (std-only; + `molt_portable_media_unittest.cc`, `molt_portable_scale_unittest.cc`) and the glue (+ `moltnet_pixels_test.cc`) |
| `src/third_party/blink/renderer/core/molt_fingerprinting/font_allowlists.{h,cc}`, `fonts.{h,cc}`, `system_fonts.{h,cc}` | font allow-lists and matcher, `local()` names included (pure, + `fonts_unittest.cc`) and their Blink glue (+ `system_fonts_test.cc`) |
| `src/third_party/blink/renderer/core/molt_fingerprinting/moltnet_font_set.{h,cc}`, `moltnet_font_set_table.cc`, `moltnet_fonts.{h,cc}` | MoltNet's bundled font set: its rules (pure, generated table, + `moltnet_font_set_unittest.cc`, `molt_moltnet_font_set_fuzzer.cc`) and the set in a renderer (+ `moltnet_fonts_test.cc`) |
| `src/chrome/browser/molt_ai/fingerprinting/molt_net_fonts.{h,cc}`, `molt_net_fonts_table.h` | opens the bundled set's files in the browser and hands MoltNet renderers read-only duplicates (+ `molt_net_fonts_unittest.cc`) |
| `tools/fonts/` | the set's pinned manifest, fetching, pinning, the generated tables, and the fresh-install font inventory (+ `fonts_test.py`) |
| `src/third_party/blink/renderer/core/molt_fingerprinting/moltnet_locale.{h,cc}` | MoltNet's default text locale (+ `moltnet_locale_test.cc`) |
| `src/third_party/blink/renderer/core/molt_fingerprinting/moltnet_text_values.{h,cc}`, `moltnet_text.{h,cc}` | MoltNet's text metrics: the grid and rounding (pure, + `moltnet_text_values_unittest.cc`) and their Blink glue, the Linux rendering and emoji pin (+ `moltnet_text_test.cc`) |
| `src/chrome/renderer/molt_ai/fingerprinting/moltnet_locale_strings.{h,cc}`, `moltnet_locale_strings_table.cc` | MoltNet's en-US renderer strings (generated table, + `moltnet_locale_strings_unittest.cc`) |
| `src/chrome/browser/molt_ai/tor/molt_net_locale_prefs.{h,cc}`, `molt_net_locale_prefs_table.h` | MoltNet's fixed default fonts, minimum logical font size and encoding (generated table, + `molt_net_locale_prefs_unittest.cc`) |
| `tools/moltnet_locale/generate_tables.py` | generates and checks both tables from the pinned `.grd` files |
| `src/third_party/blink/public/web/web_molt_fingerprinting.h` | embedder entry point (mode, seed, a document's Shields decision) |
| `src/chrome/common/molt_ai/fingerprinting/` | feature, mode switch, `molt_fingerprinting.mojom` |
| `src/chrome/browser/molt_ai/fingerprinting/` | session seed, mode switch, seed over mojo, chrome://flags entry |
| `src/chrome/browser/molt_ai/fingerprinting/molt_shields_decision.{h,cc}`, `molt_shields_delivery.{h,cc}` | the Shields decision for a top-level origin or a worker's storage key (+ `molt_shields_decision_unittest.cc`) and its delivery at each commit (`ShieldsDecisionObserver`) |
| `src/chrome/browser/molt_ai/shield/molt_shield_site_exceptions.{h,cc}` | whether a site has Shields off, from content settings (+ unit test) |
| `src/chrome/renderer/molt_ai/fingerprinting/` | hands mode and seed to Blink; `molt_shields_decision_agent.{h,cc}` hands each new document its Shields decision |
| `src/chrome/browser/molt_ai/tor/molt_net_routing.cc` | MoltNet `Accept-Language` |
| `src/chrome/browser/molt_ai/fingerprinting/molt_net_storage_values.h`, `molt_net_storage.{h,cc}` | MoltNet's fixed quota and blob limits (+ `molt_net_storage_values_unittest.cc`, `molt_net_storage_unittest.cc`) |
| `src/storage/browser/quota/molt_storage_budget_policy.h`, `molt_storage_budget.h` | MoltNet's storage budget per site, and no eviction for the pool (+ `molt_net_storage_budget_policy_unittest.cc`) |
| `src/chrome/browser/molt_ai/tor/molt_net_isolation_policy.{h,cc}`, `molt_net_isolation.{h,cc}` | which regular-profile prefs MoltNet uses MoltBrowser's defaults for (pure, + `molt_net_isolation_policy_unittest.cc`) and MoltNet's pref service (+ `molt_net_isolation_unittest.cc`) |

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
`chrome/browser/universal_web_contents_observers.cc` (background tabs),
`chrome/browser/profiles/off_the_record_profile_impl.cc`,
`media_values.cc`, `html_image_element.cc`, `style_resolver_state.cc`;
MoltNet device and client hints: `frame_fetch_context.cc`,
`navigator_ua.cc`, `navigator_device_memory.cc`,
`content/public/browser/client_hints_controller_delegate.{h,cc}`,
`content/browser/client_hints/client_hints.cc`,
`chrome/browser/client_hints/client_hints_factory.cc` and `BUILD.gn`;
MoltNet connection and network quality:
`platform/network/network_state_notifier.{h,cc}`,
`services/network/network_context.cc`, `network_context.mojom`;
MoltNet touches, screens and forms: `touch_event_manager.cc`,
`pointer_event_factory.cc`, `window_screen_details.cc`,
`permission_status_listener.cc`, `screen_orientation.cc`,
`screen_orientation_controller.cc`, `platform_locale.{h,cc}`; MoltNet's
display colour and scrollbar style: `media_values.cc`,
`core/exported/web_view_impl.cc`; MoltNet's audio device, key systems,
Media Capabilities and WebCodecs codecs: `audio_context.cc`,
`realtime_audio_destination_handler.cc`, `encrypted_media_utils.cc`,
`media_capabilities.cc`, `gpu_factories_retriever.cc`,
`video_decoder_broker.cc`; MoltNet's media playback clock, device list
and memory: `audio_device_factory.cc`,
`html_media_element_audio_output_device.cc`, `media_devices.cc`,
`user_media_processor.cc`, `memory_info.cc`,
`measure_memory_controller.cc`; MoltNet's MediaRecorder encoders:
`video_track_recorder.cc`, `media_recorder_handler.cc`,
`media/base/video_encoder.{h,cc}`; MoltNet's media pixels and encoded
bytes: `media/renderers/default_decoder_factory.cc`,
`media/gfx/mappable_shared_image_video_frame_pool.cc`,
`media/gfx/paint_canvas_video_renderer.cc`,
`media/base/video_frame_converter_internals.cc`,
`media/base/sinc_resampler.cc`, `media/base/vector_math.cc`,
`media/base/BUILD.gn`, `third_party/opus/BUILD.gn`,
`third_party/opus/src/celt/celt.c`, `celt/x86/x86cpu.c`,
`celt/arm/pitch_neon_intr.c`, `celt/arm/celt_neon_intr.c`; MoltNet's
CPU features and V8's heap: `v8/src/flags/flag-definitions.h`,
`v8/src/wasm/function-body-decoder-impl.h`, `v8/src/base/ieee754.{h,cc}`,
`v8/src/init/v8.cc`, `gin/public/isolate_holder.h`, `gin/isolate_holder.cc`,
skcms's `skcms.cc`, `skcms_public.h`, `skcms_Transform.h`,
`Transform_inl.h`, Skia's `SkGraphics.{h,cpp}`, `SkRasterPipeline_opts.h`,
`skia/BUILD.gn`, symphonia-core's `src/dsp/fft/simd.rs` (and its crate
patch), `media/filters/symphonia_decoder_bridge.rs`,
`media/filters/symphonia_audio_decoder.cc`,
`sandbox/policy/linux/sandbox_linux.cc`, `sandbox/policy/BUILD.gn`; MoltNet's
storage limits:
`content/browser/storage_partition_impl.cc`,
`content/browser/blob_storage/chrome_blob_storage_context.cc`,
`storage/browser/blob/blob_memory_controller.{h,cc}`,
`storage/browser/quota/quota_settings.h`, `quota_manager_impl.cc`,
`quota_temporary_storage_evictor.cc`; MoltNet
default text locale: `platform/language.{h,cc}`; MoltNet text metrics:
`base_rendering_context_2d.cc` (`measureText`), `text_metrics.{h,cc}`,
`platform/fonts/font_platform_data.{h,cc}`,
`platform/fonts/linux/font_cache_linux.cc`; MoltNet settings
isolation: `off_the_record_profile_impl.cc` (constructor),
`chrome/browser/profiles/BUILD.gn`; Shields decisions:
`chrome/browser/universal_web_contents_observers.cc`,
`chrome/browser/chrome_content_browser_client.h`,
`content/public/browser/content_browser_client.{h,cc}`,
`content/browser/service_worker/service_worker_version.cc`,
`content/browser/worker_host/shared_worker_host.cc`,
`service_worker.mojom`, `shared_worker_info.mojom`,
`service_worker_global_scope.h`, `dedicated_worker.cc`, and the test
fakes `fake_service_worker.{h,cc}`, `service_worker_job_unittest.cc`,
`web_embedded_worker_impl_test.cc`.

## Follow-ups

1. Build and run everything above on the build machine; add browser tests
   (canvas/WebGL/audio values stable within a site, different across sites
   and sessions, unfarbled with Shields off; MoltNet values uniform; the
   seed absent from renderer command lines).
2. Canvas-to-video export in normal windows. The farbling loop is now 2-4x
   faster (see Cost), which puts a 720p frame at about 1-5 ms of CPU; what
   is left is the synchronous GPU readback per frame, which needs measuring
   on real hardware (720p30 and 1080p30/60 `captureStream` /
   `new VideoFrame(canvas)` test pages, exports forced on and off). The
   better design is still to farble where a script reads pixels
   (`VideoFrame.copyTo`, `MediaStreamTrackProcessor`) instead of on every
   frame.
3. (Dropped: the channel vote, see known attacks. A real answer to
   scripts written against the noise is a different design, such as a
   prompt before canvas readback, not a noise revision.)
4. (Done: third-party shared workers use their top-level site's key, see
   [Shared workers](#shared-workers).)
5. (Done: real letterboxing of MoltNet windows, CSS device media queries,
   `devicePixelRatio`, no DPR client hints, new windows that open at a
   1400 x 900 page, letterboxing in CSS pixels so zoom stops showing,
   classic scrollbars 16 CSS pixels at every zoom and on every OS, and
   whole-pixel media queries, `vw`/`vh` and `visualViewport`, see
   [Letterboxing](#letterboxing-moltnet), [Zoom](#zoom) and [client
   hints](#client-hints).) Left: the checks on the build machine listed
   there; closing the moment between a new zoom and the new size, which
   needs content to send both in one visual-properties update (a new
   content hook, see [Zoom](#zoom)).
6. Fonts: the allow-lists are in (see [Fonts](#fonts)), and MoltNet takes
   every font from a set MoltBrowser ships, the same on every OS ([the
   bundled font set](#moltnet-the-bundled-font-set)); text metrics in
   MoltNet are done ([Text metrics](#text-metrics-moltnet)). Still to do:
   run `tools/fonts/molt_font_inventory.py` on fresh installs of each
   supported Windows, macOS and Linux version and correct the allow-lists
   (normal windows, and MoltNet in a build without the set); compare the
   set with Tor Browser's at the next font update. The WebGL parameter surface
   and WebGPU in MoltNet are done (fixed WebGL baselines, no WebGPU
   adapter); left: a mobile look at the baselines if MoltNet comes to
   Android, and checking them against the WebGL reports of real Macs,
   Windows and Linux machines in MoltNet windows.
7. (Done: Shields off is decided by the browser per document and per
   worker and sent only with it; no renderer holds the list, see
   [Shields off](#shields-off).)
8. MoltNet: done: touch coordinates and contact, the Window Management
   API, screen orientation, form-control formats ([MoltNet: touches,
   screens, orientation and form
   controls](#moltnet-touches-screens-orientation-and-form-controls)),
   renderer strings and the default text locale in en-US ([the browser's
   UI language](#moltnet-the-browsers-ui-language)), a touchscreen-less
   look ([Touchscreens](#touchscreens)),
   permission state and user settings that no longer come from the regular
   profile ([settings from the regular
   profile](#moltnet-settings-from-the-regular-profile)), one fixed
   connection and network quality ([connection and network
   quality](#moltnet-connection-and-network-quality)), one display
   colour and scrollbar style per platform ([MoltNet's
   display](#moltnets-display-colour-and-scrollbar-style)), and one audio
   device, no key systems and fixed media answers ([audio, DRM and media
   capabilities](#moltnet-audio-drm-and-media-capabilities)).
9. Repeat the [Chromium-update audit](#re-checked-at-chromium-156) at every
   rebase. To watch: `renderSizeHint: "hardware"` (128 at 156, and
   pinned in MoltNet; a later Chromium may read the audio hardware's
   buffer size there for normal windows), new readers of the audio
   device's parameters, new key systems or Media Capabilities fields,
   `encodingInfo()` of type `"record"`, and the features that table lists
   as behind flags, once they ship. When libyuv, libopus or libvpx roll,
   repeat the comparisons of [pixels and encoded
   bytes](#moltnet-pixels-and-encoded-bytes) (every SIMD level of x86-64
   and arm64), and check that new decoder or frame-pool paths keep
   MoltNet's video in software and in CPU memory. When V8, Skia, skcms,
   Symphonia, rustfft or FFmpeg roll, repeat the comparisons of [CPU
   features](#moltnet-cpu-features) and look for new instructions or code
   paths picked by CPU feature (new relaxed instructions, new `SkOpts`
   entries, new uses of the C library's math in V8).
