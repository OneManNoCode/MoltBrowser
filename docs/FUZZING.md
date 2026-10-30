# Fuzzing MoltBrowser's own parsers

MoltBrowser adds a lot of C++ that reads input it can't trust: page text
and HTML, arguments web pages pass to the built-in AI APIs, whatever the
sandboxed inference service sends back (it may be compromised), model
files, imported workflows, Tor's control port and release manifests. Unit
and mutation tests cover the expected cases. The libFuzzer targets listed
here cover the unexpected ones. Each target checks the parser's documented
promises, not only that it doesn't crash.

Every target is a `fuzzer_test()` in GN, so a Chromium build with a fuzzing
engine (and ClusterFuzz-style infrastructure) picks it up. The same source
also builds standalone with the system clang, with no Chromium checkout,
through `tools/fuzz/molt_fuzz.py`. CI runs a short, fixed-seed smoke pass of
the targets that don't need `//base` on every pull request.

## Running them standalone

You need clang with libFuzzer and the sanitizer runtimes, zlib, and (for
the `stand_ins` tier) OpenSSL's libcrypto. On Ubuntu 24.04 that is
`clang libclang-rt-dev zlib1g-dev libssl-dev`. Apple's clang has no
libFuzzer: use an LLVM release clang and set `CXX` / `CC` to it.

```bash
tools/fuzz/molt_fuzz.py list                    # name, tier, directory
tools/fuzz/molt_fuzz.py check                   # GN targets and seeds are in sync
tools/fuzz/molt_fuzz.py build                   # everything, into out/fuzz/bin/
tools/fuzz/molt_fuzz.py run --seconds 300 molt_dom_interpreter_fuzzer
tools/fuzz/molt_fuzz.py run --seconds 60 --tier std,zlib   # a whole tier
tools/fuzz/molt_fuzz.py smoke                   # what CI runs
```

Every binary is built with `-fsanitize=fuzzer,address,undefined`,
`-fno-sanitize-recover=all` and `-D_GLIBCXX_ASSERTIONS` (bounds-checked
`operator[]`, as Chromium's hardened libc++). MoltBrowser's code builds with
`-fno-exceptions`, as Chromium builds it. `run` uses the same options as the
GN target (`max_len`, dictionary) plus `-timeout=25` and
`-rss_limit_mb=2560`. A hang or an OOM is a failure, not just a crash. Use
`--seed N` for a different but repeatable run.

Output goes to `out/fuzz/`:

| Path | What |
| --- | --- |
| `out/fuzz/bin/<name>` | the fuzzer |
| `out/fuzz/corpus/<name>/` | what runs found (the checked-in seed corpus is only read) |
| `out/fuzz/artifacts/<name>/` | crashing, hanging or OOM inputs |

To reproduce, minimize and keep a finding:

```bash
out/fuzz/bin/molt_x_fuzzer out/fuzz/artifacts/molt_x_fuzzer/crash-...   # replay
out/fuzz/bin/molt_x_fuzzer -minimize_crash=1 -runs=100000 crash-...    # shrink
# Fix the product code, add a unit test, and copy the input into the
# fuzzer's corpus/<name>/ as regress_<what>, so every run replays it.
```

To fold a long run's finds back into the checked-in seeds (keep them
small):

```bash
mkdir /tmp/min
out/fuzz/bin/molt_x_fuzzer -merge=1 /tmp/min <dir>/corpus/molt_x_fuzzer out/fuzz/corpus/molt_x_fuzzer
```

Objects are cached by source and flags. After editing a header, use
`build --clean`.

### Tiers

