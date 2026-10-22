# Semantic search over AI memory

Status: implemented (October 2026), pending a compile on the build machine
and the model pin. The model's revision, SHA-256 and size are **empty**, so
the model is not offered at all until the owner pins it ("Pinning the
model" below). Until then molt://memory behaves as before (keyword
hashing), with the new page layout.

Roadmap item: "Semantic history search with real on-device embeddings".
Builds on the sandboxed inference service (`SANDBOXED_INFERENCE.md`).
The `@memory` address-bar shortcut and `molt://memory/?q=` are in §7.

## 1. What it does

molt://memory's search box finds a page from what it was about, in the
user's own words and in any of the model's languages, not only from words
the page contained. Results show one entry per page: the title (a link
only for http(s) URLs), the URL, the date and a snippet of the best
matching passage, with the query's words marked.

- The vectors come from a small text-embedding model run by llama.cpp in
  the sandboxed inference service. The user downloads it once, on request,
  from the memory page.
- It only ever reads what AI memory already stores: memory is opt-in,
  encrypted at rest, skips the privacy blocklist, and never records MoltNet
  (off-the-record profiles have no memory at all).
- Keyword hashing (`HashingEmbedder` v3) stays the fallback: before the
  model is installed, while its index is being built, after it is removed,
  and when it can't run.

## 2. Choosing the model

