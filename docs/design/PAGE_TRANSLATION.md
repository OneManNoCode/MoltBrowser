# Translating a page on the device

Status: implemented (October 2026), pending a compile on the build machine
and the in-browser checks in §9. The research (§1) was written before the
design. Roadmap item "Web-standard AI APIs, without Google", second half:
the browser's own Translate button.

Pin: Chromium 152.0.7977.169 (`92a6f151`). Upstream files were read at
that revision; line numbers below are 152's.

## 0. Decisions in one page

1. **Page text never leaves the device.** "Translate this page" runs on
   the user's own on-device model, through the same backend, instruction,
   splitter, language table and output checks as the web `Translator` API
   (`docs/design/WEB_AI_APIS.md` §10). Google's translate script
   (`translate_a/element.js`) is never requested, in any window.
2. **One seam: the Translate driver.** A subclass of upstream's
   `ContentTranslateDriver` replaces what happens on *Translate* and *Show
   original*; Translate's UI, prefs, language detection and metrics stay
   upstream's. Six small hooks in seven upstream files (§3).
3. **Isolated world only.** The page's text is read and written from the
   browser by a script in Translate's isolated world
   (`ISOLATED_WORLD_ID_TRANSLATE`), never in the page's main world, and
   written back as text only (§4.1, §4.4).
4. **Only on the user's click, or "always translate".** A link from a
   translated page, `hrefTranslate` and a predefined target no longer
   translate by themselves; they offer the bubble (§4.2).
5. **No model, no download.** Without a downloaded model the bubble says so
   and links to `molt://ai`; Translate doesn't pop up by itself until a
   model exists (§4.6).
6. **The language list is local.** Translate offers the Translator's 20
   languages; `translate_a/l` is never requested (§4.5).
7. **On in MoltNet, by click only** (no "always translate"), with a note in
   the bubble that the site sees the translation (§5).
8. **No Google fallback.** MoltBrowser ships no Google API key, so
   upstream's path already refused to run in a stock build; it is gone, not
   hidden behind a setting (§6).

## 1. What Chromium 152 does when you click "Translate this page"

### 1.1 The pieces