| Tier | Builds against | In CI smoke |
| --- | --- | --- |
| `std` | the product code is std-only. The harness's `CHECK` comes from `tools/fuzz/stand_ins/base/check.h` | yes |
| `zlib` | `std` plus the system zlib (the PDF scraper) | yes |
| `llama` | `std` plus the vendored llama.cpp grammar engine (`src/llama-grammar.cpp` and the few symbols it needs from `tools/fuzz/stand_ins/llama_grammar_deps.cc`) | yes |
| `stand_ins` | code that needs `//base`, `//url`, `//crypto`, BoringSSL or `zip_reader` builds against the stand-ins in `tools/fuzz/stand_ins/` (OpenSSL's libcrypto behind the crypto ones) | no |

The stand-ins approximate Chromium (each header says how): `GURL` is a
deliberately small parser (no IDN, IPv4 forms or escapes), `JSONReader`
keeps the RFC-mode contract MoltBrowser relies on, `zip_reader` walks local
headers instead of the central directory, and `base::span` / `CHECK` abort
like Chromium's. A crash inside MoltBrowser's code is a real bug. A crash
inside a stand-in is a stand-in bug. A property that depends on URL
canonicalization is only checked loosely there, and in full in the GN
target, which uses the real libraries.

## Running them in a Chromium build

```bash
gn gen out/libfuzzer --args='use_libfuzzer=true is_asan=true is_ubsan_security=true is_debug=false dcheck_always_on=true'
autoninja -C out/libfuzzer molt_dom_interpreter_fuzzer
out/libfuzzer/molt_dom_interpreter_fuzzer -dict=chrome/browser/molt_ai/fuzzers/dicts/html.dict \
  /tmp/corpus chrome/browser/molt_ai/fuzzers/corpus/molt_dom_interpreter_fuzzer
```

`//chrome/browser/molt_ai:molt_ai_tests` depends on
`//chrome/browser/molt_ai/fuzzers:molt_fuzzers`, which depends on every
target below, so they are part of `all`. Without a fuzzing engine,
`fuzzer_test()` is a no-op group. Each target's `libfuzzer_options`,
`dict` and seed corpus match `molt_fuzz.py`, and `molt_fuzz.py check`
fails when they drift apart.

## The targets

Ordered by exposure: input a page or a model controls comes first.

| Fuzzer | Input (who controls it) | Tier | What it checks beyond "no crash" |
| --- | --- | --- | --- |
| `molt_page_translate_fuzzer` | the extract script's result (page renderer), batch replies (model), the write script's answer | stand_ins | the page read back is within every cap, valid UTF-8 without NUL, for this job only; the plan ends (at most 3 generations per piece) with every piece settled and batches within their caps; the batch instruction never holds page text and the user turn reads back exactly; a slot is written at most once, keeps its surrounding whitespace, within the growth cap; the write script carries the writes exactly |
| `molt_dom_interpreter_fuzzer` | page HTML (page) | std | linear scanners: no timeout on 64 KB; no tag left in the main text; links/buttons/forms are the ones `ParseHTML()` reports |
| `molt_agent_safety_fuzzer` | page text, script results, selectors (page, workflow file) | stand_ins | sanitized text can't forge the untrusted-content fence or a chat-template token; only http(s) URLs pass; probes keep their count |
| `molt_agent_actions_fuzzer` | element snapshot (page), model reply (model) | stand_ins | observations capped and clean; every action schema is in the service's subset; accepted actions name listed elements and respect every cap; the chat never types model text into a password field |
| `molt_agent_tools_fuzzer` | the page's WebMCP tool list and input schemas, its answers to a call (page), the model's CALL_TOOL (model) | stand_ins | an offered tool has a valid name, a one-line capped description and capped arguments, never a password or file field; its arguments' schema, and the action schema with every tool kept, convert in the service; a call that may change something asks; accepted arguments read back to the same JSON; an answer is capped and clean |
| `molt_agent_plan_fuzzer` | the model's plan and its actions with a plan step (model), element snapshots, page text (page), any order of the run's events | stand_ins | an accepted plan has 1 to 6 one-line steps within their caps, no control, bidi, zero-width or other invisible character (soft hyphen, Hangul fillers, variation selectors, tag characters; decoded independently of the planner); a plan is used only from a complete, constrained, valid reply, and an unusable one is asked for again once; the run's progress matches a reference model (only the next step, rewrites capped, triggers exact); the action schema with any plan converts in the service, and an accepted action names a step the plan allows (REPLAN and a step only with a plan); the plan and the page stay inside their own fences in both prompts, and the rules never depend on plan, page or goal text |
| `molt_agent_vision_fuzzer` | geometry script result, selectors and labels (page) | stand_ins | only in-range geometry is accepted, deterministically; tags never overlap and stay inside the image (at most the cap); annotation never touches the padding and leaves no original pixel inside any mask, so a password field's pixels never reach the model |
| `molt_agent_run_draft_fuzzer` | a web agent run saved as a draft workflow: what the model typed, its URLs and queries (model), element labels and selectors (page), the goal (user), steps that worked or failed in any order | stand_ins | refused exactly when it must be (unfinished, MoltNet, a site tool), with fixed text; a draft is Casual, unconfirmed, manual, allows nothing until exactly its sites are confirmed, then only those; it opens a page first and only http(s) ones without a secret-looking address or `{{`; descriptions, names and anchors are one clean line; a kept value never looks secret or holds a goal secret, a withheld one is a `{{variable}}` with an empty default, a secret step has no value and is never a password one; it saves and loads back the same, deterministically; display text clean, capped and idempotent; variable names plain and unique |
| `molt_automation_ai_steps_fuzzer` | model answers to AI steps (model), field lists (workflow file) | stand_ins | decisions only from exact `SUCCESS`/`FAIL`; extracted values typed and capped |
| `molt_stream_filters_fuzzer` | model output, split anywhere (model) | std | every chunk sent is valid UTF-8 without NUL; no stop string shown; the result doesn't depend on how the output is split |
| `molt_stream_validator_fuzzer` | the service's messages, in any order (compromised service) | std | the browser passes on only valid UTF-8 within its byte and chunk caps; nothing after completion |
| `molt_embedding_validator_fuzzer` | embedding replies (compromised service) | std | accepted vectors are finite, unit length or zero, exactly texts x dimensions |
| `molt_json_schema_grammar_fuzzer` | `responseConstraint` schemas (page) | std | grammars are ASCII, bounded, deterministic; refusals are short and fixed |
| `molt_json_schema_grammar_llama_fuzzer` | the same, through llama.cpp's grammar engine | llama | llama.cpp parses every grammar; a walk never empties or blows up the stacks; wherever generation may end, the text is strict JSON |
| `molt_bounded_json_fuzzer` | JSON the service reads (page) | std | depth, size and value limits; write/read round trip |
| `molt_grammar_tokens_fuzzer` | vocabulary pieces (model file) | std | the UTF-8 prefix check agrees with a full validator; control and end tokens never pass |
| `molt_prompt_utils_fuzzer` | page text, token lists | std | no chat-template special text survives neutralization, and removing the inserted zero-width spaces gives the text back |
| `molt_context_budget_fuzzer` | model metadata (model file), request sizes | std | no overflow or division by zero; plans fit the window |
| `molt_embedding_utils_fuzzer` | token lists, pooled floats (model) | std | token caps, terminators, unit vectors, NaN refused |
| `molt_image_input_fuzzer` | image sizes (page), buffers | std | every plan passes the service's check; composing reads only its input |
| `molt_audio_input_fuzzer` | audio clips: channel count, rate, frame count and samples (page, through a renderer that may be compromised), byte lengths | std | only one channel at 16 kHz with the data's frame count is accepted; each sample becomes the nearest 16-bit value of the clamped one (non-finite ones silence), short clips are padded, and every clip made passes the service's check; reads only the samples given |
| `molt_web_ai_translation_fuzzer` | text and language tags (page), replies (model) | std | pieces concatenate back exactly, no split inside a character; page text never reaches the instruction |
| `molt_web_ai_policy_fuzzer` | tags, sizes, sampling params, request order, sites and timing of calls (page) | std | saturating estimates; the scheduler and the per-site device facts match reference models |
| `molt_web_ai_conversation_fuzzer` | session traffic with images and audio clips, a page's decoded audio (page) | std | quota accounting (clips by length); evictions keep a user turn first; image and clip caps, the newest ones sent; a page's audio becomes a clip only at 16 kHz mono within 30 s; browser-authored prompt parts never depend on page text |
| `molt_web_ai_site_permissions_fuzzer` | stored prefs (damaged profile), origins (page), prompt presses | std | double-to-int reads are safe; records round-trip; table caps; the prompt's input protection matches a reference |
| `molt_memory_snippet_fuzzer` | page text in history (page) | std | snippets are pieces of the text within the cap, never splitting a character |
| `molt_memory_query_fuzzer` | `molt://memory/?q=` (whoever made the URL), omnibox text (user), titles and URLs of memory's pages (site) | std | queries and titles come out valid UTF-8, capped, with no control, bidi or BOM character and single spaces, and normalizing again changes nothing; the search URL escapes all but unreserved bytes and parses back to its query; the omnibox gate tells `@memory` and `@ai` apart and never claims text starting with `?`; suggestion rows are unique http(s) URLs with ordered relevance |
| `molt_farbling_fuzzer` | canvas/WebGL pixels, audio, font names (page) | stand_ins | noise is idempotent, at most one level on one channel, identical to its per-value rule however the buffer is laid out (RGBA/BGRA, 1-4 channels, float/8-bit); font names match across encodings, and the hashed index finds what a linear scan finds |
| `molt_moltnet_font_set_fuzzer` | font family and `local()` names, language tags, scripts, weights (page) | stand_ins | every answer is a family, face or region of MoltNet's bundled set; names match whatever their ASCII case and whitespace, and every name in the table finds its family; face matching keeps the family, the slope when it has it and a weight the face renders; fallback lists every family once; a tag's Han region ignores case and `-`/`_` |
| `molt_ocr_image_fuzzer` | an attachment's first bytes (user, site), decoded pixels (any image) | std | the PNG, JPEG, WebP and TIFF signatures are told apart as an independent check does, and only the first three are decoded; the PNM tesseract reads is refused for a wrong size or a side past 32767, otherwise it is exactly the P5 or P6 header and each pixel composited over white (computed independently), gray exactly when every pixel is, the same whether written whole or row by row |
| `molt_recorded_wav_fuzzer` | the chat page's voice recording (a WebUI renderer that may be compromised) | std | only a RIFF/WAVE file within the size limit, with a 16 kHz mono 16-bit PCM format chunk before its data (checked by an independent walk), is accepted; its samples are a non-empty, even-sized part of the input; the WAV whisper-cli reads is the fixed 44-byte header and exactly those samples, and parses back to them |
| `molt_voice_command_fuzzer` | what a speech recognizer writes (anyone near the microphone, a page's audio), in any context | std | the same command every time; over 1,000 bytes or invalid UTF-8 is refused, nothing else is; heard and carried text is valid UTF-8 without control, bidi or invisible characters, single-spaced, trimmed and capped, and set exactly for the commands that carry text; a site is host-like text (letters, digits, `.` `-` `_` `/`, no mark at either end or twice in a row) within 253 bytes, with a dot when it is an address; "yes"/"no" only after a yes/no question, dictation edits only in a text field; levels, seconds, counts and choice references in range |
| `molt_spoken_choice_fuzzer` | items a page shows (titles, channels, dates: page text) and what the user said about them | std | a parsed reference names something within its caps; the match is the same every time and the same (by position) with the items reversed; unique, ambiguous and no-match outcomes are well formed (valid distinct indices, best first, scores in (0, 1], a unique description leads by the margin); positions, numbers and relations pick the item an independent reading picks |
| `molt_voice_learning_fuzzer` | what voice mode learned, as files anything on the machine may have written: the profile's learning file (words, corrections, site hints, the switch), a routine's `.molt` workflow (imported or edited), and two utterances, a correction lined up with the first and applied to the second | stand_ins | what is read back is within the caps, every phrase learnable and no side of a correction an answer to a question, hints clean; the file saves and reads back the same; a routine voice mode would run has at most 50 steps of routine kinds, each describable, and built again reads back the same (after a step opening the first step's site when that one opens none); corrections give valid UTF-8 within the grammar's cap |
| `molt_voice_controller_fuzzer` | the voice loop's decisions over a page map whose strings are any bytes (titles, channels, labels, values, link targets, tab titles: page text, through a renderer that may be compromised) and a conversation of utterances (what anyone the browser can hear says), each planned on the state the last left, with its page waits followed up | std | planning returns and is the same twice; every spoken text and status is valid UTF-8 without control, bidi or invisible characters (an independent decoder and `hidden_chars`), within its cap and unchanged by `SpeechSafeText()`; every URL to load is https with no space or control byte; every node a page action names is on the map; tabs, counts and repeats are in range; a MoltNet window only hears that voice mode is off; a held confirmation holds only page actions, words or the planner's "yes" to a numbered question (`? words` lines: the planner asking); offered choices and what "repeat" keeps stay within their caps |
| `molt_page_map_fuzzer` | a page's accessibility tree as voice mode reads it: roles, states, strings, bounds and child links that cycle, repeat or point nowhere (page, through a renderer that may be compromised), and the map's limits | std | the work stops at its caps (nodes, depth, work, items, lists, entries) and the map is the same twice; items are numbered 1, 2, ... and name visible nodes of the input; every string is valid UTF-8 without control, bidi or invisible characters (an independent decoder and `hidden_chars`), within its cap and unchanged by `SpeechSafeText()`; a protected or credential field's value is never kept and such a field is never writable; lists are sorted by prominence, and an entry's number names an item with its node and kind |
| `molt_agent_page_map_fuzzer` | a page's accessibility tree as the web agent reads it through the page map, with select options, a URL and page text that are any bytes (page, through a renderer that may be compromised), and a model reply (model) | stand_ins | the observation is the same twice; its text is valid UTF-8 within its cap and every line one the observation writes (page text never starts a line), ELEMENTS last; at most 40 elements, each `e` and its item's number, in order, naming the item's node in a tree of the page's origin, with a one-line name without double quotes; nothing but a writable non-credential text field is typeable; a select's options are its own (at most 12, no double quotes); every element is found again on the same map; the schema converts; an accepted reply acts only on what was listed (TYPE on a text field, SELECT on a listed option); the probe names only a link's target and caps its label, and the click's question matches it |
| `molt_pdf_text_scraper_fuzzer` | PDFs (site) | zlib | inflation caps (64 MB) hold; no hang; output shape |
| `molt_docx_text_extractor_fuzzer` | .docx files (user, site) | stand_ins | zip read cap; scanner output never longer than its input |
| `molt_automation_script_fuzzer` | imported `.molt` workflows (anyone) | stand_ins | save/load round trip; scrubbing secrets is idempotent; trust is never raised by loading or saving |
| `molt_onion_location_fuzzer` | `Onion-Location` headers, hosts (site) | std | honoured only once, on https, for a valid v3 onion |
| `molt_torrc_fuzzer` | pasted bridge lines (user, from anywhere) | std | no injected torrc option; fixed options exactly once |
| `molt_http_cache_site_fuzzer` | HTTP cache keys, which carry the page's URL and sites (site) | std | the site read is a part of the key with no space, a key built as `HttpCache` builds one gives back its top-level site; the budget's decisions agree |
| `molt_tor_control_protocol_fuzzer` | control-port replies and events, port file (whoever owns the port) | stand_ins | a complete reply's verdict can't change; replies and events split off a stream agree with it and always progress; fixed field sizes; loopback ports only; SAFECOOKIE accepts only the exact hash |
| `molt_release_manifest_fuzzer` | release manifest and signature (release page) | stand_ins | only a listed key's signature over the exact bytes verifies; assets are plain, distinct names; no downgrade passes the policy |

Each fuzzer's header comment states its checks in full. The checked-in
seeds come from the unit tests' vectors, and dictionaries are in each
`dicts/`.

## CI

The `fuzz-smoke` job in `.github/workflows/lint.yml` installs clang, runs
`molt_fuzz.py check`, then `molt_fuzz.py smoke`. The smoke pass builds the
`std`, `zlib` and `llama` tiers and runs each fuzzer over its seed corpus
with `-seed=1` and a fixed `-runs` count (each fuzzer's `smoke_runs`,
about 12 s on a desktop core and 20-30 s on a CI runner). The run count
is what bounds a run, so the same clang runs the same inputs every time.
Each run also has a 60 s guard: a slower runner covers fewer inputs and still
passes. The job has a 30 minute cap. It is a regression net (every seed and
`regress_*` reproducer replays on every pull request), not a fuzzing
campaign: longer runs happen locally or on ClusterFuzz-style infrastructure.

## Bugs the fuzzers found

| Fuzzer | Bug | Fix | Regression test |
| --- | --- | --- | --- |
| `molt_agent_safety_fuzzer` | `SanitizeUntrustedText()` folded angle-bracket look-alikes (U+2039, U+203A, U+FF1C, ...) to ASCII *before* dropping control characters. A control character placed between a look-alike's UTF-8 bytes (`E2 80 00 B9`) was dropped afterwards, and the look-alike came out whole. Page text could then contain `‹‹‹` / `›››` next to the untrusted-content fence, which folding exists to prevent. | Drop control characters first, then fold (`security/agent_safety.cc`) | `AgentSafetyTest.SanitizeFoldsLookalikesSplitByControlCharacters`; `corpus/molt_agent_safety_fuzzer/regress_lookalike_split_by_control` |
| `molt_agent_tools_fuzzer` | Before it landed: `CleanResult()` trimmed a site tool's answer before turning control characters into spaces, so an answer starting with one (`\u0001answer`) kept a leading space it promised not to have. | Clean first, then trim (`agents/agent_tools.cc`) | `AgentToolsTest.ParsesCallPoll`; `corpus/molt_agent_tools_fuzzer/hostile` |
| `molt_agent_run_draft_fuzzer` | Before it landed: a draft saved from an agent run was named after the user's goal even when the goal contained `{{`, unlike every other string the draft shows (descriptions are `{{variable}}`-resolved at replay, so the draft keeps `{{` out of all of them). | The generic name instead (`automation/agent_run_draft.cc`) | `AgentRunDraftTest.NamesTheDraftAfterACleanGoal` |
| `molt_automation_script_fuzzer` | Reading an imported `.molt` file cast any 64-bit field stored as a double (`created_at_unix`, `until_unix`, `max_network_bytes`, run history...) with `static_cast<int64_t>`. A value outside int64's range (`1e300`, `1.9e19`) was undefined behaviour. | `base::saturated_cast<int64_t>` (`automation/automation_script.cc`) | `AutomationScriptTest.ImportedTimesOutsideInt64Saturate`; `corpus/molt_automation_script_fuzzer/regress_int64_overflow.molt` |

## Not covered yet

- **Shield filter lists.** The filter engine is Brave's adblock-rust crate,
  which is fuzzed upstream. MoltBrowser's C++ around it
  (`LooksLikeFilterList()`, `ParseListMetadata()` in
  `shield/molt_shield_request_utils.cc`) shares a file with
  `//net` / mojom code, so it has no standalone build. It only uses
  `//base` string helpers.
- **Code that needs a browser.** This includes the Mojo boundary itself
  (`molt_ai.mojom` messages: Mojo's own validation and Chromium's mojom
  fuzzers cover deserialization), WebUI message handlers
  (`ui/webui/molt_ai/`, whose arguments are checked by
  `molt_ai_chat_handler_helpers.h`), the password vault's store, and the
  update manager's GitHub API parsing (`update/update_manager.cc`). Only
  the pure parsing and validation logic is fuzzed.
- **Vault host matching** (`vault/vault_host_match.cc`). Its comparison is
  pure, but the file needs `//net`'s Public Suffix List, which has no
  stand-in. It is a candidate for a GN-only target.
- **Page-geometry mirrors** (`PageDevicePixels()`, `PageCssPixels()` and
  related functions in `farbling.h`). They copy upstream Blink/gfx rounding
  for inputs the renderer computes itself, and `letterbox_test.cc` checks
  them against Blink.
- **The llama.cpp model loader and tokenizer** (GGUF parsing). This is
  upstream code running in the sandboxed inference service. Only the
  grammar engine is fuzzed here, through what the converter generates.
- **The browser's check of the document parser's PNGs**
  (`IsWellFormedPng()` in `document_parser/document_parser_client.cc`).
  It reads what a possibly compromised service sends, but it shares its
  file with Mojo and content code, so it has no standalone build. It is
  small, walks `base::span`s (bounds-checked) and has unit tests. It is a
  candidate for a GN-only target, as are the OCR and Whisper helpers'
  output parsers.
- **Grammar agreement for agent actions.** The fuzzers check that every
  action schema converts and that `ParseAction()` keeps its caps. They don't
  yet walk each action grammar in llama.cpp and check that `ParseAction()`
  accepts every reply the grammar allows.
- **Code that landed alongside this work.** These files have no targets
  yet: the web agent's prompt (`agents/agent_prompt.*`; only its fences and
  rules, with and without a plan, through `molt_agent_plan_fuzzer`), the
  model download plan (`models/model_download_plan.*`) and the chat's form-fill
  proposals (`ui/webui/molt_ai/molt_ai_chat_form_fill.*`). The screenshot
  logic (`agents/agent_vision.*`) has `molt_agent_vision_fuzzer`. The form fill reads model
  replies with `ParseAction()` and fences page text with the chat's
  decision prompt, and both of those are fuzzed. Its own formatting of
  pasted text and profile data is not.
- **The `stand_ins` tier in CI.** It needs OpenSSL and stand-ins that only
  approximate `//base`, so CI smoke-tests only the tiers that need neither.
  Run `molt_fuzz.py smoke --tier stand_ins` locally after changing that
  code.

## Adding a fuzzer

1. Write `<area>/fuzzers/molt_<thing>_fuzzer.cc` (or next to a Blink file).
   Check the code's promises with `CHECK`s, not just "no crash". Use
   `FuzzedDataProvider` when the input has structure. Keep it
   deterministic: no clock, randomness, network or files.
2. Add a `fuzzer_test()` to that directory's `BUILD.gn`, with a dictionary
   if tokens matter, and reach it from `molt_fuzzers`.
3. Add it to `FUZZERS` in `tools/fuzz/molt_fuzz.py`, with the same
   `max_len` and dictionary, and seeds in `corpus/molt_<thing>_fuzzer/`
   (unit-test vectors are a good start).
4. `tools/fuzz/molt_fuzz.py check && tools/fuzz/molt_fuzz.py run --seconds 600 molt_<thing>_fuzzer`.