Requirements: permissive license, multilingual, small enough for a
laptop's CPU, an official GGUF that llama.cpp b11641 runs, and vectors
that can be cut to 256 dimensions (the hot index's 1 KB per passage, so
its 50,000-passage cap and RAM budget don't change).

**Chosen: Qwen3-Embedding 0.6B** (Qwen team, Apache 2.0), Q8_0 GGUF from
the Qwen organization's own Hugging Face repository (about 640 MB).

- Multilingual (100+ languages) and trained for retrieval with an
  instruction on the query side only.
- Trained with Matryoshka representation learning, so its 1024-dimensional
  vectors can be cut to their first 256 components and re-normalized with
  little loss. Models without that (multilingual-e5, bge-m3, granite
  multilingual) would need their full 384/768/1024 dimensions: up to four
  times the hot index's memory.
- Architecture `qwen3` with last-token pooling: the same decoder path
  llama.cpp already runs for the Qwen3 chat models MoltBrowser ships, so
  no new model architecture code is exposed to hostile input.
- Published by the model's authors (provenance), unlike most community
  GGUF conversions.

Considered and not chosen:

| Model | Why not |
|---|---|
| multilingual-e5-small / -base (MIT) | Smaller and faster, but weaker retrieval and no Matryoshka truncation; XLM-R tokenizer path. A good fallback if 0.6B proves too slow on low-end machines. |
| bge-m3 (MIT) | Strong, but about the same size with 1024 fixed dimensions (4 KB per passage). |
| EmbeddingGemma 300M | Gemma terms of use, not a permissive license. |
| nomic-embed-text v1.5 (Apache 2.0) | Matryoshka, but mainly English. |

Settings (registry entry in
`src/chrome/browser/molt_ai/memory/embedding_model_registry.cc`): last-token
pooling over a trailing `<|endoftext|>` (which must tokenize to exactly one
token, or the load fails), 512 tokens per text (memory chunks are about 600
characters), 256 of 1024 dimensions, architecture `qwen3`, query prefix
`Instruct: Given a web search query, retrieve relevant passages that answer
the query\nQuery:`, no document prefix. About 1.5 GiB of free RAM to load.
The repository and file names are marked `TODO(pin)`: they could not be
confirmed from here (huggingface.co was unreachable).

## 3. Architecture

```
molt://memory (WebUI)          MemoryService (UI)         MemoryBackend (memory worker)
  memSearch ---------------->  SearchByMeaning ---------> Query: embed query, cosine over
  memSemanticStatus -------->  GetSemanticStatus           the hot index, best passage per
  memModelDownload/Cancel/     EmbeddingModelManager (UI,  page, BestSnippet()
  Remove ------------------->  one per browser): offer,    Pump: background embedding of
                               download, remove            pending chunks and migration
                                       |                         |
                                       v                         v
                               LlamaEmbedder (MemoryEmbedder) --posts to UI-->
                               EmbeddingProcessHost (UI, one per browser)
                                       |  ReadOnlyFile(GGUF), Embed(texts)
                                       v
                     MoltInferenceService.LoadEmbeddingModel (sandboxed utility process)
                       -> MoltEmbeddingModelImpl -> LlamaEmbeddingEngine (llama.cpp)
```

### 3.1 The service op (`chrome/services/molt_ai/inference/`)

- `molt_inference.mojom`: `MoltInferenceService.LoadEmbeddingModel(params,
  pending_receiver<EmbeddingModel>) => (status, description?)` and
  `EmbeddingModel.Embed(array<string> texts) => (array<float>? vectors)`,
  appended to the existing interface. The GGUF crosses as a
  `mojo_base.mojom.ReadOnlyFile`, never a path. A process loads either a
  chat model or an embedding model, once.
- Caps (mojom constants, checked in the service, a violation is a bad
  message): 16 texts per call, 1 to 32,768 bytes of valid UTF-8 per text,
  8 to 2,048 tokens per text, at most 4,096 dimensions, a terminator of at
  most 64 bytes, 8 calls queued.
- `LlamaEmbeddingEngine` (std + `llama.h` only): one text at a time from a
  cleared context: tokenize as plain text (special tokens in page text stay
  text), cut to `min(n_ctx, n_batch, n_ubatch, max_tokens)` tokens (keeping
  the terminator or the model's EOS/SEP last), decode with embeddings on,
  `llama_get_embeddings_seq(ctx, 0)`, keep the first `dimensions`
  components and L2-normalize (`engine/embedding_utils.*`). A text with no
  usable vector comes back as zeros. Cancellation between texts.
- Every new llama.cpp call was audited for exceptions at b11641 (list at
  the top of `llama_embedding_engine.cc`, summary appended to the checked
  list in `llama_engine.cc`). `llama_get_embeddings_ith` is deliberately
  not used (it aborts on a bad index in builds without NDEBUG); only MEAN,
  CLS and LAST pooling are ever set.

### 3.2 The browser side (`chrome/browser/molt_ai/runtime/`)

`EmbeddingProcessHost` launches the service ("On-Device AI Search Model" in
the task manager), loads the model and runs one `Embed()` at a time.
Queries jump the queue ahead of background work. It treats the process as
hostile, like `InferenceProcessHost`:

- the load's description must match the registry (architecture, native and
  requested dimensions, tokens per text);
- every reply is checked by `embedding_validator.*` (std-only): exactly
  texts x dimensions floats, all finite, each vector unit length (within
  1e-3) or all zeros; anything else kills the process;
- deadlines (load 3 min, call 3 min), a footprint watchdog (2 x (file +
  1 GiB)), `CrashLoopGuard` back-off, exit after 5 idle minutes, on
  critical memory pressure and at shutdown; free RAM is checked before
  each launch.

### 3.3 Memory (`chrome/browser/molt_ai/memory/`)

- `MemoryEmbedder` gained `space()` and an asynchronous `EmbedBatch()`
  (outcomes ok / retry later / failed / unavailable). `HashingEmbedder` is
  synchronous; `LlamaEmbedder` posts to `EmbeddingProcessHost` and replies
  on the memory worker.
- **Vector spaces.** Every vector belongs to a space, `"<model>/<dims>"`:
  `hashing-v3/256`, or `qwen3-embedding-0.6b@<first 12 hex of the pinned
  SHA-256>/256`. Re-pinning the model is a new space.
- `MemoryStorage` keeps the committed space's vectors in `chunks.embedding`
  and builds the next space in `chunk_vectors_next`, both encrypted like
  everything else. `CommitMigration()` swaps them in one transaction, only
  when every chunk has a next vector. Searches always use the committed
  space, so vectors of two spaces are never compared.
- `MemoryBackend` (the memory worker; replaces `MemoryService::Backend`)
  plans the switch (`PlanEmbedderSpace()`):
  - to the model: migrate in the background, in batches of 8 texts
    re-read from the encrypted chunk text, 50 ms apart;
  - back to hashing (model removed, or unavailable this session): re-embed
    everything synchronously on the worker and commit at once;
  - pages saved while the model is the committed embedder are stored with
    no vector ("pending") and embedded by the same pump.
- Failures: retry later backs off from 30 s to 10 min; a failing batch is
  split to find the one text that fails, which gets a zero vector after two
  failures so it can't stall the index; "unavailable" drops the model for
  the session and switches back to hashing.
- `EmbeddingModelManager` (UI, one per browser) owns the file
  (`~/.moltbrowser/models/`, shared with the chat models): offered only
  when pinned, on desktop, with AVX2/FMA on x86-64, and when the model needs
  at most 90% of total RAM; downloads from
  `huggingface.co/<repo>/resolve/<revision>/<file>` through
  `ModelDownloadJob` with the size and SHA-256 required (checked before the
  rename), after a free-disk check; a file of any other size on disk is
  "not installed" and never loaded. Remove ends the process, then deletes
  the file.

### 3.4 molt://memory

`molt_ai_memory_ui.cc`: messages `memSearch`, `memSemanticStatus`,
`memModelDownload`, `memModelCancel`, `memModelRemove` (`memQuery` is gone;
only this page used it), and `memInitialQuery` for `molt://memory/?q=`
(§7.1). Every handler checks its arguments with
`HasCallbackArgs()` / `StringArg()` and range checks. The page follows
CLAUDE.md's WebUI rules: `data-on-*` + `moltActions()`, no Trusted Types
policy at all (everything page- or model-derived becomes text through
`textContent` / text nodes), only http(s) URLs become links. It polls the
status every second while a download or index build runs, else every 5 s.
`tools/webui_checks/webui_smoke_test.js` covers the search, hostile data
and the model card's states.

## 4. Privacy gates

- **Opt-in.** The model only embeds text already in memory, which records
  nothing unless the user turned Memory on. Background embedding runs only
  while Memory is on and not paused; pausing stops it mid-index.
- **Blocklist.** Only pages `MemoryPrivacy` admitted are in memory; the
  model sees nothing else.
- **Retention, deletes, clear all.** They delete the next space's vectors
  too; a page deleted while its text is being embedded is not written back.
- **Never MoltNet.** `MemoryServiceFactory` builds no service for
  off-the-record profiles, so MoltNet and Incognito pages are never
  recorded, embedded or searched; the download is refused from those
  profiles and always uses the regular profile's network context.
- **On device.** Texts and queries go only to the sandboxed process, which
  has no network or disk access and clears its context before each text.
  Nothing is logged.
- **Network.** One user-initiated download from huggingface.co
  (`docs/NETWORK_TRAFFIC.md`); nothing else.

## 5. Threat model

- *Hostile page text* (already in memory) reaches only the tokenizer and
  model in the sandbox; special-token text is not parsed as special; length
  is capped before tokenizing.
- *A malicious model file* would have to match the pinned SHA-256, and is
  still only parsed in the sandbox.
- *A compromised service* can return wrong vectors: it can reorder the
  user's own search results, nothing more. Shapes, counts and values are
  validated before use, so it can't corrupt browser memory, and it has no
  way to send what it sees anywhere.
- *A model that hangs, crashes or grows*: deadlines, the footprint watchdog
  and the crash-loop guard end it; memory falls back to hashing.

## 6. Pinning the model (owner, on a machine with internet access)

Never type a hash in by hand. From the repository root:

1. Dry run: `python3 tools/models/pin_models.py --only
   qwen3-embedding-0.6b`. It prints the commit, SHA-256 and size it would
   pin, or `FAIL ... not found` if the repository or file name is wrong.
   In that case open the repository's file list on Hugging Face, take the
   Q8_0 GGUF's exact name (and the repository's), fix them in
   `embedding_model_registry.cc` and run it again. Set `HF_TOKEN` if the
   repository is gated.
