# Web-standard AI APIs on the on-device model

Status: implemented (October 2026), pending a compile on the build machine
and the in-browser checks in §12. Remembered answers, the prompt's pause
and the settings page (§4.1-§4.5) and LanguageDetector's "unavailable"
(§10) followed the same month. Roadmap item "Web-standard AI APIs,
without Google". The Prompt API (`LanguageModel`), `Summarizer`, `Writer`
and `Rewriter` run on the user's own on-device model through
`BrowserAIRuntime`, never on upstream's optimization-guide model. Translator
and LanguageDetector are not served (§10).

Pin: Chromium 152.0.7977.169 (`92a6f151`). Upstream files below were read
at that revision with `upshow new`.

## 0. Decisions in one page

1. **One seam: the browser's `AIManager`.** Blink is unchanged for these
   APIs (one Blink hook turns `LanguageDetector` off, §10). Upstream's
   `ChromeContentBrowserClient::BindAIManager` creates Chrome's `AIManager`;
   MoltBrowser's body creates `MoltAIManager` instead
   (`chrome/browser/molt_ai/web_ai/`), which implements
   `blink.mojom.AIManager` and the session interfaces on
   `BrowserAIRuntime`. Upstream's `AIManager`, `AILanguageModel` and the
   optimization guide's on-device model are never reached from a page (§2).
2. **The model is the user's.** Pages use the model that is loaded (so they
   never swap out the chat's model); with none loaded, the chat's default
   model from `settings.json`, else the smallest downloaded one. A page
   never starts a download.
3. **Nothing happens without the user's yes.** A site the user hasn't
   allowed sees `"downloadable"`, so Blink requires a user activation before
   `create()` reaches the browser; `create()` then shows an infobar naming
   the top-level origin: *Allow* / *Block*, or *Choose a model* / *Block*
   when no model is downloaded (it opens the model picker). Answers are
   remembered per profile and per top-level origin: in a MoltBrowser pref
   for a regular profile, in memory for Incognito. Closing the bar three
   times pauses it for the site for a week. The user reviews and removes
   answers in `molt://ai-settings` ("Sites using on-device AI"); removing
   an allowed site ends its sessions. No model is loaded for a site before
   it is allowed (§4).
