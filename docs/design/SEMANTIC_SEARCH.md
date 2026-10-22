# Semantic search over AI memory

Status: implemented (October 2026), pending a compile on the build machine
and the model pin. The model's revision, SHA-256 and size are **empty**, so
the model is not offered at all until the owner pins it ("Pinning the
model" below). Until then molt://memory behaves as before (keyword
hashing), with the new page layout.

Roadmap item: "Semantic history search with real on-device embeddings".
Builds on the sandboxed inference service (`SANDBOXED_INFERENCE.md`).

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
only this page used it). Every handler checks its arguments with
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

## 7. Not done, and why

- **Omnibox / history hook.** A "search memory by meaning" omnibox provider
  needs an asynchronous `AutocompleteProvider` registered in upstream
  `components/omnibox` (patched files, plus results that must not appear in
  MoltNet or Incognito windows). Not small enough for this change. The
  cheap version later: molt://memory accepting `?q=` and an `@memory`
  omnibox prefix next to the existing `@ai` one.
- **GPU.** The service already runs llama.cpp with Metal on macOS; the
  embedding context uses the same build. No Vulkan yet (roadmap).
- **A smaller fallback model** for low-RAM machines (multilingual-e5-small)
  would be one more registry entry; not added until 0.6B has been measured
  on real hardware.

## 8. Verification

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

Needs the build machine: a real compile and `molt_ai_unittests`
(`embedding_process_host_unittest.cc` and `memory_backend_unittest.cc` use
real `//base` task environments), the model's speed and memory on real
hardware, and the sandbox with a real model on each platform.