2. Pin: the same command with `--write`. It fills `/*revision=*/`,
   `/*sha256=*/` and `/*size=*/` in the registry and nothing else.
3. Check independently, using the pinned values:
   `curl -L -o /tmp/qe.gguf
   https://huggingface.co/<repo>/resolve/<revision>/<file>`, then
   `sha256sum /tmp/qe.gguf` (macOS: `shasum -a 256`) and the byte size
   (`stat -c %s`, macOS `stat -f %z`) must equal what was written, and the
   SHA-256 must match the one Hugging Face shows on the file's page.
4. Check the file is what the registry says: `pip install gguf`, then
   `gguf-dump --no-tensors /tmp/qe.gguf` must show
   `general.architecture = qwen3` and `qwen3.embedding_length = 1024`. If
   the file differs from about 640 MB, update `approx_size_bytes`.
5. Re-read the license at that revision (Apache 2.0).
6. Remove the `TODO(pin)` comments, run
   `python3 tools/models/pin_models_test.py` (it fails on a half-filled
   pin), and commit. The lint workflow runs the same test.

Re-pinning later (a new revision or quantization) changes the vector space,
so installed copies rebuild their index in the background after the update.

## 7. The @memory omnibox shortcut

Status: implemented (October 2026), pending a compile on the build machine.

