# Web-standard AI APIs on the on-device model

Status: implemented (October 2026), pending a compile on the build machine
and the in-browser checks in §12. Roadmap item "Web-standard AI APIs,
without Google". The Prompt API (`LanguageModel`), `Summarizer`, `Writer`
and `Rewriter` run on the user's own on-device model through
`BrowserAIRuntime`, never on upstream's optimization-guide model. Translator
and LanguageDetector are not served (§10).

Pin: Chromium 152.0.7977.169 (`92a6f151`). Upstream files below were read
at that revision with `upshow new`.

## 0. Decisions in one page

1. **One seam: the browser's `AIManager`.** Blink is unchanged. Upstream's
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
   per profile and per top-level origin, in memory, for the browser
   session. No model is loaded for a site before it is allowed.
4. **Few, uniform states.** `availability()` is `"unavailable"`,
   `"downloadable"` or `"available"`; never `"downloading"`. `inputQuota` is
   1024 or 4096 tokens for `LanguageModel` (by the loaded model's context)
   and 9216 for the writing APIs (fixed in Blink). Token counts come from a
   model-independent estimator, and sampling parameters are the same for
   everyone. No model name, size or exact context window reaches a page.
5. **Off where it can't be private.** `"unavailable"` in MoltNet windows,
   in workers, in documents whose top-level page isn't http(s), under the
   enterprise policies upstream honours, with Chromium's on-device AI
   switch off, and with `"web_ai_apis": false` in `settings.json`.
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
   outside Tor in MoltNet). LanguageDetector is left as upstream ships it.
10. **Upstream files touched:** `chrome/browser/chrome_content_browser_client.cc`
    (one hook) and `components/infobars/core/infobar_delegate.h` (one enum
    value). Both were already MoltBrowser-patched.
11. **`responseConstraint` is enforced.** A JSON schema is turned into a
    grammar inside the sandboxed inference service, and the model can only
    write JSON that matches it. The page gets the answer whole, once it
    parses. A schema outside the supported subset, or a regular expression,
    fails with `NotSupportedError` (§5).

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
| `LanguageDetector` | `LanguageDetectionAPI`, stable | `ContentLanguageDetectionDriver` + optimization guide model | unchanged (§10) |

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
- `responseConstraint` (`language_model.cc`, `ParseConstraint`):
  - An object is `JSON.stringify`'d and sent with `Prompt()` as
    `ResponseConstraint::NewJsonSchema`. A `RegExp` is sent as its source
    only, without its flags.
  - Unless the page sets `omitResponseConstraintInput`, Blink also appends
    the schema to the prompt text (`GetSchemaForInput`).
  - Upstream answers a constraint its model can't apply with
    `kErrorInvalidRequest` (`ai_language_model.cc`,
    `GenerateError::kInvalidConstraint`).
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
| `web_ai_policy.{h,cc}` | Pure: the availability state machine, create actions, quota buckets, token estimator, sampling clamps, language tags, model pick, `GenerationScheduler`, `ConsentMap`. |
| `web_ai_prompts.{h,cc}` | Pure: Summarizer/Writer/Rewriter options → system prompt, context and reply budget. |
| `web_ai_conversation.{h,cc}` | Pure: a `LanguageModel` session's context, quota accounting and eviction. |
| `web_ai_backend.{h,cc}` | The interface between the manager and the runtime (`GenerationJob`, device facts, cancel). |
| `molt_ai_manager.{h,cc}` | `MoltAIManager`: `blink.mojom.AIManager`, the create flow. |
| `web_ai_sessions.{h,cc}` | `LanguageModelSession`, `SummarizerSession`, `WriterSession`, `RewriterSession`: queueing, streaming, abort. |
| `molt_ai_manager_binder.{h,cc}` | `BindMoltAIManager()`: the facts about a document (MoltNet, policies, permissions policy, top-level origin, visibility). |
| `web_ai_consent.{h,cc}` | The per-profile answers and the consent infobar. |
| `runtime_web_ai_backend.{h,cc}` | `RuntimeWebAIBackend` over `BrowserAIRuntime`: device facts, model load, the scheduler, deadlines. |