| Piece | Where (152) | What it does |
|---|---|---|
| `TranslateManager` | `components/translate/core/browser/translate_manager.cc` | One per tab. Decides whether to offer, auto-translate or do nothing on each page (`InitiateTranslation()` → `ComputePossibleOutcomes()` → `MaterializeDecision()`), starts a translation (`TranslatePage()`), reverts it (`RevertTranslation()`) and turns the result into UI (`PageTranslated()`). Holds the tab's `LanguageState`. |
| `TranslateClient` | `.../translate_client.h`; Chrome's is `ChromeTranslateClient` (`chrome/browser/translate/chrome_translate_client.cc`) | The embedder: prefs, `ShowTranslateUI()` (the bubble on desktop, a message on Android), which URLs are translatable. It creates the driver and the manager (`chrome_translate_client.cc:186-209`). |
| `TranslateDriver` | `.../translate_driver.h` | The manager's view of the page: `TranslatePage(seq, script, source, target)`, `RevertTranslation(seq)`, `IsIncognito()`, MIME type, URLs. |
| `ContentTranslateDriver` | `components/translate/content/browser/content_translate_driver.cc` | The content-layer driver. The renderer's `TranslateAgent` registers each page with it (`RegisterPage()`, with the detected language) and it calls the agent back over `translate.mojom.TranslateAgent`: `TranslateFrame(script, source, target) => (cancelled, source, target, error)` and `RevertTranslation()`. `IsIncognito()` is `BrowserContext::IsOffTheRecord()`, so MoltNet's profile counts as Incognito. |
| `TranslateAgent` | `components/translate/content/renderer/translate_agent.cc` | One per main frame. Detects the page language (CLD3, in the renderer, no network) and registers the page. On `TranslateFrame` it runs the script the browser sent in **isolated world `ISOLATED_WORLD_ID_TRANSLATE`** (2), whose security origin it sets to `https://translate.googleapis.com` with an empty CSP (`isolated_world_util.cc`), polls `cr.googleTranslate.libReady`, calls `cr.googleTranslate.translate(source, target)`, polls `finished` / `error` / `errorCode` every 400 ms, and reverts with `cr.googleTranslate.revert()` (`translate_agent.cc:479-551`). |
| `TranslateScript` | `.../translate_script.cc` | The script `TranslateFrame` gets: `https://translate.googleapis.com/translate_a/element.js?cb=cr.googleTranslate.onTranslateElementLoad&...&hl=<UI language>` (`translate_script.cc:56-57,149-197`), fetched by the browser through `TranslateDownloadManager`'s loader, which `TranslateService::Initialize()` sets to the **system network context** (`translate_service.cc:67-73`), cached for a day, then prefixed with `translateApiKey` (the Google API key), `securityOrigin` and the bundled `translate.js` (`IDR_TRANSLATE_JS`). element.js loads more JS and CSS from Google inside the isolated world, and **sends the page's text to Google** from there (through the page's own network context). |
| `TranslateLanguageList` | `.../translate_language_list.cc` | The languages Translate offers: a built-in list of 133 (`kDefaultSupportedLanguages`, :46-182), replaced by `https://translate.googleapis.com/translate_a/l?client=chrome&hl=<UI>&key=<key>` once fetched (`RequestLanguageList()`, :280-310), whenever `GetSupportedLanguages(translate_allowed=true)` is asked and requests are allowed (`ResourceRequestAllowedNotifier`, `translate_service.cc:105-115`). Asked by the bubble (`TranslateUIDelegate`'s constructor), the language settings page and `translate-internals`. `IsSupportedLanguage()` gates everything else: offering, auto-translate, target choice. |
| UI | desktop: `TranslateBubbleView` (`chrome/browser/ui/views/translate/`), the omnibox icon (`TranslatePageActionController`), the app menu and context menu items; Android: `TranslateMessage` | The bubble has tabs (source / target), Options (always / never translate, change languages), and views per `TranslateStep`: before, translating, after, error. The error view always says "This page could not be translated" with *Try again* (`translate_bubble_view.cc:739-806`), whatever the error. The footnote is Google Translate's wordmark in Chrome-branded builds and **nothing** otherwise (`CreateWordmarkView()`, :99-114). |

### 1.2 The flow

1. The renderer detects the language and calls `RegisterPage()`;
   `ContentTranslateDriver` numbers the page (`page_seq_no`) and calls
   `TranslateManager::InitiateTranslation()`.
2. `ComputePossibleOutcomes()` runs filters (:782-823):
   `FilterIsTranslatePossible()` (not already translated or declined, **not
   offline**, **a Google API key configured**, a supported MIME type and
   URL, "Offer to translate" on, page language ≠ target, page language in
   the list), the ranker, the user's never-translate languages and sites,
   then which automatic translation applies: the command-line language,
   **"always translate" for this language** (`FilterAutoTranslate()`, skipped
   when `IsIncognito()`), **a link from a translated page**
   (`LanguageState::AutoTranslateTo()`, also in Incognito),
   **`hrefTranslate`** on a link from a Google page, and a **predefined
   target** an embedder set.
3. `MaterializeDecision()` auto-translates, or shows the bubble
   (`TranslateClient::ShowTranslateUI(BEFORE_TRANSLATE)`), or only enables
   the omnibox icon.
4. A click (bubble, icon, app menu "Translate…", context menu "Translate
   to X", whose availability is `CanManuallyTranslate()`: also **offline**
   and **API key** checks, :163-258) calls `TranslatePage()` (:333-421):
   `PrepareToTranslatePage()` on the driver, add the target to
   Accept-Language, unsupported source → `"und"`, same source and target →
   `"und"` (the server detects), show `TRANSLATING`, then **fetch the
   script** if it isn't cached and call `driver->TranslatePage(seq,
   script, source, target)`.
5. The agent reports back; `PageTranslated()` (:486-520) records the
   languages, clears `translation_pending`, and shows `AFTER_TRANSLATE` or
   the error. *Show original* is `RevertTranslation()`.
6. A cross-document navigation resets `LanguageState` (`DidNavigate()`);
   the agent's pipe for the old page closes, so its callback never comes.

### 1.3 Errors and states

`translate::TranslateErrors` (`translate_errors.h`): `NETWORK`,
`INITIALIZATION_ERROR`, `UNKNOWN_LANGUAGE`, `UNSUPPORTED_LANGUAGE`,
`IDENTICAL_LANGUAGES`, `TRANSLATION_ERROR`, `TRANSLATION_TIMEOUT`,
`UNEXPECTED_SCRIPT_ERROR`, `BAD_ORIGIN`, `SCRIPT_LOAD_ERROR`. On desktop
all of them show the same text. A translation whose agent answers
"cancelled" (`ContentTranslateDriver::OnPageTranslated(cancelled=true)`)
never reaches the manager, so `translation_pending` stays set until the
next navigation; only a real result or error clears it.

### 1.4 Prefs: "always translate" and friends

`TranslatePrefs` (`translate_prefs.h`), per profile: "Offer to translate"
(`translate.enabled`; the policy `TranslateEnabled` makes it managed and
then `CanManuallyTranslate()` refuses), never-translate languages and
sites, **always-translate pairs** (`translate_allowlists`: source →
target), recent target, accept/deny counts. On Android and iOS only, after
three accepted translations of a language the message UI turns "always
translate" on by itself (with an Undo) (`ShouldAutoAlwaysTranslate()`,
`translate_ui_delegate.cc:352-379`); the desktop bubble never does.

### 1.5 Incognito and other off-the-record profiles

Upstream keys everything on `TranslateDriver::IsIncognito()`
(`IsOffTheRecord()`): no automatic translation by preference (the bubble
hides "Always translate"), no error/init/detection callbacks to
`translate-internals`, no language histogram. Link continuation and
`hrefTranslate` still auto-translate in Incognito. The script fetch is made
"in the strictest mode for request headers" (no cookies), but from the
**system network context**: in a MoltNet window that request leaves
outside Tor (the known gap in `docs/MOLTNET_COMPLIANCE.md` §5.7 for users
who set `GOOGLE_API_KEY`), while element.js's own requests and the page
text go through the window's context, through Tor.

### 1.6 What a MoltBrowser build does today

MoltBrowser builds with `google_api_key = ""`, so
`google_apis::HasAPIKeyConfigured()` is false (`api_key_cache.cc:341-343`):
the key is the placeholder `dummytoken`. Then `FilterIsTranslatePossible()`
prevents every trigger (`kDisabledMissingAPIKey`), `CanManuallyTranslate()`
is false (the menu items are off), and `TranslateManager::IsAvailable()` is
false. **Translate is effectively off in a stock MoltBrowser build** unless
the user exports their own `GOOGLE_API_KEY`; then it sends page text to
Google. What still happens without a key: the language list request
(`translate_a/l?...&key=dummytoken`) whenever the bubble, the language
settings or `translate-internals` ask for the list (`docs/NETWORK_TRAFFIC.md`).

One more path: the context menu's **"Translate selection to X"** (partial
translate, `PartialTranslateManager`) sends the selected text to Google's
contextual-search endpoint. On the default menu it is shown only when
`CanManuallyTranslate()` and Google is the default search engine
(`render_view_context_menu.cc:1401-1418`), so today never; the simplified
menu (`kMenuSimplification`, off by default) asks only
`TranslateManager::CanPartiallyTranslateTargetLanguage()`.

### 1.7 What MoltBrowser already has

The web `Translator` API runs on the user's on-device model
(`chrome/browser/molt_ai/web_ai/`, `docs/design/WEB_AI_APIS.md` §10): a
fixed table of 20 languages (`kTranslatorLanguages`), a linear splitter for
long text (`SplitForTranslation()`), a fixed MoltBrowser instruction with
the page text as the untrusted user turn (`BuildTranslatorPrompt()`), a
reply constrained to `{"translation": string}` by the inference service's
grammar (`kTranslatorResponseSchema`), a cleanup (`CleanTranslation()`),
and a process-wide backend over `BrowserAIRuntime` that shares the model
with the chat, one generation at a time, visible tabs first, with per-site
and global caps and a deadline (`RuntimeWebAIBackend`,
`GenerationScheduler`).

## 2. The hook, and why this one

Candidates, smallest first:

1. **Replace the script.** Serve a local `cr.googleTranslate` instead of
   element.js, so the renderer's `TranslateAgent` runs it unchanged. But the
   isolated world has no channel to the browser's model: the script would
   need a new Mojo interface or renderer changes to get its translations,
   and the agent's 400 ms polling of `finished` would be the only progress.
   Rejected.
2. **Change the renderer's `TranslateAgent`.** A new `TranslateFrame`
   implementation would need the model from the renderer (a new browser
   interface), plus renderer and Mojo changes. Rejected: more upstream
   code, and the renderer would hold page text and translations together
   with the page.
3. **A driver of our own (chosen).** `TranslateManager` only ever talks to
   the page through `TranslateDriver::TranslatePage()` /
   `RevertTranslation()`, and reports through `TranslateManager::PageTranslated()`
   (which `ContentTranslateDriver::OnPageTranslated()` calls). A subclass of
   `ContentTranslateDriver` (`molt_ai::translate::MoltTranslateDriver`) that
   ChromeTranslateClient creates instead keeps registration, language
   detection, navigation handling, the UI, the prefs and the metrics
   exactly as upstream, and replaces only what happens on *Translate* and
   *Show original*: it reads and writes the page's text from the browser
   through `RenderFrameHost::ExecuteJavaScriptInIsolatedWorld()` (isolated
   world 2, never the page's main world) and translates with the web AI
   backend. Google's script is never requested.

   What the driver can't do alone, and the hooks it needs in upstream
   files (each marked `// MoltBrowser:`):
   - `TranslateManager::TranslatePage()` fetches element.js before it calls
     the driver; `CanManuallyTranslate()` and `FilterIsTranslatePossible()`
     refuse without an API key or a network. One virtual on
     `TranslateDriver`, `TranslatesOnDevice()` (false by default), lets the
     manager skip all three for our driver, and refuse partial translate
     and the automatic triggers other than "always translate" (§4.2).
   - The language list is a browser-wide singleton: a setter on
     `TranslateLanguageList`, called once at startup from
     `TranslateService::Initialize()`, replaces it with the translator's
     table, and the list no longer has a fetcher (§4.5).
   - The bubble's footnote (empty in non-Google builds) becomes
     MoltBrowser's status line: on-device, progress and *Stop*, or why it
     failed and *Choose a model* (§4.6).
   - `ChromeTranslateClient::ShowTranslateUI()` doesn't pop the bubble up by
     itself while no on-device model is downloaded (§4.6).

## 3. Code

`src/chrome/browser/molt_ai/translate/` (namespace
`molt_ai::page_translate`; `BUILD.gn` targets in brackets):

| File | What |
|---|---|
| `page_translate_plan.{h,cc}` [`logic`, std-only] | Caps; the language list in Translate's codes; `ResolvePagePair()`; `TranslationPlan` (slots → pieces → batches, visible first; accept, retry alone, split, settle; what to write); `CheckTranslation()`; how a job's end becomes a `TranslateErrors` value and a footnote problem |
| `page_translate_messages.{h,cc}` [`logic`] | The footnote's text and link for each state |
| `page_translate_script.{h,cc}` [`io`] | `kPageTranslateScript`, the isolated-world script (extract / apply / revert) |
| `page_translate_io.{h,cc}` [`io`, base::Value] | Builds the script calls (arguments as JSON); `ParseExtraction()`, `ParseApplyResult()`, `ReadBatchReply()` |
| `page_translation_job.{h,cc}` [`job`, no content/] | One translation: device facts → extract → load model → batches → writes; Stop, deadline, throttling, failures. The page is a `PageScriptRunner` |
| `molt_translate_driver.{h,cc}` [`translate`] | `MoltTranslateDriver` (the `ContentTranslateDriver` subclass) and the content/ `PageScriptRunner` (`ExecuteJavaScriptInIsolatedWorld()` on the document's `WeakDocumentPtr`) |
| `page_translate_status.{h,cc}` [`translate`] | Per-tab status for the footnote; `ShouldOfferTranslationAutomatically()` |
| `page_translate_footnote_view.{h,cc}` [`footnote_view`, desktop] | The bubble's footnote view |

Additive in `web_ai/web_ai_translation.{h,cc}`: the batch form of the
Translator's instruction (`BuildTranslatorBatchPrompt()`,
`TranslatorBatchResponseSchema()`, `TranslatorBatchTextTokens()`,
`kTranslatorMaxBatchTexts`). Nothing the Translator API uses changed.

Upstream hooks (each file imported unchanged from 152 first, then edited;
every hook is marked `// MoltBrowser:`):

| File | Hook | 152 code it sits in |
|---|---|---|
| `components/translate/core/browser/translate_driver.h` | `virtual bool TranslatesOnDevice() const { return false; }` | `class TranslateDriver`, after `HasCurrentPage()` |
| `components/translate/core/browser/translate_manager.cc` | `on_device` skips the offline and API-key checks | `CanManuallyTranslate()` (:163-185), `FilterIsTranslatePossible()` (:862-875) |
| same | returns false on the device | `CanPartiallyTranslateTargetLanguage()` (:260) |
| same | `DoTranslatePage("", source, target)` instead of the script fetch | `TranslatePage()`, before `TranslateDownloadManager::GetInstance()->script()` (:402) |
| same | `PreventAutoHrefTranslate()`, `PreventPredefinedLanguageAutoTranslate()` | end of `ComputePossibleOutcomes()` (:821) |
| same | no auto-translate by link | `FilterAutoTranslate()`'s `else if (!link_auto_translate_target.empty())` (:957) |
| `components/translate/core/browser/translate_language_list.{h,cc}` | `SetLocalLanguages()`: replaces `supported_languages_`, resets the fetcher | new public method next to `SetResourceRequestsAllowed()` |
| `chrome/browser/translate/translate_service.cc` | `SetLocalLanguages(PageTranslateLanguageCodes())` | first thing in `TranslateService::Initialize()` |
| `chrome/browser/translate/chrome_translate_client.cc` | `new MoltTranslateDriver(...)` | the `translate_driver_` initializer in the constructor (:191) |
| same | no automatic `BEFORE_TRANSLATE` bubble while no model is known | `ShowTranslateUI()`, after the error step is set (:298) |
| `chrome/browser/ui/views/translate/translate_bubble_view.cc` | `CreateWordmarkView(web_contents)` returns MoltBrowser's footnote in non-Google builds | `CreateWordmarkView()` (:99) and its two callers (constructor, `SwitchView()`) |

## 4. Design

### 4.1 Reading the page

The job runs `kPageTranslateScript` with `{op: "extract"}` in the primary
main frame's document, in **Translate's isolated world** (world 2, which
upstream's renderer agent would set up with Google's origin for its
script; here that agent is never asked to translate, so the world is never
given that origin, and nothing in it loads or fetches anything). It is
apart from world 1, where MoltBrowser's agent and other features run their
scripts. The page's scripts can't see the isolated world's variables or
patch the DOM methods it calls; they do see the DOM changes, as they see
Google Translate's.

What is read, in page order (depth first; an open shadow root's content
where it is shown, before the host's light children):

| Read | Why |
|---|---|
| Visible text nodes under `<body>` with at least one letter | What the user reads. "Visible": not under `display:none`/`hidden`/`content-visibility:hidden` (`checkVisibility()`), and not `visibility:hidden` itself. `display:contents` elements are looked into |
| The `<title>`'s text node | Shown in the tab and history. Treated as on screen |
| `alt` of visible `<img>` | What screen readers and broken images show |
| `placeholder` of visible text fields and textareas | Visible text, not a value |

| Never read | Why |
|---|---|
| `script`, `style`, `noscript`, `template` | Not text for people; writing there would be code |
| `code`, `pre`, `kbd`, `samp`, `var` | Code and output must stay exact |
| Form values: `input` values, `textarea` content, `select`/`option` | Translating them changes what is submitted. A `<button>`'s *text* is read (it isn't a value) |
| Editable content (`contenteditable`, design mode) | The user's own writing |
| `translate="no"`, `class="notranslate"` (on `<html>` too) | The page's own opt-out, as Chrome honors it |
| `iframe`, `object`, `embed`, `canvas`, `audio`, `video`, SVG and MathML | Other documents, or text whose layout depends on its length |
| `title` attributes, `aria-label`, `<meta>` | Not visible text; left for later (§10) |

Whitespace runs collapse to one space where the element's computed
`white-space` collapses them (the original is kept for the restore), so the
model doesn't see source indentation.

**Caps** (`page_translate_plan.h`): at most 3,000 texts, 2,000 characters
each (a longer text is skipped, not cut), 60,000 characters in all, and
60,000 DOM nodes visited; past a cap the rest of the page stays as it is
and the footnote says so. The browser checks the result again
(`ParseExtraction()`): a dict for this job's id, at most 3,000 entries of
exactly `[kind 0-3, bool, string]`, each string made valid UTF-8 without
NUL and within 6,000 bytes, 180,000 in all. Anything else fails the job
before anything is translated (a broken or compromised renderer).

The script keeps each slot (the node or element, the attribute, the value
it read) in a variable of the isolated world, under a random job id;
nothing about the page is kept in the browser beyond the texts of the
current job.

### 4.2 When a page is translated

Only when the user clicks: the bubble's *Translate*, the address bar icon,
the app menu's *Translate...*, the context menu's *Translate to X*; or, in
a regular profile, under **"always translate" for a language the user
chose** (upstream's rule: never off the record). Upstream also translates
by itself after a link from a translated page, for an `hrefTranslate` link
from a Google page, and for a target an embedder predefines; on the device
these offer the bubble instead (`translate_manager.cc` hooks). Partial
translate ("Translate selection") is off: it sends the selection to
Google's contextual-search endpoint.

The bubble no longer pops up by itself until an on-device model is
downloaded (`ShouldOfferTranslationAutomatically()`, which answers from
the last device check and starts a new one); the icon and menus still
offer translation and then explain how to get a model. Until the first
check answers (the first page in another language after start), it
doesn't pop up either.

### 4.3 Translating

1. **Device facts first** (`WebAIBackend::GetDeviceFacts()`): no local
   inference on this device, or no model downloaded, ends the job before
   the page is read. Nothing is downloaded, ever; the user downloads from
   `molt://ai`. The "Let sites ask to use on-device AI" switch is about
   sites, not the user's own translations, and isn't consulted.
2. **The page is read** (§4.1). No text with letters: done, and no model is
   loaded.
3. **The model is loaded** if none is (`PrepareModel()`: the loaded one,
   else the chat's default, else the smallest downloaded; the same as for
   the web APIs, so page translation never swaps out the chat's model).
4. **Pieces and batches.** Each text goes through the Translator's
   `SplitForTranslation()`: surrounding whitespace and text without letters
   (numbers, punctuation) stay as they are; a long text is cut at sentence
   ends. Pieces are packed, **visible texts first, then the rest in page
   order**, into batches of at most 16 texts and 384 estimated tokens (one
   piece's budget, `kTranslatorPieceTokens`), so neighbouring texts (the
   parts of a sentence split by a link) travel together.
5. **One generation per batch** under `BuildTranslatorBatchPrompt()`: the
   Translator's kind of fixed instruction naming the two languages, the
   texts as a JSON array in the user turn (untrusted content to the
   inference service), decoded under `{"translations": [string × n]}` with
   `minItems = maxItems = n`, so the model can only answer with one
   translation per text. Temperature 0.2, no reasoning, a reply budget of
   at most 832 tokens.
6. **The reply is checked** (`ReadBatchReply()`): strict JSON
   (`base::JSONReader`), one member, exactly n strings, each made valid
   UTF-8 without NUL; each translation then passes the Translator's
   `CleanTranslation()` (trimmed, unquoted, not empty) and must not grow
   past 4× its text + 256 bytes. A text that fails is tried again alone; a
   reply that fails as a whole is split in two and both halves go first;
   a text that fails alone keeps its original. Six failed batches in a row
   end the job.
7. **Written back as it goes**: a text is written once all its pieces are
   settled, so the visible part of the page changes first.
8. **Scheduling**: the generations go through the web AI backend's
   scheduler like a web page's (one generation at a time for all pages and
   tabs, visible tabs first, each tab its own "site"), so the chat waits
   behind at most one batch. A full queue is retried after a second, 60
   times at most.
9. **Ends** when every piece is settled, on *Stop*, after 15 minutes (the
   backend also cancels any one generation after 5), or when the page is
   gone. A new translation (another target) replaces a running one; a
   navigation drops it without a report, as upstream's does when the
   page's agent goes away.

### 4.4 Writing back, and "Show original"

`{op: "apply", writes: [[slot, text], ...]}` sets `Text.nodeValue` or
`setAttribute("alt" | "placeholder", text)`: text only, never HTML. The
model's text reaches the script as a JSON string inside the script's
arguments (`base::JSONWriter`), so it can't become code there either. A
slot is written only if it still holds what was read, is still in the
document, and isn't (by now) inside `script`, `style`, `noscript`,
`template`, `textarea`, `iframe`, `object`, `embed` or editable content,
whatever the page did since. Each slot is written once per job.

*Show original* (`RevertTranslation()`) cancels a running job and runs
`{op: "revert"}`: every slot that still holds the translation gets back the
exact value read (whitespace included); a slot the page changed since stays
the page's. A new translation of the same document (another target
language, or after the page came back from the back/forward cache) first
restores the previous one the same way, so it always starts from the
original.

### 4.5 Languages

Translate's list is the Translator's table (`web_ai::kTranslatorLanguages`,
20 languages), installed by `TranslateService::Initialize()` before
Translate may make requests, in Translate's codes: `zh` and `zh-Hant` are
`zh-CN` and `zh-TW`, as Translate's language detection, prefs and bubble
name them (`PageTranslateLanguageCodes()`). The bubble, the settings page's
"offer to translate" languages and `translate-internals` show these 20, the
same for every user and model. The list has no fetcher, so
`translate.googleapis.com/translate_a/l` is never requested.

A translation is resolved against the table with the Translator's best
fit (`ResolvePagePair()`), from the language the page's detection (or the
user, under *Options → Page is not in X*) gave: no language detected is
`UNKNOWN_LANGUAGE` (nothing on the device can detect it, so it is not sent
as "und"); a language outside the table is `UNSUPPORTED_LANGUAGE`; the
same language as the target is `IDENTICAL_LANGUAGES`. The reported source
is the table entry's code, so Translate counts it as supported.