### 7.1 What it does

- **`@memory <words>`** in the address bar shows a default row, *Search
  memory for "words"*, and under it the best five pages from memory (title
  and URL). Enter opens `molt://memory/?q=<words>`, which runs that search
  on the page; choosing a page opens it.
- **`@memory`** alone: one row that opens molt://memory. **`@m` ..
  `@memor`**: a hint row (never the default) where the shortcut works.
- **`molt://memory/?q=<words>`** opens the page with that search run. The
  page asks the browser for it (`memInitialQuery`); the handler reads the
  *committed* URL, never what the page says, and parses its query with
  `molt_memory::QueryFromUrlQuery()`: form-urlencoded, the first `q` wins,
  then normalized (valid UTF-8, no control, bidi or BOM characters, single
  spaces, at most 500 characters, the search box's limit). The page only
  ever puts it in the search box as a value. `molt_memory_query_fuzzer`
  covers the parser, the URL it is built from and the round trip.
- Matching words first, at once; by meaning when the on-device model is
  already loaded and answers in time (7.4).

The keyword "@memory" is matched case-insensitively and must be followed by
whitespace or the end ("@memoryfoo" is ordinary text). The pure logic
(recognizing the shortcut, the query, the URL in both directions, the
profile gate and the rows) is std-only in
`components/omnibox/browser/molt_memory_query.*`.

### 7.2 How Chromium 152 does @history, and the hook chosen

Upstream's `@bookmarks`, `@history`, `@tabs` (and `@gemini`, `@page`,
`@aimode`) are **starter-pack keywords**: `TemplateURL`s defined in
`components/search_engines/template_url_starter_pack_data.cc` (an id, name
and keyword string resources, a search URL such as
`chrome://history/?q={searchTerms}`), written into every profile's keyword
table and versioned (`kCurrentDataVersion`, 13 in 152).
`FeaturedSearchProvider` lists them when the user types "@", choosing one
enters keyword mode, and in keyword mode
`AutocompleteController::ShouldRunProvider()` runs only that scope's
providers (for `@history`: HistoryQuick, HistoryURL, HistoryEmbeddings and
FeaturedSearch) plus the Search and Keyword providers, which keyword mode
needs. `HistoryEmbeddingsProvider` reaches its //chrome service through a
virtual on `AutocompleteProviderClient` (`GetHistoryEmbeddingsSearch()`).

A starter-pack `@memory` was not chosen:

- it touches many upstream files: a `StarterPackId` value (a generated Java
  enum), a `SearchEngineType` (a metrics enum), two string resources, the
  data version, icons, `FeaturedSearchProvider`, the controller's scope
  switch;
- it is persisted in each profile's Web Data, so it outlives a downgrade or
  the feature;