`BrowserAIRuntime` gained `GetLoadedContextTokens()` (the loaded model's
context size, for the quota bucket).

## 4. Availability, consent and the model

`DecideAvailability()` (`web_ai_policy.cc`), first match wins:

| Condition | `availability()` | `create()` |
|---|---|---|
| Feature off (`chrome://flags`, `--disable-features`) | the interface is absent (the browser would answer `"unavailable"`) | - |
| Enterprise policy disallows | `"unavailable"` | fails |
| MoltNet, a worker, top-level page not http(s), the On-device AI switch off, or `"web_ai_apis": false` | `"unavailable"` | fails |
| Options not supported (§5) or an invalid language tag | `"unavailable"` | fails |
| The user blocked this site this session | `"unavailable"` | fails |
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
nothing is loaded); then the session. If the page aborts `create()` or goes
away at any step, the request is dropped: no infobar answer is needed and
no model is loaded.

**The infobar** (`web_ai_consent.cc`) names the top-level origin, so an
iframe asks on behalf of the page the user sees. Only the tab's current
page can raise it (not a prerendered page or one in the back/forward
cache). Requests from the same tab and origin share one infobar. *Allow*
and *Block* last until MoltBrowser restarts (Incognito's until it closes);
closing the infobar answers nothing, and the page may ask again after
another click. With no model downloaded, *Choose a model* records the
allow and opens `molt://ai`, where the user picks and downloads a model;
the pending `create()` fails, and the page can try again once the model is
there.

Why an infobar and not upstream's model-download UX: upstream has none (a
gesture alone lets any page start a multi-gigabyte download), and a
Chromium permission type would mean a new `ContentSettingsType`,
permission-context plumbing and settings UI across many upstream files.
The infobar is MoltBrowser-only code, asks at the moment of use, and the
gesture rule keeps a page from raising it unprompted. Persistent grants and
a revoke UI are a follow-up (§13).

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
  `responseConstraint`.

**Structured output: `responseConstraint`** (LanguageModel `prompt()` /
`promptStreaming()`).

- **A JSON schema is enforced.**
  - The session passes it with the generation job. The browser checks only
    its size (at most 32 KiB) and never parses it.
  - The sandboxed inference service turns it into a grammar. llama.cpp's
    grammar sampler then allows only tokens that keep the answer inside it
    (SANDBOXED_INFERENCE.md §9).
  - Reasoning is skipped for the request.
  - Blink's copy of the schema in the prompt text stays as Blink sends it
    (`omitResponseConstraintInput` is honoured by Blink, unchanged).
- **The reply is held back.** It goes to the page as one chunk at the end,
  and only once it parses as JSON (`base::JSONReader`, RFC 8259; the
  runtime checks too). So `promptStreaming()` with a constraint delivers
  the whole answer at once. A failed request leaves the context unchanged.
- **The supported subset** (details in
  `chrome/services/molt_ai/inference/engine/json_schema_grammar.h`):
  - `{}` / `true` (any value, nested at most 3 deep);
  - `type`, one or a list: object, array, string, number, integer, boolean,
    null;
  - `properties` (generated in the order given), `required`,
    `additionalProperties`: `false`/`true`/absent (no extra keys are
    generated), or a schema when there are no `properties` (a map);
  - `items` (one schema), `minItems`/`maxItems` up to 64;
  - `minLength`/`maxLength` up to 1024;
  - integer `minimum`/`maximum`/`exclusiveMinimum`/`exclusiveMaximum`
    giving a range of at most 1001 values, or a minimum of 0 or 1 alone;
  - number `minimum: 0` alone, or whole-number bounds 0 ≤ min ≤ max with a
    span of at most 1000;
  - `enum` (up to 256 values), `const`;
  - `anyOf`/`oneOf` of these, on their own (oneOf is treated as anyOf);
  - annotations (`title`, `description`, `default`, `examples`,
    `$schema`, ...) are ignored.