4. **Few, uniform states.** `availability()` is `"unavailable"`,
   `"downloadable"` or `"available"`; never `"downloading"`. `inputQuota` is
   1024 or 4096 tokens for `LanguageModel` (by the loaded model's context)
   and 9216 for the writing APIs (fixed in Blink). Token counts come from a
   model-independent estimator, and sampling parameters are the same for
   everyone. No model name, size or exact context window reaches a page.
5. **Off where it can't be private.** `"unavailable"` in MoltNet windows,
   in workers, in documents whose top-level page isn't http(s), under the
   enterprise policies upstream honours, with Chromium's on-device AI
   switch off, and with `"web_ai_apis": false` in `settings.json` (the
   switch in `molt://ai-settings`).
6. **Shields are not consulted.** The APIs work in regular and Incognito
   windows whatever the site's Shields setting; fingerprinting is limited
   by the bucketing above, not by Shields (§8).
7. **Pages share the one model with the chat and come second.** One page
   generation at a time; visible pages first; round-robin between sites;
   caps per site, per session and in total; a five-minute deadline; no
   hidden reasoning. Closing the page, navigating away, `destroy()` or an
   `AbortSignal` cancels the generation in the inference service.
8. **Page input is hostile.** Sizes are checked before text is read; text
   is made valid UTF-8 without NULs; every page string goes to the model as
   untrusted content (the page's system prompt included), never as
   MoltBrowser's instructions.
9. **Exposure.** `Writer` and `Rewriter` are turned on by default (at 152
   they are origin-trial only), so all four interfaces exist on every page
   and their presence says nothing about the user. The Translator API is
   turned off by default (its upstream backend downloads from Google,
   outside Tor in MoltNet). `LanguageDetector` reports `"unavailable"` in
   every window for every user (§10).
10. **Upstream files touched:** `chrome/browser/chrome_content_browser_client.cc`
    (one hook) and `components/infobars/core/infobar_delegate.h` (one enum
    value), both already MoltBrowser-patched; for the remembered answers,
    `chrome/browser/browsing_data/chrome_browsing_data_remover_delegate.cc`
    (two calls), `chrome/browser/profile_resetter/profile_resetter.cc` (one
    call) and `chrome/browser/profiles/chrome_browser_main_extra_parts_profiles.cc`
    (the factory, already patched); for `LanguageDetector`,
    `third_party/blink/renderer/modules/ai/ai_interface_proxy.cc` (two
    function bodies).

```
renderer (Blink, unchanged)                 browser process
---------------------------                 ---------------
LanguageModel / Summarizer /   AIManager    MoltAIManager (per document)
Writer / Rewriter  ---------- mojo -------> |  availability: policy facts + device facts + consent
                                            |  create: consent infobar -> PrepareModel -> session
AILanguageModel, AISummarizer, ...  <------ |  sessions: quota, queue, prompts (web_ai_*.cc)
ModelStreamingResponder  <-- deltas, 200 ms |
                                            v
                                RuntimeWebAIBackend (process-wide)
                                |  GenerationScheduler: one page generation at a time
                                v
                                BrowserAIRuntime  (shared with the AI chat)
                                |  ChatPrompt with trust labels, request ids, cancel
                                v
                                MoltInferenceService (sandboxed utility process)
```

## 1. What Chromium 152 has

**Interfaces and features** (`runtime_enabled_features.json5`,
`third_party/blink/public/common/features.cc`):

| Web API | Blink feature at 152 | Upstream backend | MoltBrowser |
|---|---|---|---|
| `LanguageModel` (Prompt API) | `AIPromptAPI`, stable (Windows, macOS, Linux, ChromeOS); `AIPromptAPIMultimodalInput` and `AIPromptAPIStructuredOutput` stable; tool use experimental | `AIManager` → `AILanguageModel` → optimization guide on-device session (Gemini Nano, fetched by the component updater) | `MoltAIManager` → `LanguageModelSession` → `BrowserAIRuntime` |
| `Summarizer` | `AISummarizationAPI`, stable | `AIManager` → `AISummarizer` | `SummarizerSession` |
| `Writer` | `AIWriterAPI`, experimental + origin trial | `AIWriter` | `WriterSession`, enabled by default (§9) |
| `Rewriter` | `AIRewriterAPI`, experimental + origin trial | `AIRewriter` | `RewriterSession`, enabled by default (§9) |
| `Proofreader` | experimental + origin trial | `AIProofreader` | always `"unavailable"` |
| Embeddings | test only | `AISemanticEmbedder` | always `"unavailable"` |
| `Translator` | `TranslationAPI`, stable | `TranslationManagerImpl` + TranslateKit components | off by default (§10) |
| `LanguageDetector` | `LanguageDetectionAPI`, stable | `ContentLanguageDetectionDriver` + optimization guide model | always `"unavailable"` (§10) |

The `*ForWorkers` variants are off for web pages (extensions turn them on
in `ChromeContentRendererClient`). `AIClassifierAPI` is experimental (flag
only) and not part of this work.

**Binding.** Content binds `blink.mojom.AIManager` through
`ContentBrowserClient::BindAIManager(browser_context, context_user_data,
rfh, receiver)`: for documents with the document's
`DocumentAssociatedData` as user data, for dedicated and shared workers with
the worker host and `rfh == nullptr`, and for service workers through
`ServiceWorkerHost::BindAIManager` (`browser_interface_binders.cc`).
Chrome's override stores one `AIManager` under `kAIManagerUserDataKey`.

**Blink behaviour MoltBrowser relies on** (`modules/ai/`):

- `availability()` maps the 23 `ModelAvailabilityCheckResult` values to
  four strings; every `kUnavailable*` reason becomes `"unavailable"` and
  only shows in the console.
- `create()` calls the matching `CanCreate*` first. When the answer is
  `"downloadable"` it requires user activation (`RequiresUserActivation`);
  with `AIRelaxUserActivationReqs`, enabled by default at 152, sticky
  activation is enough (the user has interacted with the frame).
- The renderer dispatches `downloadprogress` 0 and 1 itself when
  `create()` succeeds; the browser only reports real progress, which
  `CreateMonitor` checks against a fixed total.
- Streaming is a series of deltas (`OnStreaming`); aborting a request
  closes its `ModelStreamingResponder` pipe.
- The writing APIs' `inputQuota` is the Blink constant
  `kWritingAssistanceMaxInputTokenSize` (9216); usage is measured by the
  browser.
- `responseConstraint` (JSON schema) is turned into prompt text by Blink
  before the prompt is sent.
- Answers `kErrorInvalidRequest` and `kErrorRequestThrottled` make the
  renderer file a crash report (`DumpWithoutCrashing`), so MoltBrowser uses
  them only where upstream does.

**Policies and switches upstream's `AIManager` honours:**
`BuiltInAIAPIsEnabled` (profile pref `policy.built_in_ai_apis_enabled`),
`GenAILocalFoundationalModelSettings` (local state; `1` disallows), the
"On-device AI" user switch (local state), and the permissions-policy
features `language-model`, `summarizer`, `writer`, `rewriter`.
`MoltAIManager` honours the same ones.

## 2. The seam, and why this one

Three places could have carried the change:

1. **Inside upstream's `AIManager` and session classes**, swapping the
   optimization guide's `OnDeviceSession` for `BrowserAIRuntime`. Rejected:
   it would patch `ai_manager.cc`, `ai_language_model.cc`,
   `ai_summarizer.cc`, `ai_writer.cc`, `ai_rewriter.cc`,
   `ai_context_bound_object*` and the optimization guide's model execution
   code; the session API (`Append`/`Generate` on a token context) doesn't
   map onto `ChatPrompt` with trust labels; and every code path left
   unpatched could still ask the component updater for Google's model
   (upstream's manager also starts an eager model init).
2. **In Blink.** Rejected: the renderer is the untrusted side, and Blink
   already does the spec work (options, events, errors, abort).
3. **At `BindAIManager`.** Chosen. One function body changes; the mojo
   interface is the contract Blink already speaks; upstream's backend is
   never constructed for a page, so it can't download or load anything on
   a page's behalf. The cost is reimplementing the browser half of the mojo
   surface, which is also where the privacy decisions (consent, bucketing,
   limits) have to live anyway.

The hook keeps upstream's `kAIManagerUserDataKey`, so the manager lives and
dies with the document (or worker) exactly as upstream's did.

## 3. Code

All new code is under `src/chrome/browser/molt_ai/web_ai/`:

| File | What it is |
|---|---|
| `web_ai_policy.{h,cc}` | Pure: the availability state machine, create actions, quota buckets, token estimator, sampling clamps, language tags, model pick, `GenerationScheduler`. |
| `web_ai_site_permissions.{h,cc}` | Pure: `SitePermissionTable`, the per-site answers and the prompt's pause (embargo), their bounds and stored form (§4.1-§4.2). |
| `web_ai_prompts.{h,cc}` | Pure: Summarizer/Writer/Rewriter options → system prompt, context and reply budget. |
| `web_ai_conversation.{h,cc}` | Pure: a `LanguageModel` session's context, quota accounting and eviction. |
| `web_ai_backend.{h,cc}` | The interface between the manager and the runtime (`GenerationJob`, device facts, cancel). |
| `molt_ai_manager.{h,cc}` | `MoltAIManager`: `blink.mojom.AIManager`, the create flow. |
| `web_ai_sessions.{h,cc}` | `LanguageModelSession`, `SummarizerSession`, `WriterSession`, `RewriterSession`: queueing, streaming, abort. |
| `molt_ai_manager_binder.{h,cc}` | `BindMoltAIManager()`: the facts about a document (MoltNet, policies, permissions policy, top-level origin, visibility). |
| `web_ai_consent.{h,cc}` | The consent infobar. |
| `web_ai_site_permissions_service.{h,cc}` | `WebAISitePermissions`: a profile's table, kept in the `molt_ai.web_ai.sites` pref (or in memory), with observers and the Clear browsing data operations. |
| `web_ai_site_permissions_factory.{h,cc}` | Its keyed-service factory (pref registration, which profiles get one) and the hooks Clear browsing data and profile reset call. |
| `web_ai_settings_switch.{h,cc}` | Reads and writes `"web_ai_apis"` in `settings.json`, keeping every other key. |
| `runtime_web_ai_backend.{h,cc}` | `RuntimeWebAIBackend` over `BrowserAIRuntime`: device facts, model load, the scheduler, deadlines. |

`BrowserAIRuntime` gained `GetLoadedContextTokens()` (the loaded model's
context size, for the quota bucket). The settings section is
`src/chrome/browser/ui/webui/molt_ai/molt_web_ai_sites_handler.{h,cc}`
(§4.5).

## 4. Availability, consent and the model

`DecideAvailability()` (`web_ai_policy.cc`), first match wins:

| Condition | `availability()` | `create()` |
|---|---|---|
| Feature off (`chrome://flags`, `--disable-features`) | the interface is absent (the browser would answer `"unavailable"`) | - |
| Enterprise policy disallows | `"unavailable"` | fails |
| MoltNet, a worker, top-level page not http(s), the On-device AI switch off, or `"web_ai_apis": false` | `"unavailable"` | fails |
| Options not supported (§5) or an invalid language tag | `"unavailable"` | fails |
| The user blocked this site, or the prompt is paused for it (§4.2) | `"unavailable"` | fails |
| The device can't run on-device inference, or no catalog model fits its RAM and none is downloaded | `"unavailable"` | fails |
| The site isn't allowed yet, or no model is downloaded | `"downloadable"` | needs user activation; shows the infobar |
| Allowed and a model is downloaded | `"available"` | loads the model if needed and creates |

The feature, policy, MoltNet, worker, top-level-page and On-device AI
checks are answered without asking the runtime anything, so a MoltNet page
never touches it (`settings.json` is read by the runtime side).

**The create flow** (`MoltAIManager::StartCreate`): requests that can never
succeed fail first (initial prompts over the large quota, prompts that
aren't text, shared context over 9216 tokens); then device facts; then the
consent infobar if the site isn't allowed; then `PrepareModel` (load if
nothing is loaded); then the answer is checked again (it may have been
taken back while the model loaded); then the session. If the page aborts
`create()` or goes away at any step, the request is dropped: no infobar
answer is needed and no model is loaded.

**The infobar** (`web_ai_consent.cc`) names the top-level origin, so an
iframe asks on behalf of the page the user sees. Only the tab's current
page can raise it (not a prerendered page or one in the back/forward
cache). Requests from the same tab and origin share one infobar. *Allow*
and *Block* are remembered (§4.1). Closing the infobar with its close
button answers nothing and counts towards a pause (§4.2); an infobar that
goes away by itself (the page navigates, the tab closes, a newer infobar
replaces it) answers nothing and doesn't count. With no model downloaded,
*Choose a model* records the allow and opens `molt://ai`, where the user
picks and downloads a model; the pending `create()` fails, and the page can
try again once the model is there.

Why an infobar and not upstream's model-download UX: upstream has none (a
gesture alone lets any page start a multi-gigabyte download), and a
Chromium permission type would mean a new `ContentSettingsType`,
permission-context plumbing and settings UI across many upstream files.
The infobar is MoltBrowser-only code, asks at the moment of use, and the
gesture rule keeps a page from raising it unprompted. Remembering the
answers and reviewing them don't need a permission type either (§4.4).

### 4.1 Remembered answers

`WebAISitePermissions` (`web_ai_site_permissions_service.{h,cc}`) is a
keyed service per profile, made by `WebAISitePermissionsFactory`, over the
pure `SitePermissionTable` (`web_ai_site_permissions.{h,cc}`).

- **Key:** the top-level origin as it serializes (`https://example.com`,
  `http://example.com:8080`); only http(s) origins have one. Origin, not
  site: `sub.example.com` asks for itself, as Chromium's permissions do.
- **Regular profile:** the profile pref `molt_ai.web_ai.sites`, written on
  every change. An answer lasts until the user removes it on the settings
  page (§4.5), clears "Site settings" in Clear browsing data, or resets
  the profile.
- **Incognito and Guest:** their own table, in memory only, starting
  empty: nothing is read from or written to the regular profile, and it
  goes when the window closes.
- **MoltNet:** no table; the APIs are off there.
- **Bounded:** 512 sites. Past that the oldest prompt history (§4.2) goes
  first, then the oldest answer.
- **Taking an answer back** (Remove, Remove all, Clear browsing data, a
  profile reset, or Block in a later prompt): every `MoltAIManager` of the
  profile hears about the change (`WebAIEnvironment::ObserveConsent`); one
  whose site is no longer allowed ends its sessions (running generations
  are cancelled in the inference service and the session pipes close, so
  the page's next call fails), and a `create()` whose model is still
  loading fails.

**Stored form** (format 1):

```
{"version": 1,
 "sites": {"https://example.com":    {"answer": "allow", "answered": 1792627200000},
           "https://other.example":  {"answer": "block", "answered": 1792627200000},
           "https://paused.example": {"dismissals": 3, "last_dismissal": 1792627200000}}}
```

Times are milliseconds since the Unix epoch. Format 1 is the first that is
stored (before it, answers lived in memory for the browser session), so
there is nothing older to migrate. Reading trusts nothing in the file:

- no version, or one below 1: nothing is read; a later version: the fields
  this code knows are read (a later format may add fields, not change
  these);
- each key must be exactly how an http(s) origin serializes (no path,
  default port, upper case or other scheme);
- `"answer"` other than `"allow"` / `"block"`, a count below 1 or a missing,
  NaN, negative or out-of-range time drops the entry; counts are capped at
  100;
- an answer's time is clamped to now (a missing or bad one counts as now,
  so clearing "the last hour" removes it rather than keeping it forever),
  and prompt history stored beside an answer is dropped.

Reading never writes; the next change writes the table back in format 1.

### 4.2 Pausing the prompt (embargo)

Closing the infobar with its close button three times pauses it for the
site for seven days. The count isn't reset when a pause ends, so each
further close pauses it for another seven days. An answer (Allow or Block)
clears the count; infobars that go away by themselves don't count.

Why these numbers: they are Chromium's for permission prompts
(`PermissionDecisionAutoBlocker`: `kDefaultDismissalsBeforeBlock` 3,
`kDefaultEmbargoDays` 7, the count kept after an embargo), so the bar
behaves like any permission prompt. One or two stray closes don't silence
a site. Three caps the nuisance: Blink only asks for *sticky* activation
before `create()` reaches the browser (`AIRelaxUserActivationReqs`), so
after a single click a page could otherwise put the bar back up as soon as
it is closed. A week is long enough that a site that keeps asking stops
being a bother and short enough that a site the user later wants can ask
again without a trip to settings; the settings page lists paused sites and
lifts a pause with Remove.

A clock that moves never stretches a pause: it ends seven days after the
stored time, or as soon as the clock reads more than seven days before it.

### 4.3 What a page can tell apart

| Site's state | `availability()` | `create()` after a click |
|---|---|---|
| Never asked, or the bar closed fewer than three times | `"downloadable"` | the infobar |
| Allowed (and a model downloaded) | `"available"` | creates |
| Blocked | `"unavailable"` | fails at once, no infobar |
| Paused | `"unavailable"` | fails at once, no infobar |

- **Blocked and paused look the same** on purpose: the user's explicit
  "no" can't be told from a few closes, and both look like every other
  `"unavailable"` (a device that can't run a model, the switch off, a
  policy). The reason goes only to the DevTools console, the same one for
  blocked and paused.
- **Closed and blocked differ**, as before: after a close the site still
  sees `"downloadable"`, after *Block* `"unavailable"`. This only tells the
  site what the user did on it.
- **A pause ends:** a week later the site sees `"downloadable"` again and
  can infer that the earlier `"unavailable"` was a pause. Chromium's
  embargo shows the same.
- **Per top-level origin:** a site learns only what the user did on it.
  An iframe the page delegates to (permissions policy) sees the page's
  state.
- **Persistence:** `"available"` now survives a restart, like a site
  permission, and survives clearing cookies (as permissions do); "Site
  settings" in Clear browsing data removes it. It is a bit the user set,
  not something a site can write, so it can't serve as an identifier.
- **Incognito** starts with no answers and never sees the regular
  profile's, so a site can't link an Incognito visit to the regular
  profile through them.

### 4.4 Where the answers live, and why not a content setting

Two ways were weighed: a new `ContentSettingsType`, or a dictionary pref
MoltBrowser owns. The pref won:

- A new type is an enum value in `content_settings_types.mojom` (which
  recompiles much of the tree, and the generated Java on Android), its
  registration in the content-settings registry, the histogram enum and
  the UMA mapping with its size check, and the site-settings WebUI's type
  lists; and the pause would still need storage of its own
  (`PermissionDecisionAutoBlocker` works only for permission types, which
  means `permission_util.cc` and a permission context too). Every Chromium
  rebase would carry all of it.
- The pref is MoltBrowser code: registered by the factory's
  `RegisterProfilePrefs` (no edit to `browser_prefs.cc`), versioned,
  checked on load, and holding answers and pauses together.
- What it costs: three one-line hooks, and no entry in
  `chrome://settings/content` or the page info bubble (`molt://ai-settings`
  is where the answers are listed):
  - `ChromeBrowsingDataRemoverDelegate`: answers with "Site settings"
    (`DATA_TYPE_CONTENT_SETTINGS`), honouring the time range and the site
    filter; pauses with history or site usage data, whatever the time
    range, as Chromium clears its embargoes;
  - `ProfileResetter::ResetContentSettings`: both;
  - `EnsureBrowserContextKeyedServiceFactoriesBuilt`: the factory, so the
    pref is registered before profiles load.
- **Sync:** the pref is registered without `SYNCABLE_PREF`, so it is never
  synced.

### 4.5 The settings page

`molt://ai-settings`, "Sites using on-device AI" (deep link
`molt://ai-settings/?section=webai`; handler
`molt_web_ai_sites_handler.{h,cc}`):

- **"Let sites ask to use on-device AI"** is `"web_ai_apis"` in
  `settings.json` (all profiles). It applies to the next page request (the
  runtime's cached device facts are dropped). It is written by
  read-modify-write, keeping every other key; a `settings.json` that isn't
  a JSON object is left alone and the section says so. The page's *Save
  Settings* used to rewrite `settings.json` with only the form's keys,
  which turned the APIs back on; it now merges the form over the file, and
  *Reset to Defaults* keeps the switch.
- **Allowed, Blocked, Not asking for now** (with the date a paused site
  may ask again), each with *Remove*, and *Remove all*. The page shows the
  regular profile's answers, also when opened from an Incognito or MoltNet
  window (Incognito's own answers aren't listed; they go when it closes);
  a Guest session shows its own, with a note that they last until it
  closes. The list refreshes when answers change elsewhere.
- **WebUI rules:** the strict molt CSP; controls wired with `data-on-*`
  and `moltActions()`; the DOM built with `createElement` and
  `textContent` (no HTML sink); every message checked with
  `HasCallbackArgs()`, `BoolArg()` and `BoundedStringArg()`; *Remove* only
  removes a key that exactly matches a stored one.

## 5. Session semantics

**`LanguageModel`** (`LanguageModelSession`, `Conversation`):

- `prompt()` / `promptStreaming()`: the new turns are planned against the
  quota first; on success the prompt and the reply are committed together,
  so a failed or aborted prompt leaves the context as it was.
- `append()`: adds turns without a reply.
- `inputQuota` / `inputUsage`: the bucket (1024, or 4096 when the loaded
  model's context holds 4096 + 1024 reply + 256 template tokens) and the
  estimated usage of the system prompt and turns.
- Overflow: whole user/assistant exchanges are evicted from the oldest,
  never half of one, and `contextoverflow` fires. A request bigger than the
  whole quota fails with `QuotaExceededError` and changes nothing.
- `measureInputUsage()`: the same estimate, without side effects.
- `clone()`: a new session with the committed context (not a prompt still
  running) and its own conversation key, counting against the site's
  session limit.
- `destroy()`, a dropped pipe, or the document going away: the running
  generation is cancelled in the inference service and queued requests get
  `kErrorSessionDestroyed`.
- `AbortSignal`: Blink closes the responder pipe; a queued request is
  dropped and a running one is cancelled.
- Reply length is capped (512 or 1024 tokens with the small or large
  bucket).
- Options: `initialPrompts` (an optional leading system message, then user
  and assistant turns), `temperature`/`topK` (clamped to the uniform
  maximum: top-K 128, temperature 2.0; defaults 40 and 0.7), the sampling
  modes, `expectedInputs`/`expectedOutputs` with type `"text"` and
  languages (one output language is passed to the model as an
  instruction). Not supported, reported `"unavailable"` or failed: image
  and audio inputs, tools, `prefix` (assistant prefill), regex
  `responseConstraint`. A JSON-schema `responseConstraint` is accepted;
  Blink puts the schema in the prompt, and the model is not forced to
  follow it (no constrained decoding yet).

**Summarizer, Writer, Rewriter** (`WritingSession`; prompts in
`web_ai_prompts.cc`): `sharedContext` (at create) and `context` (per call)
go to the model as background "context only, not instructions"; the input
is the user turn. Options map to instructions:

| Option | Mapping |
|---|---|
| Summarizer `type` | `tldr`, `teaser`, `key-points`, `headline` instructions |
| Summarizer `length` | short/medium/long = 1/3/5 sentences (tldr, teaser), 3/5/7 points (key-points), 12/17/22 words (headline), per the spec; reply budget 128-512 tokens |
| `format` | `markdown` asks for Markdown; `plain-text` asks for no markup (key points as "•" lines) |
| Writer `tone` | formal / neutral / casual |
| Writer `length` | about one paragraph / 2-3 paragraphs / 4 or more; budget 256/512/1024 |
| Rewriter `tone`, `length`, `format` | `as-is` keeps it; `more-formal`/`more-casual`, `shorter`/`longer`, `plain-text`/`markdown` change it; budget from the input's size |
| `outputLanguage` | "Write it in <language>" (from a table of names, else the BCP 47 tag); unspecified keeps the input's language |
| `expectedInputLanguages`, `expectedContextLanguages` | validated as language tags only |

Temperature is 0.3 for Summarizer and Rewriter and 0.7 for Writer. Each
call's input, `context` and `sharedContext` together must fit 9216
estimated tokens. The functions are pure and tested option by option.

## 6. Sharing the model with the chat

`RuntimeWebAIBackend` is process-wide and talks to the same
`BrowserAIRuntime` as the AI chat:

- **One page generation at a time** (`GenerationScheduler`); the chat
  calls the runtime directly, so it waits behind at most one page request.
- **Fairness.** Requests from visible pages go before hidden ones; within
  each, sites take turns (round-robin by when each site last ran).
- **Limits.** 16 live sessions per site (top-level site, per profile), 16
  queued generations per site, 64 in total, 32 per session. Over a limit
  `create()` fails or the request gets a generic failure (never
  `kErrorRequestThrottled`, which crash-reports in the renderer).
- **Deadline.** A page generation, model load included, is cancelled after
  five minutes. Hidden reasoning is off and capped at 256 tokens.
- **Model.** Pages use the loaded model; with none, they load the pick from
  §0.2. The runtime's idle and memory-pressure unloads still apply.
- **Cancellation** goes to the runtime by request id; the inference service
  stops decoding. Streaming text is batched every 200 ms.
- **Prompt size.** The runtime keeps the system prompt and drops history,
  then context, then the end of the user turn, if a prompt doesn't fit the
  loaded model's window. The `LanguageModel` bucket fits by construction;
  a writing call near 9216 tokens on a small-context model is cut (§13).

## 7. Untrusted input

- Every size is checked before text is read or copied: a request whose byte
  count alone exceeds three bytes per quota token fails at once.
- Token estimates are computed by a linear scan; no `std::regex`, no
  parsing of page text beyond UTF-8 lead bytes.
- Text is made valid UTF-8 with no NUL bytes (`ToDisplayText`).
- Language tags are checked by a linear BCP 47 shape check (35 bytes max)
  before they are named in a prompt.
- Everything from the page (system prompt, turns, input, context) is put in
  the `ChatPrompt` fields the inference service treats as untrusted and
  neutralizes (chat-template tokens can't be injected). MoltBrowser's own
  instructions are the system prompt and the preambles. The output goes
  back to the same page only; no tool, agent or other site sees it.
- Renderer input never reaches a `CHECK`. A permissions-policy or
  feature-flag violation that Blink itself prevents is a bad message (as
  upstream); everything else is an ordinary error.

## 8. Privacy and fingerprinting

What a page can learn, per call:

- **Availability**: unavailable / downloadable / available. "Available"
  only for sites the user allowed (and hasn't removed), so to any other
  site the answer is "downloadable" on every capable machine.
  "Unavailable" for capable machines appears only with a policy, a switch,
  MoltNet, a block the user chose or a paused prompt, and these look the
  same (§4.3). Device capability is about one bit (no catalog model fits
  the RAM, or no inference support).
- **Quotas**: 1024 or 4096 for `LanguageModel` (about one bit, the model's
  context class, and only to allowed sites); 9216 for the writing APIs.
- **Token counts**: from the model-independent estimator (a third of a
  token per ASCII byte, four thirds per other character, plus four per
  message), so `measureInputUsage()` and `inputUsage` say nothing about the
  tokenizer.
- **Parameters**: the default and maximum top-K and temperature
  (`GetLanguageModelParams`) are the same for every user.
- **Not hidden** (documented gaps): token timing (the stream reveals the
  model's speed, so roughly the hardware class and model size), the model's
  own behaviour (its answers identify the model family to a determined
  site), and the first call's latency (whether a model was already loaded,
  that is, whether the user just used the chat). These are visible only to
  sites the user allowed.

**MoltNet:** off. Every API reports `"unavailable"` in MoltNet windows, for
every MoltNet user alike; no device facts are read, nothing is loaded and
no prompt is shown. (On-device AI is allowed in MoltNet for the chat, which
the user drives; a page-driven API would expose timing and model behaviour
and would add a prompt a site can trigger.)

**Shields:** not consulted. The APIs are a feature the user grants per
site, not a tracking surface Shields should hide; with Shields off a site
learns nothing more. Fingerprinting is limited by the bucketing above.

**Incognito:** works, with its own answers and pauses (none carried over
from the regular profile, none written to disk), forgotten when Incognito
closes.

## 9. Exposure at 152

`LanguageModel` and `Summarizer` are stable in 152 and need nothing.
`Writer` and `Rewriter` ship behind an origin trial;
`molt_feature_overrides.cc` enables `kAIWriterAPI` and `kAIRewriterAPI`,
which turns the Blink features on for every page
(`copied_from_base_feature_if: "overridden"`). So all four interfaces exist
on every http(s) page in every window type, MoltNet included, and their
presence carries no information; `availability()` says whether a site may
use them. `chrome://flags` and `--disable-features` can still turn any of
them off (the interface then disappears, as in Chromium). Proofreader and
embeddings keep their upstream exposure and always report
`"unavailable"`.

## 10. Translator and LanguageDetector

These need translation and language-identification models, which
MoltBrowser doesn't ship yet; they are not served by `MoltAIManager`.

- **Translator**, as upstream ships it, is enabled and backed by
  `TranslationManagerImpl`: after a click, a page's `Translator.create()`
  makes the component updater download Google's TranslateKit library and
  language packs (browser-wide, so outside Tor even from a MoltNet window).
  MoltBrowser therefore defaults the `TranslatorAPIAllowed` pref
  (`on_device_translation.translator_api_allowed`) to off in
  `molt_privacy_prefs.cc`: `Translator.availability()` reports
  `"unavailable"` in every window and `create()` fails with "disallowed by
  policy" before any download. The interface stays present. An
  administrator can turn it on with the `TranslatorAPIAllowed` policy.
- **LanguageDetector** reports `"unavailable"` in every window, MoltNet
  included, for every user, and `create()` fails ("Model not available").
  Upstream's detector runs a TFLite model in the renderer (Blink's
  `LanguageDetector`, `modules/ai/on_device_translation/`), whose file
  Blink gets from the browser over
  `language_detection.mojom.ContentLanguageDetectionDriver`, which
  `ChromeContentBrowserClient::BindLanguageDetectionDriver` serves from the
  optimization guide's model delivery. That needs a Google API key, which
  MoltBrowser doesn't ship: in a keyless build `availability()` said
  `"downloadable"` and `create()` waited for a model that never arrived;
  with the user's own key it said `"available"` (about one bit, and a model
  fetched from Google). The hook is in Blink:
  `AIInterfaceProxy::GetLanguageDetectionModelStatus()` and
  `GetLanguageDetectionModel()` (`modules/ai/ai_interface_proxy.cc`)
  answer "not available" in the renderer, asynchronously, without asking
  the browser. The browser's binding is left alone because Translate's
  page-language detection (`LanguageDetectionAgent`) uses the same driver
  through the same frame broker, and the browser can't tell its requests
  from a page's.

  **CLD3 instead: investigated, not done.** CLD3 (`third_party/cld_3`) is
  already in the renderer for Translate and needs no download, but serving
  `LanguageDetector` with it is not a contained change:
  - detection runs on Blink's `LanguageDetectionModel`
    (`platform/language_detection/`), a wrapper around the TFLite model
    that loads a file; CLD3 would replace its backend, which means Blink
    platform code, its `DEPS` and its `BUILD.gn` (a dependency on
    `//third_party/cld_3/src/src:cld_3`, whose visibility to Blink isn't
    established), besides `AIInterfaceProxy`;
  - `LanguageDetector::ConvertResult()` `CHECK`s the predictions' shape
    (an `"unknown"` entry, scores that never exceed what is left, `"und"`
    alone only for empty input). CLD3 reports the top languages with
    per-span probabilities and byte proportions, which would have to be
    turned into a distribution meeting those `CHECK`s exactly, in floats;
    a mistake is a renderer crash any page can trigger;
  - none of it can be compiled or run without a Chromium checkout, and
    CLD3's sources (a DEPS checkout that needs protobuf) weren't reachable
    to test the conversion.

  It is a good follow-up: CLD3 is deterministic and the same binary for
  everyone, so every user, MoltNet included, would get the same answers.

## 11. Switches

| Switch | Effect |
|---|---|
| "Let sites ask to use on-device AI" off in `molt://ai-settings`, or `"web_ai_apis": false` in `~/.moltbrowser/settings.json` | every site gets `"unavailable"` (from the page: at once; a hand edit: within 30 s) |
| *Remove* / *Remove all* in `molt://ai-settings` | the site is asked again; an allowed site's sessions end |
| Clear browsing data: "Site settings" / history or site usage data; profile reset | answers in the time range / the prompt's pauses / both |
| Chromium's On-device AI switch (`chrome://settings`) | same |
| Policies `BuiltInAIAPIsEnabled` = false, `GenAILocalFoundationalModelSettings` = 1 | same |
| `chrome://flags` `#prompt-api` (with `#prompt-api-multimodal-input`, which also enables it), `#summarizer-api`, `#writer-api`, `#rewriter-api` set to Disabled | the interface disappears |
| Permissions policy (`allow="language-model"` etc.) | as upstream: cross-origin iframes need it |
| Policy `TranslatorAPIAllowed` = true | turns the upstream Translator back on |

## 12. Tests and verification

In `molt_ai_unittests` (`//chrome/browser/molt_ai/web_ai:unit_tests`):

- `web_ai_policy_unittest.cc` (20): the availability table, including an
  exhaustive check of invariants over every input combination (an
  unallowed site never sees "available", MoltNet and workers always see
  "unavailable", ...), create actions, buckets, the estimator and its
  size shortcut, saturation, sampling clamps, language tags, model pick,
  and the scheduler (one at a time, foreground first, sites in turn, caps,
  cancel).
- `web_ai_site_permissions_unittest.cc` (19): key shapes, answers, the
  pause (third close, its end to the millisecond, the count kept, an
  answer clearing it, clock changes either way), the settings list, exact
  removal, the Clear browsing data range (half-open) and filters, the
  bounds and what goes first, and the stored form's rules (versions, bad
  values, round trip), plus random operations against the invariants.
- `web_ai_site_permissions_service_unittest.cc` (10): the service over a
  `TestingPrefServiceSimple`: answers and pauses survive a restart, the
  stored format, a damaged pref ignored and cleaned on the next write,
  versions, Incognito never writing, non-http(s) and opaque origins,
  observers, removal, Clear browsing data with a URL filter, no writes
  after shutdown.
- `web_ai_prompts_unittest.cc` (10): every option → prompt mapping and
  reply budget.
- `web_ai_conversation_unittest.cc` (10): quota accounting, whole-exchange
  eviction, rollback.
- `molt_ai_manager_unittest.cc` (41): the mojo surface over real pipes with
  a fake runtime and a fake environment: availability per state, MoltNet
  never calling the runtime, policies, permissions policy as a bad message,
  consent (allow, block, dismiss, needs-model), an answer taken back
  (sessions end; a `create()` whose model is loading fails), aborted
  `create()`, quotas, overflow, append, abort and destroy cancelling the
  generation, the document going away, clone, measure, unsupported
  prompts, throttling and failures, queue limits, and all three writing
  APIs.
- `molt_ai_chat_handler_helpers_unittest.cc` (in
  `//chrome/browser/ui/webui/molt_ai:unit_tests`): `BoolArg()` and
  `BoundedStringArg()`, which the settings section's handler uses, with
  hostile values.
- `molt_feature_overrides_unittest.cc`: Writer and Rewriter are enabled by
  default and can be turned off.

Run here (no Chromium checkout): the 42 pure tests with clang
(ASan/UBSan) and g++ `-O2 -Werror`, and compiled with `-fno-exceptions
-fno-rtti`; the 39 manager tests against small functional stand-ins of
`base` and mojo (ASan/UBSan/LSan and g++), with mojom stand-ins written
from the 152 `.mojom` files; the binder, consent, runtime backend and the
`BindAIManager` hook syntax-checked against stand-ins with the 152
signatures; all of it clean under Chromium's clang warning set. 31
mutations of the logic were all caught.

For the remembered answers, the pause, the settings page and
LanguageDetector (same harness): the 19 table tests with the pure tests
(clang ASan/UBSan, g++ `-O2 -Werror`, `-fno-exceptions -fno-rtti`); the
10 service tests and the argument-helper tests against functional
stand-ins of `base::Value`, `PrefService`, `GURL` and `url::Origin`; the
41 manager tests; the factory, the binder, the infobar, the settings
switch, the WebUI handler, the edited *Save* / *Reset* handlers and the
Clear browsing data / profile reset / factory hooks (taken verbatim)
syntax-checked against stand-ins with the 152 signatures under
Chromium's warning set; the two `AIInterfaceProxy` bodies compiled from
the file and run against stand-ins (they answer asynchronously and never
reach the browser); 25 mutations of the table, the pause and the
revocation all caught. The WebUI checks pass, and the smoke test feeds
the section hostile keys and names, rows of the wrong shape and junk
answers (4 mutations of its script caught).

**Needs the build machine:** the real compile and link (generated mojo
bindings, GN deps and visibility), `molt_ai_unittests`, and in a browser:
each API from a page in a regular, Incognito and MoltNet window; the
infobar (allow, block, choose a model, navigation); abort during a
generation; the chat and a page generating at the same time; `Writer` and
`Rewriter` present without an origin-trial token; `Translator` reporting
`"unavailable"` with no component-update request in a traffic capture.
For §4 and §10: allow, block and a paused site after a restart; three
closes then a week (or a clock change); *Remove* ending a live session;
Clear browsing data ("Site settings", "last hour"; history) and a profile
reset; the settings page in a regular, Incognito, Guest and MoltNet
window, and its switch; `Save Settings` keeping `"web_ai_apis"`;
`LanguageDetector.availability()` `"unavailable"` and `create()` failing
in every window, with Translate still detecting page languages.

## 13. Gaps and follow-ups

- Answers are keyed by top-level origin; the scheduler's limits by
  top-level site.
- The answers live in a MoltBrowser pref, not a content setting, so
  `chrome://settings/content` and the page info bubble don't show them
  (§4.4); `molt://ai-settings` does.
- Incognito's answers aren't listed on the settings page (they go when the
  window closes).
- The switch is browser-wide (`settings.json`), the answers per profile.
- Removing an allowed site ends its sessions; an answer changed while a
  generation is running cancels it, but the page still holds the text it
  already received.
- No constrained decoding: JSON-schema `responseConstraint` is best effort;
  regex constraints, `prefix`, images, audio and tools are unsupported.
- Workers and extensions (pages and service workers) get
  `"unavailable"`.
- A writing-API call near the 9216-token limit on a model with a small
  context window is cut by the runtime (context first, then the end of the
  input) rather than refused.
- Timing and model behaviour are visible to allowed sites (§8).
- Translator needs a MoltBrowser translation model ("On-device
  translation" on the roadmap). LanguageDetector could be served by CLD3
  in the renderer, uniformly for everyone (§10).