- `TemplateURLService` is shared with off-the-record profiles (they use
  their original profile's), so `@memory` would be listed and usable in
  Incognito and MoltNet unless more hooks hid it;
- keyword mode keeps the Search provider running.

Chosen: **a provider for the text `@memory`**, like MoltBrowser's existing
`@ai` (`AiPromptProvider`), plus **one gate**:

- `MoltMemoryProvider` (`components/omnibox/browser/molt_memory_provider.*`,
  type `TYPE_BUILTIN` like `AiPromptProvider`; a new provider type would
  have meant the metrics proto and several switch statements). It is
  registered next to `AiPromptProvider` for controllers that have built-in
  providers (the omnibox and its classifier).
- The gate, first thing in `ShouldRunProvider()`: for `@memory` input only
  the memory provider runs. Because the controller asks the same function
  in `Start()`, `Stop()`, when aggregating matches and when deciding it is
  done, no other provider sees, answers or is waited for. `Start()` also
  stops what the others still do for the text typed before.
- Reaching memory (in //chrome) without touching `AutocompleteProviderClient`
  and `ChromeAutocompleteProviderClient`: `MoltMemorySearch`
  (`molt_memory_search.*`) is an interface plus a registry keyed by the
  profile's `PrefService`, the one `AutocompleteProviderClient::GetPrefs()`
  returns for that profile's omnibox. `MemoryService` owns a
  `MemoryOmniboxSearch` (`chrome/browser/molt_ai/memory/`) registered under
  its profile's `PrefService` until `Shutdown()`.

Upstream edits, each marked `MoltBrowser:` (none in files another workstream
owns):

| File | Hook | In 152's |
|---|---|---|
| `components/omnibox/browser/autocomplete_controller.cc` | include; register `MoltMemoryProvider`; remember the `AiPromptProvider` (the existing MoltBrowser hook) | `InitializeSyncProviders()` |
| same | the gate, for `@memory` and `@ai` input (7.7) | top of `ShouldRunProvider()` |
| same | stop the other providers for that input | the provider loop of `Start()` |
| `components/omnibox/browser/autocomplete_controller.h` | `molt_memory_provider_` and `ai_prompt_provider_` members | the providers' `raw_ptr`s |
| `chrome/browser/ui/omnibox/chrome_omnibox_client.cc` | no action prediction for that text | `OnTextChanged()` |
| same | no shortcut saved for that text | `OnSuccessfulNavigation()` |
| `components/omnibox/browser/BUILD.gn` | sources, three small targets | |

### 7.3 Privacy, from the code path

- **Never in Incognito, Guest or MoltNet.** `MemoryServiceFactory` builds a
  service only for regular original profiles
  (`WithRegular(kOriginalOnly)`, `WithGuest(kNone)`), and only a
  `MemoryService` registers a search, under its own profile's
  `PrefService`. An off-the-record or Guest omnibox's client returns that
  profile's own `PrefService`, under which nothing is registered. Before even
  looking, the provider asks the client `IsOffTheRecord()` and
  `IsGuestSession()` (MoltNet is off the record): `ShortcutStateFor()` gives
  `kHidden`, and the provider adds no row and no hint. "@memory ..." still
  reaches no other provider there (the gate doesn't depend on the
  profile), so the popup stays empty and Enter does nothing: the text is
  never sent to a search engine from those windows either. The memory
  page's `@memory` tip is only shown where memory exists.
- **Never while Memory is off or paused.** `kOff` / `kPaused`: one row
  saying so, which opens molt://memory without the words, and no search;
  `MemoryService::SearchForOmnibox()` checks again.
- **Nothing goes to a search engine or any server.**
  - *Remote suggest and every other provider:* the gate. The Search
    provider (default and keyword suggest requests), Document, enterprise
    search aggregator, zero-suggest, on-device head, Keyword (extension
    omnibox API), unscoped extension, contextual search and history
    embeddings providers are never started for `@memory` input.
  - *Preconnect and prerender:* `ChromeOmniboxClient::OnTextChanged()`
    feeds `AutocompleteActionPredictor`, which learns (text, URL) pairs
    from what the user opens and then preconnects or prerenders the
    current match (`DoPreconnect()` / `DoPrerender()`). Without the hook,
    after a few visits to a page from the same `@memory` text, arrowing to
    it would preconnect or prerender it. The hook skips both learning and
    predicting for `@memory` text.
  - *Search prefetch and preload:* `SearchPrefetchService::OnResultChanged()`
    only prefetches matches the suggest server marked
    (`BaseSearchProvider::ShouldPrefetch()` / `ShouldPrerender()`), and its
    and `SearchPreloadService`'s `OnNavigationLikely()` only search-type
    matches of the default engine. The shortcut's matches are `NAVSUGGEST`
    (its own rows) and `HISTORY_BODY` (pages), with no server data.
    `AutocompleteDictionaryPreloadService` only loads compression
    dictionaries already stored.
  - *Service worker:* for a query, `AutocompleteController::Start()` asks
    for the default match's service worker; that is
    `chrome://molt-memory/...`, which has none.
  - *chrome:// URLs* are never preconnected or resolved
    (`PreconnectManagerImpl::StartPreconnectUrl()` and
    `StartPreresolveHost()` require http(s)), so neither are the hint rows.
  - No match has an image or icon URL (no `BitmapFetcher`); page icons come
    from the local favicon database.
- **The query never leaves the device.** provider ->
  `MoltMemorySearch::Search()` (in process) -> `MemoryService` -> the memory
  worker (`MemoryBackend::OmniboxSearch()`) -> only for the meaning pass,
  `LlamaEmbedder` -> `EmbeddingProcessHost::EmbedIfLoaded()` -> the
  sandboxed model process, which has no network. Nothing is logged. The
  shortcuts database doesn't get `@memory` text either
  (`OnSuccessfulNavigation()` hook): it would keep memory's titles and URLs
  where Wipe memory can't reach them.
- What stays as for any navigation: opening a page from the list records a
  visit in history; `chrome://` pages are never in history; a
  `molt://memory/?q=` tab is restored with its URL like any tab.

### 7.4 Answers and the model

- **Keyword pass** (`MemoryBackend::KeywordSearch()`, synchronous on the
  memory worker): the titles and URLs of the newest 1,000 pages (decrypted
  for the search, nothing cached), plus, while the committed index is
  HashingEmbedder's, the nearest passages of the keyword index, kept only if
  they contain every word. Every word must appear (prefix match on words as
  HashingEmbedder folds them; inside a word for non-ASCII scripts); a word
  in the title counts 1.0, in the URL 0.6, in the passage 0.3; ties by
  recency (`memory_keyword_match.*`).
- **Meaning pass**, only when the committed index is the model's:
  `MemoryEmbedder::EmbedQueryIfReady()`; `LlamaEmbedder` sends the query
  through `EmbeddingProcessHost::EmbedIfLoaded()`, which embeds it only if a
  process is `kReady` with this very model and otherwise answers
  `kNotLoaded` without launching, loading or replacing anything. **Typing
  never loads the model**: it is used only when background indexing or a
  search on molt://memory left it loaded (it exits after 5 idle minutes).
  Its hits replace the keyword ones in a second, final answer.
- **The async window:** the omnibox shows that answer only if it arrives
  before `AutocompleteController`'s stop timer
  (`AutocompleteControllerConfig::stop_timer_duration`, 1.5 s after the
  last keystroke). `MoltMemoryProvider::Stop()` drops anything later, and
  the provider type is not one the controller lets update after it stopped.
- Every `Start()` sets the previous search's cancel flag: a search the
  worker hasn't begun is skipped, and a late model answer is dropped.
- The memory worker runs at best-effort priority: on a busy machine the
  keyword answer can come late; the default row never waits.

### 7.5 Page text in the omnibox

Titles and URLs come from sites. `molt_memory::SuggestionRows()` keeps only
`IsSuggestibleUrl()` URLs (http(s), a host, printable ASCII, at most 4 KB),
each once, and normalizes titles like queries (at most 256 characters;
the URL when there is no title). The provider then requires a valid
`GURL` with an http(s) scheme, runs titles through
`AutocompleteInput::SanitizeString()`, shows URLs with
`url_formatter::FormatUrl()` (IDN spoof checks) and only sets match text
and `ClassifyTermMatches()` classifications. The omnibox renders text; no
markup is ever parsed and nothing in a title can run.

### 7.6 Limits

- English strings in the provider, as `AiPromptProvider`'s.
- A custom search engine with the keyword `@memory` is shadowed.
- With the model's index committed, the keyword pass reads only titles and
  URLs (of the newest 1,000 pages); page text is found by meaning.
- No snippet in the omnibox, no Shift+Delete to forget a page from there.
- "@memory" is not in Chromium's "@" list (it isn't a starter pack); the
  hint row does that job where the shortcut works.

### 7.7 The same gate for `@ai` prompts

`AiPromptProvider` (`components/omnibox/browser/ai_prompt_provider.*`, older
than this work) turns `@ai <prompt>` into a row that opens
`chrome://molt-ai/?q=<prompt>`. Until this change the prompt was ordinary
omnibox text for everything else: as the user typed, the Search provider
sent `@ai <prompt>` to the default engine's suggest service (and offered a
web search for it), the unscoped extension provider passed it to
extensions that watch the address bar, the action predictor learned
(text, URL) pairs from it, and the shortcuts database kept it with the
row opened.