### 4.6 The bubble

The bubble is upstream's (tabs, options, always/never translate, language
pickers, error view with *Try again*). MoltBrowser fills its footnote,
which Chrome-branded builds use for Google Translate's wordmark and other
builds leave empty, with a status line that follows the tab:

| State | Footnote | Link |
|---|---|---|
| Before | "Translation runs on this device: the page's text isn't sent anywhere." In MoltNet also: "The site can see the translation, and so the language you read." | |
| No model (before, or failed) | "Translation runs on this device and needs an AI model. Nothing is downloaded until you choose one." | *Choose a model* (opens `molt://ai` in a new tab) |
| Device can't run models | "This device can't run on-device AI, so pages can't be translated here." | |
| Working | "Translating on this device... 12 of 80" | *Stop* |
| Done | "Translated on this device. Machine translation can be wrong.", or that the page was long / some text was left as it was / it was stopped | |
| Failed | why: the model didn't load (*Choose a model*), the language wasn't detected (choose it under Options), the pair isn't covered, already in that language, the page couldn't be read, model errors, timed out | |

*Stop* keeps what is translated and reports a (partial) translation, so
*Show original* can undo it; stopped before anything changed, it is an
error with nothing to undo.

### 4.7 What Translate is told

| Job end | `TranslateErrors` | Page state |
|---|---|---|
| Everything settled, something written | `NONE` | translated (partial if texts failed or the page was cut at a cap) |
| Nothing to translate | `NONE` | "translated", unchanged |
| Every text failed, nothing written | `TRANSLATION_ERROR` | original |
| Stop / model errors / page gone, something written | `NONE` | translated, partial |
| Same, nothing written | `TRANSLATION_ERROR` | original |
| Timed out, nothing written | `TRANSLATION_TIMEOUT` | original |
| No model, device can't, model didn't load | `INITIALIZATION_ERROR` | original |
| Language unknown / unsupported / identical | `UNKNOWN_LANGUAGE` / `UNSUPPORTED_LANGUAGE` / `IDENTICAL_LANGUAGES` | original |