- **Refused:** everything else, e.g. `$ref`, `pattern`, `format`, `allOf`,
  `not`, `if`, `prefixItems`, `uniqueItems: true`, `multipleOf`, unknown
  keywords. Also refused: schemas over the size, nesting (12) or ambiguity
  caps, and schemas no value can match.
- **What the page sees** (Blink's `ConvertModelStreamingResponseErrorToDOMException`):

  | Case | Status | `prompt()` rejects with |
  |---|---|---|
  | Schema refused (as above), not JSON, empty or over 32 KiB; a regex | `kErrorInvalidRequest` | `NotSupportedError`: "The request is invalid - the input or options could not be processed." Blink also calls `DumpWithoutCrashing`, as for upstream's invalid constraint. |
  | Answer cut off by the reply budget (512 or 1024 tokens) before the JSON closed | `kErrorResponseExceedsMaxTokens` | `QuotaExceededError`: "The response exceeded output limits and was truncated." |
  | Answer isn't complete JSON, or the model couldn't continue the format | `kErrorResponseParsingFailed` | `UnknownError`: "Failed to parse the response." |

  `promptStreaming()` errors its stream with the same exception.

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
- A `responseConstraint` schema is not parsed in the browser; only its
  size is checked. The sandboxed inference service parses and converts it,
  with bounded, linear code (SANDBOXED_INFERENCE.md §9.2-9.3). The
  constrained reply is parsed with `base::JSONReader`, which is
  memory-safe.
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
  only for sites the user allowed this session, so to any other site the
  answer is "downloadable" on every capable machine. "Unavailable" for
  capable machines appears only with a policy, a switch, MoltNet or a block
  the user chose. Device capability is about one bit (no catalog model fits
  the RAM, or no inference support).
- **Quotas**: 1024 or 4096 for `LanguageModel` (about one bit, the model's
  context class, and only to allowed sites); 9216 for the writing APIs.
- **Token counts**: from the model-independent estimator (a third of a
  token per ASCII byte, four thirds per other character, plus four per
  message), so `measureInputUsage()` and `inputUsage` say nothing about the
  tokenizer.
- **Parameters**: the default and maximum top-K and temperature
  (`GetLanguageModelParams`) are the same for every user.
- **Structured output**: which schemas are enforced or refused is the
  same for every user and model. A refused schema fails before any token
  is generated. (A model whose vocabulary can't continue a grammar fails
  differently, but no catalog model is expected to.)
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

**Incognito:** works, with its own answers (none carried over from the
regular profile), forgotten when Incognito closes.

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
- **LanguageDetector** is unchanged. Its model comes from the optimization
  guide's model delivery, which needs a Google API key that MoltBrowser
  doesn't ship (docs/NETWORK_TRAFFIC.md), and a page's call doesn't start a
  download. So in a keyless build it reports `"downloadable"`, and after a
  click `create()` waits for a model that never arrives (later calls fail
  once the provider's small pending queue is full). With the user's own
  API key the model arrives at startup and the API works, in every window;
  that it is available is then about one bit. Not verified on a build.
  Making it `"unavailable"` needs a hook in upstream code; it is a
  follow-up.

## 11. Switches

| Switch | Effect |
|---|---|
| `"web_ai_apis": false` in `~/.moltbrowser/settings.json` | every site gets `"unavailable"` (read at most every 30 s) |
| Chromium's On-device AI switch (`chrome://settings`) | same |
| Policies `BuiltInAIAPIsEnabled` = false, `GenAILocalFoundationalModelSettings` = 1 | same |
| `chrome://flags` `#prompt-api` (with `#prompt-api-multimodal-input`, which also enables it), `#summarizer-api`, `#writer-api`, `#rewriter-api` set to Disabled | the interface disappears |
| Permissions policy (`allow="language-model"` etc.) | as upstream: cross-origin iframes need it |
| Policy `TranslatorAPIAllowed` = true | turns the upstream Translator back on |

## 12. Tests and verification

In `molt_ai_unittests` (`//chrome/browser/molt_ai/web_ai:unit_tests`):

- `web_ai_policy_unittest.cc` (22): the availability table, including an
  exhaustive check of invariants over every input combination (an
  unallowed site never sees "available", MoltNet and workers always see
  "unavailable", ...), create actions, buckets, the estimator and its
  size shortcut, saturation, sampling clamps, language tags, model pick,
  the scheduler (one at a time, foreground first, sites in turn, caps,
  cancel) and the consent map.
- `web_ai_prompts_unittest.cc` (10): every option → prompt mapping and
  reply budget.
- `web_ai_conversation_unittest.cc` (10): quota accounting, whole-exchange
  eviction, rollback.
- `molt_ai_manager_unittest.cc` (43): the mojo surface over real pipes with
  a fake runtime and a fake environment: availability per state, MoltNet
  never calling the runtime, policies, permissions policy as a bad message,
  consent (allow, block, dismiss, needs-model), aborted `create()`, quotas,
  overflow, append, abort and destroy cancelling the generation, the
  document going away, clone, measure, unsupported prompts, throttling and
  failures, queue limits, and all three writing APIs. Constrained prompts:
  - the schema reaches the job;
  - the reply is held back, then delivered whole once it is JSON;
  - non-JSON replies fail without touching the context;
  - each failure maps to its Blink status;
  - an oversized or empty schema is refused at once.
- `molt_feature_overrides_unittest.cc`: Writer and Rewriter are enabled by
  default and can be turned off.

Run here (no Chromium checkout): the 42 pure tests with clang
(ASan/UBSan) and g++ `-O2 -Werror`, and compiled with `-fno-exceptions
-fno-rtti`; the 43 manager tests against small functional stand-ins of
`base` and mojo (ASan/UBSan/LSan and g++), with mojom stand-ins written
from the 152 `.mojom` files (and a `base::JSONReader` stand-in over the
engine's strict reader); the binder, consent, runtime backend and the
`BindAIManager` hook syntax-checked against stand-ins with the 152
signatures; all of it clean under Chromium's clang warning set. 31
mutations of the logic were all caught. Constrained decoding has its own
tests (SANDBOXED_INFERENCE.md §9.5).

**Needs the build machine:** the real compile and link (generated mojo
bindings, GN deps and visibility), `molt_ai_unittests`, and in a browser:
each API from a page in a regular, Incognito and MoltNet window; the
infobar (allow, block, choose a model, navigation); abort during a
generation; the chat and a page generating at the same time; `Writer` and
`Rewriter` present without an origin-trial token; `Translator` reporting
`"unavailable"` with no component-update request in a traffic capture;
`prompt()` with a `responseConstraint` on each catalog model (the answer
parses and matches; an unsupported schema rejects with
`NotSupportedError`).

## 13. Gaps and follow-ups

- Answers last for the browser session only, and there is no list to
  review or revoke them (a site is unblocked by restarting). A Chromium
  permission type with a settings page is the follow-up.
- Answers are keyed by top-level origin; the scheduler's limits by
  top-level site.
- Dismissing the infobar isn't remembered, so a page can ask again after
  each click (there is no embargo).
- Regex `responseConstraint`, `prefix`, images, audio and tools are
  unsupported. A JSON-schema `responseConstraint` is enforced only within
  the subset in §5 (others fail), reasoning is skipped for it, and the
  constrained reply isn't streamed (SANDBOXED_INFERENCE.md §9.4).
- Workers and extensions (pages and service workers) get
  `"unavailable"`.
- A writing-API call near the 9216-token limit on a model with a small
  context window is cut by the runtime (context first, then the end of the
  input) rather than refused.
- Timing and model behaviour are visible to allowed sites (§8).
- Translator needs a MoltBrowser translation model ("On-device
  translation" on the roadmap); LanguageDetector should report
  `"unavailable"` until then (§10).