The gate now covers it. `molt_omnibox::ClassifyLocalInput()`
(`molt_local_input.*`, std-only) names the input that stays on the device:

| Input | Who claims it | Reaches |
|---|---|---|
| `@memory`, `@memory <words>` | `MoltMemoryProvider` | that provider only |
| `@ai` + whitespace + prompt (ASCII any case; Unicode spaces count) | `AiPromptProvider`, which now uses the same test (`AiPromptStart()`) so they can't drift apart | that provider only |
| anything else, `?...` included | | every provider, as before |

`ShouldRunProvider()` gives local input to its own provider (the controller
remembers both), `Start()` stops the others' leftover work, and the two
`ChromeOmniboxClient` checks (action prediction, shortcuts) use
`IsLocalOnlyInput()`. This holds in every profile; an `@ai` prompt in
Incognito or MoltNet still opens the local AI page.

`@ai` followed by whitespace with no prompt yet is local too (no row, no
request); `@ai` alone and `@aim ...` are ordinary text. Both keywords also
match in full-width forms (`＠ａｉ`, `＠ｍｅｍｏｒｙ`), which an East Asian
IME types in full-width mode: each full-width form (U+FF01-U+FF5E) is
compared as its ASCII character, one UTF-16 unit either way. Other
look-alikes (Cyrillic `а`, ...) are not folded.