## 5. MoltNet and Incognito

**Incognito** behaves as upstream's Translate does there, on the device:
translation on the user's click, no "always translate", nothing about the
page kept anywhere (the model holds no conversation for it).

**MoltNet: on, by click only.** The two features MoltNet already treats
differently pull in different directions:

- The **web `Translator` API is off** in MoltNet windows: a site calls it
  itself, as often as it likes, with text it picks, and times the answers
  with `performance.now()`. That fingerprints the model and the machine on
  every visit without the user doing anything, and Tor Browser has no such
  API.
- The **agent is on** (on-device models only): the user starts it, and
  what it does in a page (what it types, how fast) is visible to that page
  too, once per request.

Page translation is the agent's case: nothing runs unless the user clicks
*Translate* in that tab, once per page. Nothing leaves the device, so
MoltNet's rule that whatever a MoltNet window causes goes through Tor or
doesn't happen holds trivially (upstream's script fetch, through the
browser-wide context, was the one Translate request that went around
Tor). And the alternative is worse: a MoltNet user who can't translate a
page in the exit country's language copies its text into a regular
window's translator or chat, which links the two contexts.

What a page can learn when the user translates it in MoltNet, and what is
done about it:

- **That it was translated, into which language, and the translation
  itself.** The page sees its DOM change (as with any translator). The
  language is the one the user reads, which MoltNet otherwise hides (every
  MoltNet window reports en-US). The bubble says so before the click, in
  MoltNet windows only.