**Not `?`.** In 152 a typed `?` as the first character makes
`OmniboxEditModel` call `EnterKeywordModeForDefaultSearchProvider()`, which
removes it and puts the omnibox in keyword mode for the default search
engine, Chromium's "force a web search" (MoltBrowser doesn't change
`omnibox_edit_model.cc`). The text that follows is a web search, with
suggestions, by the user's choice. `AiPromptProvider` used to claim a
leading `?` too, which only text that keeps its `?` (pasted, dropped, or
with no default search engine) reached: it got "Ask MoltBrowser AI" as its
default row while the rest of the omnibox had already sent it to the search
engine for suggestions. That branch is gone, so `?` means one thing: a web
search. `?` is not gated.

## 8. Not done, and why

- **GPU.** The service already runs llama.cpp with Metal on macOS; the
  embedding context uses the same build. No Vulkan yet (roadmap).
- **A smaller fallback model** for low-RAM machines (multilingual-e5-small)
  would be one more registry entry; not added until 0.6B has been measured
  on real hardware.

## 9. Verification

Done without a Chromium checkout:

- The engine end to end against the vendored llama.cpp b11641 objects with
  tiny generated GGUFs (qwen3 with last-token pooling and terminator; BERT
  encoders with mean and CLS pooling and 30,000-character texts;
  cancellation; invalid inputs), also under ASan, UBSan and LSan.
- `embedding_utils` (15 tests), `embedding_validator` (8), the memory core
  (38, incl. spaces, snippets and the embedder interface), and
  `MemoryBackend` + `MemoryStorage` (17) against real SQLite with small
  stand-ins for `//base` and `//sql`; three seeded bugs were caught.
- The mojom with upstream's generator; the service, host, `LlamaEmbedder`,
  `EmbeddingModelManager`, `MemoryService` and the WebUI with
  `-fsyntax-only` against stand-in headers.
- `check_embedded_js.py`, `check_no_inline_handlers.py`, `npm test`
  (jsdom smoke test of every page), `tools/models/pin_models_test.py`.

The @memory shortcut (§7), without a Chromium checkout:

- `memory_omnibox_unittest.cc` (shortcut text, query, `?q=` parsing and
  building, the gate for each kind of profile and Memory state, rows),
  `omnibox_local_input_unittest.cc` (which input is local: `@memory`,
  `@ai`, never `?`) and `memory_keyword_match_unittest.cc` with system
  gtest under ASan and UBSan; the memory core's existing tests still pass.
- `MemoryBackend::OmniboxSearch()`: the real backend with an in-RAM
  stand-in for `MemoryStorage`, running the new tests of
  `memory_backend_unittest.cc` (keyword pass, upgrade by meaning, model
  not loaded, cancellation, a space change in between).
- `molt_memory_query_fuzzer`: two million runs; it catches a seeded
  bidi-character leak and a cap off by one.
- `MoltMemoryProvider` and `MoltMemorySearch` with `-fsyntax-only` against
  stand-ins of 152's omnibox headers.
- The WebUI checks and `npm test`, with a `molt://memory/?q=` page whose
  query is markup.

Needs the build machine: a real compile and `molt_ai_unittests`
(`embedding_process_host_unittest.cc` and `memory_backend_unittest.cc` use
real `//base` task environments; `memory_omnibox_provider_unittest.cc`
uses the omnibox's test support for the provider and the controller's
gate), the model's speed and memory on real hardware, the sandbox with a
real model on each platform, and the shortcut in a real omnibox (each kind
of window; no request in a network log while typing `@memory` or `@ai`).