- **Which model, roughly.** The page chose the text and sees the model's
  exact output. One click gives one sample; a page can't trigger more
  without the user.
- **Timing.** The page sees when texts change, which says something about
  the machine's speed.
- **No automatic translation:** "always translate" doesn't apply off the
  record (upstream's rule; MoltNet's profile is off the record), and links,
  `hrefTranslate` and predefined targets never translate by themselves
  (§4.2), so a site can't make a MoltNet tab translate page after page.

`docs/MOLTNET_COMPLIANCE.md` §1.14, §2 and §5.7 say this for users.

## 6. Google's path: dropped, not a setting

An off-by-default "use Google Translate" setting doesn't fall out of this
design: MoltBrowser builds have no Google API key, so upstream's
`TranslateManager` refused to offer or run translation in a stock build
(§1.6); the setting would do nothing for nearly everyone, and would need
Google's language list (and its fetch) and a MoltNet exception. With
`TranslatesOnDevice()` true for every tab, Google's script is never fetched
even for a user who exports `GOOGLE_API_KEY`, which closes the MoltNet gap
that described (`docs/MOLTNET_COMPLIANCE.md` §5.7). Upstream's code for it
is still compiled (the hooks only bypass it).

## 7. Threat model

| Who | Can | What stops it |
|---|---|---|
| The page (scripts) | Change the DOM before, during and after a translation; move or remove the nodes being translated; nest them in `script`/`style`/editable content | The script re-checks every slot before writing (same value as read, connected, not in an unsafe container); writes are text; it restores only what still holds the translation |
| The page (text) | Put instructions in its text ("ignore the above...") | The text is the untrusted user turn; MoltBrowser's instruction is fixed; the reply can only be n strings (grammar); each is checked and capped. What remains: the page's text steering the translation of other text **in the same batch** (up to 16 neighbouring texts), e.g. a comment steering the translation of the article next to it. Its effect is plain text in that same page, whose author controls it anyway; the bubble labels machine translation as such, and *Show original* restores it |
| The page (size) | Huge DOMs, huge texts, many nodes | Caps in the script (nodes visited, texts, characters) and again in the browser; linear walks, no regular expression over more than one capped text; batch, reply and time caps |
| A compromised renderer | Return anything from the scripts | `ParseExtraction()` / `ParseApplyResult()` check shape, caps and UTF-8 (fuzzed). It can show the user anything anyway; it never gets more than the texts of its own page and their translations |
| The model (steered or broken) | Write anything inside the JSON strings, the wrong number of them, or nothing usable | Constrained decoding plus the browser's checks; bounded retries; a failing text keeps its original; never markup or script |
| Other sites | Nothing | Only the tab's own main frame is read or written; iframes aren't touched |
| The network | Nothing | Nothing is sent: no script, no language list, no translation call |

Fingerprinting: the language list is fixed (the same 20 for everyone);
what a translated page can learn is in §5.

## 8. Network traffic

| Before | After |
|---|---|
| `translate.googleapis.com/translate_a/l?client=chrome&hl=..&key=dummytoken` when the bubble, the language settings or `translate-internals` asked for languages | never |
| `translate.googleapis.com/translate_a/element.js` (browser-wide context) and the translation calls with the page's text (the page's context), on *Translate*, only with a user-supplied `GOOGLE_API_KEY` | never; the page is translated on the device |
| Partial translate's selection to Google's contextual search (reachable only with a key and Google as the search engine) | off |

Unchanged: the Translate Ranker model stays off (`TranslateRankerQuery`,
`TranslateRankerEnforcement`); language detection is CLD3 in the renderer.

## 9. Tests and verification

Run here (no Chromium checkout):

- The `molt_ai_unittests` sources, built standalone with system gtest
  against `tools/fuzz/stand_ins` and the web AI round's functional base
  shims (clang ASan+UBSan, g++ -O2 -Werror, Chromium's warnings with
  `-fno-exceptions -fno-rtti`): `page_translate_plan_unittest.cc`
  (languages, pairs, checks, batches and caps, visible first, whitespace
  kept, retries and splitting, double answers, forged batches, reports),
  `page_translate_messages_unittest.cc`, `page_translate_io_unittest.cc`
  (script arguments as JSON, hostile model text, extraction shapes and
  caps, apply results, batch replies), `page_translation_job_unittest.cc` (a
  whole job against a fake page and backend: order and schema of the first
  batch, no model / no device / no text / model fails / unreadable page,
  halving, too many failures, throttling, Stop during a generation and
  during a write, the deadline, a page that loses the job, a gone page, a
  destroyed job), and the new `web_ai_translation_unittest.cc` cases (every
  batch schema through the service's converter, the batch prompt).
- A syntax pass over `molt_translate_driver.cc`, `page_translate_status.cc`,
  `page_translate_footnote_view.cc` and the modified `translate_driver.h`
  against declaration stand-ins with the 152 signatures.
- `molt_page_translate_fuzzer` (stand_ins tier), 4 minutes, about 340,000
  runs, no finding; `molt_web_ai_translation_fuzzer` smoke and 1 minute.
- `tools/webui_checks/page_translate_test.js` in jsdom (§4.1, §4.4), with
  `npm test`, `check_embedded_js.py` and `check_no_inline_handlers.py`.

Needs the build machine:

- `autoninja ... chrome molt_ai_unittests`: the hooks in
  `translate_manager.cc`, `translate_language_list.cc`,
  `translate_service.cc`, `chrome_translate_client.cc` and
  `translate_bubble_view.cc` were checked by reading them against 152, not
  compiled; `molt_page_translate_fuzzer` in a libFuzzer build.
- Upstream's own Translate unit and browser tests assume Google's path
  (script fetch, language-list fetch, API key) and are not expected to pass
  unchanged; they aren't part of MoltBrowser's test targets.

In the browser (each in a regular, an Incognito and a MoltNet window):

1. Without a model: no bubble pops up on a German page; the icon's bubble
   says a model is needed; *Choose a model* opens `molt://ai`; *Translate*
   fails with the same footnote; nothing is downloaded.
2. With a model: translate `de.wikipedia.org`: the visible part changes
   first, the footnote counts up, *Stop* stops it, *Show original* restores
   the page exactly (compare `document.body.innerHTML` before and after in
   DevTools), translating into another language starts from the original.
3. A page with `translate="no"`, forms, `contenteditable`, code blocks and
   a shadow DOM component: only visible prose changes.
4. A capture (`docs/NETWORK_TRAFFIC.md`, "How we'll verify"): no request to
   `translate.googleapis.com` or `translate-pa.googleapis.com` while the
   bubble shows, the language settings open, and a page is translated;
   none at startup.
5. MoltNet: the footnote's note shows; "Always translate" isn't offered;
   a link from a translated page doesn't translate the next page.
6. `chrome://translate-internals`: the language list is the 20 languages.

## 10. Not done, and follow-ups

- **New content** a page adds after the translation (infinite scroll, a
  single-page app's next view) stays in its language; *Translate* again
  reads the page again. Upstream's element.js watches mutations.
- **Scrolling** doesn't re-prioritize: what was visible when the page was
  read goes first.
- **Sentences split by markup** are translated part by part (in one batch,
  with an instruction to keep them coherent); word order across a link can
  read oddly. Translating whole blocks with their markup would need an
  HTML round trip, which this design avoids on purpose.
- **Frames**: only the main frame (upstream's default too).
- **`title` attributes, `aria-label`** and other accessible names are not
  translated.
- **Continuing a translation across links** in the same tab is off; an
  explicit per-tab "keep translating this site" could bring it back without
  letting a page drive it.
- **Show original during a second translation** (a translated page being
  translated into another language) restores the page but, as upstream,
  leaves Translate believing a translation is pending until the next page.
- **Android and iOS**: Android uses the same driver (its message UI has no
  footnote, so "no model" shows as a generic error); iOS keeps upstream's
  driver (`TranslatesOnDevice()` false) and so upstream's keyless
  behaviour.
- **React and similar frameworks** may re-render a node the translation
  changed; the next *Translate* picks it up. Only text node values change
  (no nodes are added or removed), the least disruptive kind of change.
- **Language detection** (CLD3) errors on short or mixed pages are
  corrected by the user under *Options*.
- **Policies**: Translate's own policy (`TranslateEnabled`) turns this off,
  as upstream. The on-device AI policies (`BuiltInAIAPIsEnabled`,
  `GenAILocalFoundationalModelSettings`) govern sites' AI APIs and Chrome's
  model, and aren't consulted.
