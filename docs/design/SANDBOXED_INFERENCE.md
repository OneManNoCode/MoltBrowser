# S5 phase 2: on-device inference in a sandboxed utility process

Status: design only, not implemented. Audit item S5 ("Rule of 2"): `llama_model_load_from_file`, tokenization and inference run in the unsandboxed browser process, so a GGUF or tokenizer bug becomes full browser compromise. Phase 1 (document parsing in a utility process) is a separate change.

Pin: Chromium 152.0.7977.169 (`92a6f151`). Upstream files below were read at that revision with `upshow new`.

## 0. Decisions in one page

1. **New service.** Add a Chrome-layer service, `molt_ai.mojom.MoltInferenceService`, in `src/chrome/services/molt_ai/inference/`.
   - It is launched with `content::ServiceProcessHost::Launch` and `[ServiceSandbox=sandbox.mojom.Sandbox.kOnDeviceModelExecution, RequireContext=sandbox.mojom.Context.kBrowser]`.
   - Every `llama_*`/`ggml_*` call moves there.
   - GN `visibility` on `//third_party/llama_cpp` makes it impossible for browser code to include `llama.h` again.
2. **Sandbox type.** Use `kOnDeviceModelExecution` on macOS, Linux and Windows.
   - It is the only type that allows Metal on macOS. On macOS `kService`/`kUtility` use `utility.sb`, which has no IOKit or `MTLCompilerService` rules.
   - On Linux and Windows it is the only type whose memory limit scales with RAM (8, 16, 32 or 64 GB). The others are capped at 4 GB (`sandbox/constants.h` `kDataSizeLimit`).
   - CPU weight repacking on AVX2 alone exceeds 4 GB for an 8B model.
   - Costs: the GPU-level seccomp policy and broker on Linux, and LPAC on Windows, which needs install-directory ACLs. See §2.4.
3. **Files cross the boundary as handles, never paths.**
   - The browser opens the GGUF read-only and sends `mojo_base.mojom.ReadOnlyFile`. Its traits `CHECK` on the sending side that the handle is not writable.
   - The service converts it to a `FILE*` and calls `llama_model_load_from_file_ptr()` (`include/llama.h:532`, already in b11641). mmap still works.
4. **One required llama.cpp patch.** llama.cpp passes `MAP_POPULATE` on every Linux mmap load (`src/llama-mmap.cpp:493`, called via `init_mappings(true)` at `src/llama-model.cpp:1874`). Chrome's seccomp answers that flag with `CrashSIGSYS` (`sandbox/linux/seccomp-bpf-helpers/syscall_parameters_restrictions.cc:245-256`). llama.cpp already calls `posix_madvise(WILLNEED)` on the same range, which seccomp allows, so the patch only drops the flag.
5. **Most of the runtime moves into the service, and the browser validates what comes back.**
   - Moving: the chat template, context budget, special-token neutralization and the reasoning filters (Harmony, `GemmaThoughtFilter`, `<think>`).
   - Checked in the browser: UTF-8, sizes, counts, protocol order and liveness of everything coming back.
6. **Lifetime.** There is one process per loaded model. Dropping the remote kills the process, so idle unload, memory-pressure unload and model swap all become "kill the process".
7. **No caller changes in this phase.** `BrowserAIRuntime` keeps its public API, including the blocking calls made from ThreadPool workers. So `MoltAIChatHandler`, `WebAgent`, `AutomationRunner` and `automation_background_browser` are untouched. Async variants and migrating callers to them is a later, optional step.
8. **Upstream files patched:** `chrome/utility/services.cc` and `chrome/utility/BUILD.gn`, imported unchanged first and then patched. No sandbox policy files are expected to change.

```
browser process (unsandboxed)                     utility process (kOnDeviceModelExecution)
-----------------------------                     ------------------------------------------
MoltAIChatHandler / WebAgent / AutomationRunner
   | (blocking calls on ThreadPool workers, as today)
BrowserAIRuntime  (registry, RAM check, request ids, idle/pressure, facade)
   |
InferenceProcessHost (UI thread)  --ReadOnlyFile(GGUF)-->  MoltInferenceService.LoadModel
   |  StreamChat/StreamPrompt(ChatRequest, params) ----->  Model (queue) -> LlamaEngine (worker seq)
   |  <---- GenerationStreamer.OnChunk / OnProgress /        load, template, budget, neutralize,
   |         OnComplete (validated: UTF-8, caps, order)      tokenize, decode, filters, stop seqs
   |  close streamer pipe = cancel
   |  drop remotes = kill process (idle, pressure, swap)
```

## 1. Starting point

- `src/chrome/browser/molt_ai/runtime/browser_ai_runtime.{h,cc}` (1,787 lines) contains all llama code. It is a `NoDestructor` singleton guarded by `generation_mutex` and `load_mutex`. Its parts:
  - **LoadModel:** `llama_model_load_from_file(info.file_path)` at `:1098`, with `n_gpu_layers=-1` and default params, so `use_extra_bufts=true` and mmap is on.
  - **Context sizing:** `ComputeContextSizing`.
  - **Special texts:** `CollectSpecialTexts`, which also sets the `thought_channels` flag that decides whether the Gemma filter runs.
  - **StreamChat** (`:1544-1750`):
    - reasoning control (Qwen3 `/no_think`, gpt-oss "Reasoning:");
    - `NeutralizeSpecialTokens` on `context`, `user_message` and `history`;
    - the `PlanPromptBudget` loop;
    - `ApplyChatTemplate` (`llama_chat_apply_template`);
    - tokenization with `parse_special=true`.
  - **StreamPrompt** (`:1340-1457`): neutralizes both the system prompt and the body, and tokenizes with `parse_special=false`.
  - **GenerateLocked** (`:603-796`):
    - KV-prefix reuse via `conversation_key`;
    - chunked prompt decode;
    - sampling;
    - the filter pipeline Harmony → Gemma → Think → `Utf8Assembler`;
    - hidden-token budget and forced answer (`kForceClose` / `kForceFinal` injected into the decode).
  - **Cancellation:** per-request ids (`NewRequestId`, `CancelRequest`, `IsRequestCancelled`, `FinishRequest`).
  - **Unloading:** idle unload via an epoch timer, and unload on critical memory pressure.
- **Std-only helpers** (unit-tested standalone): `context_budget.*`, `prompt_utils.*` (`NeutralizeSpecialTokens`, `LooksLikeSpecialTokenText`) and `stream_filters.*`.
- **Callers:** all call from ThreadPool `MayBlock` tasks, and all are blocking.
  - `molt_ai_chat_handler.cc`: `LoadModel` at `:1110` and `:1400`, `StreamChat` at `:1456`, plus `RefreshModelStatus`, `GetAvailableModels`, `GetHardwareCapability`, `DeleteModel` and the request-id calls.
  - `agents/web_agent.cc:239`: `RunPrompt`.
  - `automation/automation_runner.cc:1224`: `RunPrompt`.
  - `automation_background_browser.cc`: passes the singleton through.
  - Nothing outside the runtime calls `Tokenize`, and `ApplyChatTemplate` is only called internally.
- **GGUF parsing:** nothing else in the browser parses a GGUF. `ModelDownloadJob` only builds URLs and hashes the download.
- **Build wiring:** `//chrome/browser/molt_ai:molt_ai_runtime` depends on `//third_party/llama_cpp`. On macOS, `default.metallib` is installed into `MoltBrowser Framework.framework/.../Resources/` by `scripts/release.sh` and `scripts/dev-install-mac.sh`. The `.metal` sources are bundled by `//third_party/llama_cpp:llama_cpp_metal_resources`, a `bundle_deps` of `chrome/BUILD.gn:1276`.

## 2. Sandbox type and what it gives llama.cpp

### 2.1 How upstream Chrome runs its own model

- **Interface:** `services/on_device_model/public/mojom/on_device_model_service.mojom` declares `[ServiceSandbox=kOnDeviceModelExecution, RequireContext=kBrowser] interface OnDeviceModelService`. `sandbox.mojom` describes this type as "equivalent to the GPU process's sandbox, but can be used by service processes to host trustworthy models that may process untrustworthy inputs".
- **Launch:** `chrome/browser/optimization_guide/model_execution/optimization_guide_global_state.cc:48` calls `ServiceProcessHost::Launch<OnDeviceModelService>(receiver, Options().WithDisplayName(...))`.
- **Weights:** passed as `mojo_base.mojom.File` in a `ModelFile` union. `public/cpp/model_assets.cc` opens them with `FLAG_OPEN|FLAG_READ|FLAG_ASYNC|FLAG_WIN_SEQUENTIAL_SCAN` and prefetches with `base::PreReadFile`.
- **Disk caches:** the process can't write to disk, so shader caches come as writable file handles (`program_cache`).
- **GPU access:** direct, via Dawn inside the sandbox, not through Chrome's GPU process.
- **Pre-sandbox setup:**
  - `content/utility/on_device_model/on_device_model_sandbox_init.cc` warms Dawn drivers (`PreSandboxInit`). On Linux it also starts the GPU broker, adds `/sys/devices/system/cpu/online`, and dlopens the GPU and Vulkan libraries (`PreSandboxHook`).
  - In `content/utility/utility_main.cc`, `PreSandboxInit` is keyed on the service name (`:292`, `OnDeviceModelService::Name_`). The Linux hook is keyed on the sandbox type (`:335`).
  - So our service gets the Linux hook but not the Dawn warm-up, which is what we want.
- **Cancellation and idle:** closing the `StreamingResponder` pipe cancels a generation (`on_device_model.mojom`). `ServiceClient` resets the remote on idle, which kills the process.

### 2.2 Recommendation

Use `kOnDeviceModelExecution` everywhere:

- It is the only sandbox type with the macOS GPU rules (`on_device_model_execution.sb`: `IOAccelerator` and the AGPM/IOSurface user clients, `com.apple.MTLCompilerService`, the Metal shader cache).
- On Linux and Windows its memory limit scales with RAM (`sandbox_linux.cc:476` for `RLIMIT_DATA`, `sandbox_win.cc:1145` for the job memory limit).
- On Linux its pre-sandbox hook already loads `libvulkan.so.1` and the common ICDs, and the broker allows `/usr/share/vulkan/icd.d` and `/dev/dri` (`gpu_pre_sandbox_hook_linux.cc`). That is exactly the Vulkan plan.
- It needs no sandbox policy patches, which keeps rebase cost down.

### 2.3 Per-platform: what the sandbox blocks and the workaround

**macOS.** Seatbelt is applied when the helper starts, so there is no pre-sandbox hook. Utilities run in the plain `MoltBrowser Helper.app` (`CHILD_NORMAL`, no `allow-jit` entitlement), and Metal doesn't need JIT. Parameters come from `SetupGpuSandboxParameters` (`sandbox_parameters_mac.mm:266`).

| Need | Allowed? | Work-around |
|---|---|---|
| Open `~/.moltbrowser/models/*.gguf` by path | No. User-directory access is behind `kMacSandboxOdmeUserDirAccess`, which is off. | The browser passes a `ReadOnlyFile`; the service calls `fdopen(fd, "rb")` and `llama_model_load_from_file_ptr`. |
| mmap that fd `PROT_READ`/`MAP_SHARED`; Metal `newBufferWithBytesNoCopy` over the mapping | Yes (only `PROT_EXEC` would be checked) | — |
| `MTLCreateSystemDefaultDevice`, IOKit GPU user clients, pipeline compiles through `MTLCompilerService` | Yes | — |
| `default.metallib` via `[NSBundle bundleForClass:]` → `pathForResource:` (`ggml-metal-device.m:504-517`) | Yes. `common.sb:149` allows `file-read*` under `bundle-path`, the outer `.app`, which contains the framework's `Resources/`. | None needed. If a dev build lacks the metallib, ggml falls back to compiling `Resources/kernels/*.metal`, which is also inside the bundle. Fallback only if verification shows a denial: pass the metallib as a `ReadOnlyFile` plus a small ggml patch to use `newLibraryWithData:`. `GGML_METAL_PATH_RESOURCES` pointing outside the bundle won't work, which is fine. |
| Metal shader cache | Yes (`maybe-allow-metal-shader-cache-access`; `helper-bundle-id` is set for this type) | — |
| `sysctlbyname("machdep.cpu.brand_string")` (CPU device name, `ggml-cpu.cpp:294`) | No | Harmless: the call fails with EPERM and logs a sandbox denial. Optionally don't call it. |
| Network, writes outside caches | No | Not needed. |
| Memory | No RLIMIT; bounded by `recommendedMaxWorkingSetSize` | — |

**Linux.** This type forks from the unsandboxed zygote (`utility_sandbox_delegate.cc:146`). Threads are allowed during sandbox init (`sandbox_linux.cc:97`). The seccomp policy is the GPU one with `mremap` allowed (`sandbox_seccomp_bpf_linux.cc:189`), and a GPU broker mediates file access.

| Need | Allowed? | Work-around |
|---|---|---|
| Open the GGUF by path | No (broker) | `ReadOnlyFile` + `fdopen`. Call `TakePlatformFile()` before `fdopen` so no `ScopedFD` still owns the fd (fd-ownership enforcement). |
| `mmap(..., MAP_SHARED \| MAP_POPULATE)` (`llama-mmap.cpp:493`) | **No: `CrashSIGSYS`** | Local patch: don't add `MAP_POPULATE`. The existing `posix_madvise(POSIX_MADV_WILLNEED)` at `:516` still prefetches. |
| `posix_fadvise`, `madvise(WILLNEED/RANDOM)` | Yes (`baseline_policy.cc:243`) | — |
| `fopen("/proc/cpuinfo")` (CPU device name) | No (broker) | Harmless: falls back to "CPU". |
| `/proc/self/status`, `prctl(PR_SET_PTRACER)` (ggml backtrace) | Would be denied or crash | Already compiled out by `GGML_MOLTBROWSER_NO_BACKTRACE`. Keep it so. |
| `arch_prctl(ARCH_REQ_XCOMP_PERM)` (AMX, `amx.cpp:216`) | Risky | Compiled out: it only exists under `__AMX_INT8__ && __AVX512VNNI__`, and BUILD.gn doesn't set those. Keep it that way, or call it pre-sandbox if AMX is ever enabled. |
| `sched_setaffinity`, `pthread_setschedparam`, `getcpu`/NUMA | Not reached | ggml only calls these with a cpumask, a non-normal priority, or `ggml_numa_init`. Never set them. |
| `std::thread::hardware_concurrency` | Yes (broker allows `/sys/devices/system/cpu/online`) | The browser passes `n_threads` anyway. |
| `RLIMIT_DATA` | 8 GB (≤16 GB RAM) / 16 / 32 / 64 GB | Shared read-only file mappings don't count. CPU repack buffers, the KV cache and compute buffers do. See §2.5. |
| Vulkan (later) | Hook dlopens `libvulkan.so.1` + ICDs; broker allows ICD JSON and `/dev/dri/renderD*`; NVIDIA policy chosen from GPU info | Mostly ready. ggml-vulkan's build-time shader generation is the bigger job. |

**Windows.** This type uses LPAC with `lpacChromeInstallFiles`, `registryRead`, `lpacPnpNotifications` and a `chromeInstallFiles` impersonation capability (`sandbox_win.cc:439`). The token is `USER_RESTRICTED_SAME_ACCESS`/`USER_LIMITED` (`utility_sandbox_delegate_win.cc:157`), and the job memory limit scales with RAM.

| Need | Allowed? | Work-around |
|---|---|---|
| Start the process at all | **Only if the install directory grants read+execute to the LPAC capability SIDs.** Chrome's installer does this (`ConfigureAppContainerSandbox`, `install_worker.cc:967`). Our NSIS installer (`installer/windows/moltbrowser.nsi`) and the 7-Zip portable ZIP do not. | Add an ACL step to NSIS, granting the same SIDs Chrome's setup derives. The portable ZIP has no reliable fix, so a fallback is needed (§2.4). Verify on Windows. |
| Open the GGUF by path | No (LPAC has no user-profile access) | `ReadOnlyFile`, opened **without `FLAG_ASYNC`**. Upstream uses `FLAG_ASYNC`, but an overlapped handle breaks llama.cpp's synchronous CRT reads. Convert with `_open_osfhandle(h, _O_RDONLY\|_O_BINARY)` + `_fdopen(fd, "rb")`. A read-only handle has no `FILE_EXECUTE`, so it can't be mapped executable. |
| `CreateFileMapping`/`MapViewOfFile` on that handle (`llama-mmap.cpp:594`); `PrefetchVirtualMemory` | Yes | — |
| Job memory limit | 8/16/32/64 GB of committed memory | File-backed read-only views aren't charged; repack, KV and compute buffers are (§2.5). |
| Vulkan (later) | Unknown: needs `vulkan-1.dll` and ICD discovery (registry, DriverStore DLLs) | Preload with `ServiceProcessHost::Options::WithPreloadedLibraries` (PassKey-gated, so a small content patch), plus a pre-lockdown warm-up. Alternative: ggml-webgpu over Chromium's Dawn (D3D12), which is what Chrome's own model service uses in this sandbox. Open question. |

### 2.4 Alternative for Linux/Windows CPU: `kService`/`kUtility`

- **Pros:**
  - Linux: a smaller seccomp policy, no GPU broker, no GPU ioctls, and no GPU libraries loaded before the sandbox.
  - Windows: a `USER_LOCKDOWN` token without LPAC, so no ACL dependency.
- **Cons:**
  - A 4 GB data/job limit. With repacking, an 8B Q4_K_M model alone needs about 4.3 GB of anonymous memory, so `use_extra_bufts=false` would be mandatory. That costs roughly 1.3–2× slower prompt processing on AVX2 (to be measured) and a capped context.
  - No path to Vulkan.
  - A second set of behaviours to test.
- **Suggestion:** `kOnDeviceModelExecution` everywhere. If Windows can't ship the LPAC ACLs (portable ZIP), use `kService` with repacking off on Windows only. That means a second mojom interface name, because the sandbox type is a compile-time attribute of the interface. Owner decision (§7).

### 2.5 Memory-mapping a multi-GB GGUF

- **The mapping itself:**
  - The mapping is `MAP_SHARED`/`PROT_READ` (on Windows, a read-only file view), so on Linux and Windows it doesn't count against `RLIMIT_DATA` or the job limit.
  - Lazy tensors (`LLAMA_LAZY_MODE_AUTO`, tensors over 4 GiB) read through the `FILE*` after load. The `llama_file(FILE*)` constructor doesn't own the stream (`llama-mmap.cpp:229`), so **keep the `FILE*` open until `llama_model_free`**.
- **What does count against the limits** (anonymous memory):
  - **CPU repack:** on AVX2, `use_extra_bufts=true` (the default, `llama-model.cpp:3345`) repacks Q4_0, Q4_K (`repack.cpp:5006`), IQ4_NL, MXFP4 and Q8_0 weights into anonymous `CPU_REPACK` buffers.
  - **KV cache:** for example, Qwen3-8B uses about 147 KB per token at f16, so 2.4 GB at 16K context.
  - **Compute buffers.**
- **Service-side budget:**
  - The service reads its own limit: `getrlimit(RLIMIT_DATA)` on Linux, `QueryInformationJobObject` on Windows; there is none on macOS.
  - If repacked weights + the minimum KV cache + 1 GB would exceed about 85% of the limit, load with `use_extra_bufts=false`.
  - Shrink `n_ctx` so the KV cache fits.
  - Report both choices in `ModelDescription`.
- **Browser-side checks:** keep `CheckRamForModel` before launching, so the user still gets the specific "needs N GB" errors.
- **Splits:** `load_from_file_ptr` doesn't support split GGUFs. Every catalog entry is a single file; keep it so.
- **Upstream prefetch:** upstream's `base::PreReadFile` warms the page cache in the browser; it's optional for us.

### 2.6 Why not plug llama.cpp into upstream's `on_device_model` service

`services/on_device_model/backend.h` has a pluggable `Backend`, but reusing the service doesn't fit:

- Its session API (Append `InputPiece` / `Token` enum / Generate) doesn't fit our budgeted `ChatPrompt` with trimming.
- Selecting our backend means patching `on_device_model_service.cc::DefaultImpl()`.
- It is registered only when `optimization_guide::features::CanLaunchOnDeviceModelService()`, so we would carry optimization-guide feature plumbing.
- The mojom is `[Stable]` and full of fields we don't use.
- It is a fast-moving upstream area, so rebases would keep breaking the patches.

Instead we reuse the sandbox type and upstream's patterns: a read-only file handle, closing the pipe to cancel, and dropping the remote to kill the process.

## 3. Mojo interface

File: `src/chrome/services/molt_ai/inference/public/mojom/molt_inference.mojom`, target `.../public/mojom:mojom` (deps `//mojo/public/mojom/base`, `//sandbox/policy/mojom`).

```mojom
module molt_ai.mojom;

import "mojo/public/mojom/base/big_string.mojom";
import "mojo/public/mojom/base/file.mojom";
import "sandbox/policy/mojom/context.mojom";
import "sandbox/policy/mojom/sandbox.mojom";

struct ModelLoadParams {
  mojo_base.mojom.ReadOnlyFile weights;  // opened by the browser; never a path
  uint64 file_size;                      // as the browser saw it (sanity check)
  uint64 total_ram_bytes;                // for ComputeContextSizing
  uint32 cpu_threads;                    // n_threads / n_threads_batch
  uint32 max_context_tokens;             // cap; service picks min(sizing, cap)
  bool allow_cpu_repack = true;          // may be overridden by the memory budget
};

enum LoadModelStatus {
  kOk, kUnsupportedCpu, kInvalidModelFile, kOutOfMemory, kBackendUnavailable,
};

struct ModelDescription {
  string architecture;        // GGUF general.architecture, <= 64 bytes
  string backend;             // "metal" | "cpu" | "vulkan"
  uint32 context_tokens;      // n_ctx allocated
  uint32 train_context_tokens;
  uint32 vocab_size;
  bool cpu_repack_used;
};

[ServiceSandbox=sandbox.mojom.Sandbox.kOnDeviceModelExecution,
 RequireContext=sandbox.mojom.Context.kBrowser]
interface MoltInferenceService {
  // At most once per process (a second call is a bad message). The model
  // lives while `model` is connected; the browser drops both pipes to unload,
  // which ends the process.
  LoadModel(ModelLoadParams params, pending_receiver<Model> model)
      => (LoadModelStatus status, ModelDescription? description);
};

enum ReasoningMode { kOff, kOn };
enum ChatRole { kUser, kAssistant };

struct GenerationParams {
  uint32 max_visible_tokens = 0;     // 0 = adaptive (AdaptiveReplyTokens)
  uint32 max_reasoning_tokens = 0;   // 0 = adaptive
  ReasoningMode reasoning = kOff;
  float temperature = 0.7;
  float top_p = 0.9;
  uint32 top_k = 40;
  string conversation_key;           // KV-prefix reuse; "" = none; <= 128 bytes
  array<string> stop_sequences;      // visible-text stops; <= 8, each 1..64 bytes
};

struct ChatTurn { ChatRole role; mojo_base.mojom.BigString content; };

// Mirrors molt_ai::ChatPrompt. The trust labels are the field names:
// system_prompt and context_preamble are browser-authored; context, history
// and user_message are untrusted and are neutralized before
// parse_special=true tokenization.
struct ChatRequest {
  string system_prompt;
  string context_preamble;
  mojo_base.mojom.BigString context;
  array<ChatTurn> history;           // oldest first; <= 512 turns
  mojo_base.mojom.BigString user_message;
};

struct RawPromptRequest {            // agents / automation (StreamPrompt)
  string system_prompt;              // neutralized too, as today
  mojo_base.mojom.BigString prompt;
};

enum GenerationStatus {
  kEndOfGeneration, kMaxTokens, kStopSequence, kContextFull, kCancelled,
  kPromptTooLong, kSystemPromptTooLong, kTokenizeFailed, kDecodeFailed,
};

struct GenerationSummary {
  GenerationStatus status;
  uint32 prompt_tokens;
  uint32 reused_tokens;
  uint32 generated_tokens;           // visible + hidden
  uint32 history_dropped;
  bool context_trimmed;
  bool user_message_trimmed;
};

// Implemented by the browser. Closing it cancels the request (queued or running).
interface GenerationStreamer {
  // During prompt evaluation, at most every 500 ms (liveness + UI).
  OnPromptProgress(uint32 done_tokens, uint32 total_tokens);
  // Visible text since the last chunk: complete UTF-8, reasoning and channel
  // markers removed, stop sequences applied, never empty.
  OnChunk(string text);
  // Exactly once, last. The service drops the pipe afterwards.
  OnComplete(GenerationSummary summary);
};

interface Model {
  // Served one at a time, in arrival order; a request whose streamer is closed
  // when its turn comes is skipped.
  StreamChat(ChatRequest request, GenerationParams params,
             pending_remote<GenerationStreamer> streamer);
  StreamPrompt(RawPromptRequest request, GenerationParams params,
               pending_remote<GenerationStreamer> streamer);
  // Plain-text tokenization with this vocab (no BOS, parse_special=false).
  // Null on failure or if text exceeds the cap. For tests, a future context
  // meter and memory chunking; no current browser caller needs token ids.
  Tokenize(mojo_base.mojom.BigString text) => (array<int32>? tokens);
  CountTokens(mojo_base.mojom.BigString text) => (uint32? count);
  // Drops a cached KV prefix (conversation deleted or cleared).
  ForgetConversation(string conversation_key);
};
```

### 3.1 Loading

**Browser, on a `MayBlock` worker:**

1. Open the file with `base::File(path, FLAG_OPEN | FLAG_READ)`. No `FLAG_ASYNC`; add `FLAG_WIN_SHARE_DELETE` so `DeleteModel` isn't blocked by an exiting process.
2. Check that it is a regular file (`GetInfo`).
3. Check `GetLength()` against the registry's `file_size_bytes` when pinned.
4. Launch the service on the UI thread and call `LoadModel` with a deadline (10 min; a 12 GB cold load is about 1 min).

**Service, on a worker sequence:**

1. Run the CPU check (`ComputeUnsupportedReason`) again, as a second line of defence.
2. `llama_backend_init()` once.
3. Convert the handle to a `FILE*` (§2.3).
4. Set `model_params.n_gpu_layers=-1`.
5. Apply the memory budget (§2.5).
6. `llama_model_load_from_file_ptr`, then `llama_init_from_model`, keeping the current retry at `kMinContextTokens`.
7. `CollectSpecialTexts` and `UsesGemmaThoughtChannel`.
8. Reply with the description.

### 3.2 Streaming, cancellation, stop sequences

- **Cancel.** The browser closes the `GenerationStreamer` receiver. The service-side remote's disconnect handler, on the service's main sequence, sets the request's `std::atomic<bool> cancelled`. `GenerateLocked` already polls a cancel check per token and per prompt chunk (`IsCancelled`), so it switches to the flag. Queued requests whose pipe is closed are skipped. A Stop pressed before the request is even sent is still remembered by the browser's request-id map, as today.
- **Generation thread.** Generation must not run on the thread that receives mojo messages, or cancellation would only be seen between requests. So `LlamaEngine` lives on `base::SequenceBound` on a ThreadPool sequence (`MayBlock`, `USER_BLOCKING`, `CONTINUE_ON_SHUTDOWN`, so a decode in progress can't delay process exit). Chunks are posted back to the main sequence, which owns the streamer remotes.
- **Stop sequences** (new; the runtime only stops on end-of-generation tokens today):
  - Add a std-only `StopSequenceFilter` to `stream_filters` after `ThinkTagFilter` and before `Utf8Assembler`.
  - It holds back up to `max_len-1` bytes until it can tell whether they start a stop string.
  - The matched stop text is not emitted; status is `kStopSequence`.
  - The browser runs the same filter over what it receives. That copy is authoritative for callers (cheap), and a match there also cancels the request.
- **Liveness.** `OnPromptProgress` lets the browser tell a long CPU prompt evaluation from a hang: a 32K-token prompt on CPU can take minutes.

### 3.3 Where neutralization and the Gemma thought filter run, and why the service

Both run **in the service**. The browser does not re-run the reasoning filters; it validates structure (§5.3).

1. **Rule of 2.** Both functions parse attacker-influenced bytes.
   - `NeutralizeSpecialTokens` scans page, PDF and memory text.
   - The filters scan model output that page text can steer by prompt injection.
   - The bytes come from untrusted input, the code is C++, and in the browser it has no sandbox. In the service, a bug in either is contained.
2. **Integrity doesn't depend on location.**
   - Neutralization protects the *prompt structure* (no fake turns or tool results) against an uncompromised model process.
   - If the service is compromised, its output is attacker-chosen regardless, and neither a browser-side neutralization nor a browser-side filter changes that.
   - So the browser's protections must be on what it *does* with output: WebUI escaping, the `agent_safety` gates and action parsing. Those are independent of the filters.
3. **Correctness requires adjacency.**
   - Neutralization must use the exact special-text list of the vocab that will tokenize the text. In the browser, that list (derived from an untrusted GGUF, potentially 250k entries) would have to be shipped over and processed there, which is more browser attack surface for no gain.
   - The reasoning filters are coupled to the decode loop. `hiding()` decides hidden versus visible budgets and triggers injecting `kForceClose`/`kForceFinal` tokens into the KV cache (`browser_ai_runtime.cc:750-782`). Doing that in the browser would need a per-token round trip.
4. **Trust labels stay structural.** `ChatRequest` keeps the `ChatPrompt` fields separate, so the service knows what to neutralize. The browser must never pre-concatenate untrusted text into `system_prompt`.

### 3.4 Tokenize

Kept small and read-only:

- plain text, no special-token parsing;
- results capped;
- ids range-checked by the browser;
- never used for a security decision.

The context budget, which today is the only consumer of token counts, moves into the service with the rest of prompt building.

## 4. Process lifetime and the `BrowserAIRuntime` API

### 4.1 One process per model

**Launch.** A new `InferenceProcessHost` (browser, UI thread) calls:

```
ServiceProcessHost::Launch<molt_ai::mojom::MoltInferenceService>(
    receiver,
    Options().WithDisplayName("On-Device AI Model")
             .WithObserver(weak)
             .WithProcessCallback(cb)
             .Pass())
```

- The observer delivers `OnServiceProcessCrashed`.
- The process callback gives a `base::Process`, used for a terminate-after-grace watchdog and footprint sampling.
- Avoid "Google" in the display name.

**Load or swap,** keeping today's ordering logic (`browser_ai_runtime.cc:1080-1092`):

1. If free RAM is less than the new model needs and a model process exists, drop it first. Wait for the termination notification, with a 5 s grace, then `Terminate`.
2. Otherwise launch and load the new process while the old one keeps serving.
3. On success, drop the old remotes; on failure, drop the new process.
4. `loaded_model_id` changes only on success.

**Unload.** `UnloadModel` and `DeleteModel` drop the remotes. `DeleteModel` waits for process termination before `std::filesystem::remove`; this matters on Windows.

**Warm-keeping:** the existing epoch idle timer (default 10 min, `settings.model_idle_unload_minutes`).

**Optional:** launch the empty process when the chat UI initializes, to hide 100–300 ms of launch latency.

### 4.2 Idle and memory pressure

- **Idle** (`MaybeUnloadIdle`): when no generation or load is in flight, drop the remotes and the process exits. This frees all memory and Metal buffers at once.
- **Critical memory pressure:** the browser's existing listener does the same. Keep today's policy of not interrupting a generation.
  - Add a footprint watchdog: every 30 s while loaded, sample the child's private footprint. Kill the process if it exceeds the expected footprint (weights + KV + 2 GB) by 50%.
  - On macOS this is the only memory bound.
- **Side benefit on Linux:** the OOM killer now picks the inference process rather than the browser.

### 4.3 Crash recovery

- **Detection:**
  - A disconnect of the `Model` remote or of a streamer without `OnComplete`.
  - `OnServiceProcessCrashed`.
  - A missed deadline, after which the browser kills the process itself.
- **During generation:** the request fails with "The on-device model stopped unexpectedly (it may have run out of memory). Try again, or pick a smaller model." Text already streamed stays on screen, marked as interrupted.
- **State:** `loaded_model_id` is cleared. The chat handler's existing logic (`must_load` when `GetLoadedModelId() != wanted`) lazily relaunches on the next send. `WebAgent` and `AutomationRunner` behave as after an idle unload today ("No model loaded").
- **Crash-loop guard, per model id + file size + mtime:**
  - Two crashes during **load** within 1 h: refuse auto-load ("This model file crashed the on-device AI process while loading. Delete and re-download it.").
  - Crashes during **generation**: back off relaunches (1 s, 5 s, 30 s). After 3 in 10 min, stop auto-reloading until the user picks the model again. Page text can deterministically crash a tokenizer, and automation loops must not hammer.
- **Crash dumps:** an inference-process dump contains prompts (page text, chats). Make sure such dumps are never uploaded, or are stack-only.

### 4.4 Mapping onto the existing API (callers unchanged)

| `BrowserAIRuntime` method | New behaviour |
|---|---|
| `GetInstance`, `Initialize` | Unchanged, but no `llama_backend_init` in the browser. The AVX2/FMA check stays (`base::CPU`) and runs before any launch. |
| `GetLocalInferenceUnsupportedReason`, `GetHardwareCapability`, `CheckRamForModel`, `CanRunModel`, `GetModelMemoryUsage`, `RefreshModelStatus`, `GetAvailableModels`, `GetModelInfo`, `DownloadModel` | Unchanged (registry stays in the browser). |
| `LoadModel(id, &error)` | Blocking facade: opens the file, launches, loads, swaps; maps `LoadModelStatus` to today's user-facing strings. |
| `UnloadModel`, `DeleteModel`, `Shutdown` | Drop remotes (kill); `DeleteModel` waits for exit before removing the file. |
| `StreamChat(ChatPrompt, cb, opts)`, legacy `StreamChat(vector)`, `StreamPrompt`, `RunPrompt` | Blocking facade over `Model.StreamChat`/`StreamPrompt`. `TokenCallback` is invoked **on the calling worker thread**, exactly as now, with `is_done=true` once at the end. `GenerationResult` is filled from validated chunks and a clamped summary. |
| `ApplyChatTemplate` (public, internal-only use) | Removed from the header (moves into the service). |
| `NewRequestId`, `CancelRequest`, `IsRequestCancelled`, `FinishRequest`, `CancelGeneration` | Unchanged semantics. `CancelRequest` additionally posts to the UI thread to close that request's streamer. |
| `GetLoadedModelId` | Returns a mutex-guarded string; no longer blocks behind a decode. |
| `StartMemoryPressureMonitoring`, `SetIdleUnloadTimeout` | Unchanged API; unloading means killing the process. |

**Blocking facade.** The mojo remotes live on the UI thread, because `Launch` requires it. A blocking call works like this:

- It posts the request to the UI thread.
- It waits on a per-request `std::condition_variable` in 250 ms slices inside `base::ScopedBlockingCall(WILL_BLOCK)`.
- Each wake drains queued chunks and invokes `callback` on its own thread.
- It returns on `OnComplete`, disconnect, deadline, or app termination (`//chrome/browser/lifetime:termination_notification` `AddAppTerminatingCallback`, which wakes all waiters).

That last case matters because callers post with the default `SKIP_ON_SHUTDOWN`, and a running task blocks shutdown. The facade also `DCHECK`s that it is not on the UI thread.

The existing file already blocks workers on `std::mutex` for minutes, so std primitives are consistent with it. They also avoid the `AssertBaseSyncPrimitivesAllowed` checks without a new `thread_restrictions.h` friend.

**Later (2c):** add async variants (`StreamChatAsync(..., on_chunk, on_done)` on the UI thread), migrate the three generation call sites, and delete the facade.

### 4.5 Concurrency

Today `generation_mutex` serializes generations. Now the service's single `llama_context` serves requests FIFO. Contention between chat, agent and automation is unchanged; `AutomationRunner` already queues its own requests.

## 5. Threat model

### 5.1 What the sandbox removes from the browser process

Bugs in any of these become a compromise of a sandboxed process instead of the browser:

- the GGUF parser and model loader (`gguf.cpp`, `llama-model-loader.cpp`);
- tokenizers and pre-tokenizers (`llama-vocab.cpp`, `unicode.cpp`), including the **`std::regex`/`std::wregex` fallback on input text** (`unicode.cpp:890-1626`). That fallback is super-linear and recursive in libc++, so stack overflow or a hang on hostile page text is possible.
- ggml kernels fed malformed tensor shapes;
- Metal backend bugs;
- exceptions escaping `llama_*` calls;
- the stream filters and neutralization.

A hostile or corrupt model, including the "unverified" unpinned downloads, no longer leads directly to browser code execution.

### 5.2 What a compromised inference process can still do

1. **Return arbitrary text.**
   - Chat answers, `[[ACTION]]` blocks, and `NAVIGATE`/`CLICK`/`TYPE` steps for `WebAgent` and automation.
   - This equals a perfect prompt injection (S6). The boundary is the `agent_safety`/`automation_security` gates: http(s)-only, origin scope, confirmations, credential-field detection.
   - Residual risk: Auto mode skips confirmation, so a compromised service can act inside approved origins in the user's logged-in profile.
2. **Read everything it is sent and mix conversations.**
   - It sees page text, PDFs, chat history, memory hits, and form-fill profile data if a local model is used for form fill.
   - It holds the KV cache of all conversations, so it can copy chat content into an agent's output, for example a navigation URL query string.
   - Direct exfiltration is blocked: no network on macOS (`.sb` has no network rules), on Windows (LPAC has no `internetClient`), or on Linux (GPU seccomp; verify `socket(AF_INET)` is denied).
   - Indirect exfiltration through outputs remains. Mitigations are S6 (confirm cross-origin navigations, no data-bearing URLs to unapproved origins) and never sending secrets (§5.4).
3. **Deny service:** hang, loop, flood, emit huge output, crash on purpose, burn CPU/GPU/battery, and allocate up to its memory limit (unbounded on macOS).
4. **Lie about metadata:** token counts, `n_ctx`, architecture, statuses.
5. **Attack the browser over mojo:**
   - Malformed structs are rejected by mojo validation.
   - Logic and state bugs in our receiver are the remaining surface: chunks after completion, out-of-range numbers, oversized strings.
   - Mojo `string` is **not** UTF-8-validated in C++.
6. **Attack the GPU driver or kernel.** GPU ioctls on Linux, IOKit user clients on macOS and the driver stack on Windows are reachable, the same as Chrome's GPU process and its model service. This is the price of `kOnDeviceModelExecution` over `kService` on CPU-only platforms.
7. **Persist within its own sandbox.**
   - It can't rewrite the model: the handle is read-only and the mapping is `PROT_READ`.
   - It can't write user files: the Linux broker allows only GPU paths, and on macOS user-directory access is off.
   - On macOS it **can** write the Metal shader cache for `MoltBrowser Helper`, which future inference processes reuse. Upstream accepts the same exposure.

### 5.3 Browser-side validation (all in a std-only `StreamValidator`, unit-tested standalone)

- **UTF-8.** Every `OnChunk` must be valid UTF-8; the service's `Utf8Assembler` guarantees complete characters. Invalid UTF-8 means `ReportBadMessage`, kill the process, and fail the request. Replace NUL with U+FFFD. Never `CHECK` on service data, same rule as for renderer input.
- **Length.**
  - Per chunk: at most 4 KiB. A token piece is at most a few hundred bytes; filter flushes are small.
  - Total visible text: at most `min(2 MiB, (max_visible + max_hidden) × 512 B)`.
  - Exceeding either means cancel and kill.
- **Count and rate.**
  - Chunks: at most `max_visible + max_hidden + 8`.
  - Progress messages: at most 4/s.
  - Any message type: at most 2,000/s.
  - The browser coalesces chunks into at most about 30 UI updates/s before `FireWebUIListener`, so a fast or hostile service can't flood the UI thread or the WebUI.
- **Protocol.** `OnComplete` exactly once and nothing after it. A missing `OnComplete` before disconnect is a failure.
- **Liveness.** No message for 120 s, or past the hard deadline (load 10 min, generation 15 min), means kill.
- **Numbers.**
  - Summary counts clamped to `[0, n_ctx]`. `AutomationRunner` adds them into an `int` (`current_run_tokens_`), so unclamped values could overflow.
  - `ModelDescription` is a load failure if `architecture` is more than 64 bytes or not `[a-z0-9_.-]`, or if `context_tokens` is outside `[256, max_context_tokens]`.
  - Tokenize: size at most `text bytes + 16`, ids in `[0, vocab_size)`.
- **Inputs the browser sends.**
  - Cap `conversation_key` (128 bytes) and stop sequences.
  - Cap history (512 turns) and context size (`BigString` already handles multi-MB page text).
- **Stop sequences.** Re-applied in the browser (§3.2).

### 5.4 Things to stop sending

- The process must never receive secret values: vault entries, API keys, or decrypted recorded secrets. Recorded workflows already use vault references; keep it that way for LLM steps.
- Form fill should send only the fields the user asked for (relates to S19).
- No automatic local-to-cloud fallback on local failure (privacy). None exists today; keep it so.

## 6. Implementation plan

Size key: S < 100 lines, M 100–400, L > 400. Dependencies: steps 1–3 can run in parallel; 4 needs 2 and 3; 5 needs 4; 6 needs 3 and 5; 7–9 come last.

1. **llama.cpp sandbox patch** (S, about 40 lines, ½ day).
   - `src/third_party/llama_cpp/src/llama-mmap.cpp`: under a new define, `#if !defined(LLAMA_MOLTBROWSER_SANDBOXED)`, around `flags |= MAP_POPULATE`.
   - `src/third_party/llama_cpp/BUILD.gn`:
     - add `config("llama_sandboxed")` and apply it to `:llama_cpp`;
     - set `visibility = [ "//chrome/services/molt_ai/inference/*", ":*" ]` on `:llama_cpp`, `:ggml` and `:llama_common`.
   - `src/third_party/llama_cpp/README.chromium`: record the patch, and change the description "in the browser process" to "in a sandboxed utility process".
   - Can be checked standalone: build `llama-mmap.cpp` plus a small harness that installs a seccomp filter trapping `MAP_POPULATE` and loads a small GGUF through `load_from_file_ptr`.
2. **Move the std-only helpers** (S, mechanical; plus M for `StopSequenceFilter`).
   - `git mv` `context_budget.*`, `prompt_utils.*`, `stream_filters.*` and their `*_unittest.cc` from `src/chrome/browser/molt_ai/runtime/` to `src/chrome/services/molt_ai/inference/engine/`. `chrome/services` must not depend on `chrome/browser`.
   - Update the include guards and paths and the new `engine/BUILD.gn` (`inference_utils`, `unit_tests`).
   - Delete or trim `runtime/BUILD.gn` and update the label in `src/chrome/browser/molt_ai/test/BUILD.gn`.
   - Add `StopSequenceFilter` with tests (about 120 + 120 lines).
   - Standalone-verifiable, as today.
3. **Mojom** (M, about 150 lines).
   - `src/chrome/services/molt_ai/inference/public/mojom/molt_inference.mojom` and `.../public/mojom/BUILD.gn`.
4. **Service** (L, about 1,300 lines, of which about 800 are moved from `browser_ai_runtime.cc`; 2–3 days).
   - `src/chrome/services/molt_ai/inference/llama_engine.{h,cc}`:
     - load from `FILE*`, plus the memory-budget logic;
     - `BuildChatTokens` (moved from the `StreamChat` body) and `BuildRawTokens` (moved from the `StreamPrompt` body);
     - `Generate` (moved `GenerateLocked`, plus the stop filter and progress);
     - `Tokenize`, `Detokenize`, `TokenToPiece`, `ApplyChatTemplate`, `CollectSpecialTexts`, `ReadModelArch`;
     - a `llama_log_set` callback that logs at `VLOG(1)` and never logs prompt text.
   - `src/chrome/services/molt_ai/inference/molt_model_impl.{h,cc}`: the `mojom::Model` implementation. FIFO queue, per-request cancel flags, input caps, posting chunks to the main sequence.
   - `src/chrome/services/molt_ai/inference/molt_inference_service.{h,cc}`: the `mojom::MoltInferenceService` implementation.
     - Calls `llama_backend_init` once.
     - Accepts one `LoadModel` per process; a second call is a bad message.
     - Converts the handle to a `FILE*`: POSIX `fdopen`; Windows `_open_osfhandle` + `_fdopen`.
     - Keeps the `FILE*` open until `llama_model_free`.
   - `src/chrome/services/molt_ai/inference/BUILD.gn`.
   - `src/chrome/services/molt_ai/inference/DEPS`: `+third_party/llama_cpp/include`.
5. **Register the service in the utility process** (S; upstream files).
   - Commit A: import `src/chrome/utility/services.cc` and `src/chrome/utility/BUILD.gn` from the pin **unchanged**, then run `tools/chromium/overlay_to_patches.py` and `tools/chromium/verify_patches.py`.
   - Commit B: `services.cc` gets `#include "chrome/services/molt_ai/inference/molt_inference_service.h"`, a `RunMoltInferenceService` factory, and `services.Add(RunMoltInferenceService)` in `RegisterMainThreadServices`, guarded by `#if !BUILDFLAG(IS_ANDROID)`. `BUILD.gn` gets the `//chrome/services/molt_ai/inference` dep. Regenerate and verify the patches.
   - **Coordinate with phase 1:** it edits the same two files. Whoever lands first does the import. Agree on `chrome/services/molt_ai/<service>/` as the layout.
6. **Browser client** (L, about 900 new or changed lines; 2–3 days).
   - `src/chrome/browser/molt_ai/runtime/inference_process_host.{h,cc}` (UI thread):
     - launch, load, swap, observer, process handle and grace-kill;
     - the `GenerationStreamer` implementation, with validation, stop re-application and coalescing;
     - deadlines and liveness, idle and pressure kills, footprint watchdog, crash-loop guard.
   - `src/chrome/browser/molt_ai/runtime/stream_validator.{h,cc}` and its unittest (std-only).
   - `src/chrome/browser/molt_ai/runtime/browser_ai_runtime.{h,cc}`:
     - delete about 900 lines of llama code and `ApplyChatTemplate`;
     - add the facade (§4.4);
     - `Initialize` no longer calls llama.
   - `src/chrome/browser/molt_ai/BUILD.gn` (`molt_ai_runtime`): drop `//third_party/llama_cpp` and `inference_utils`; add the mojom, `//content/public/browser`, `//chrome/browser/lifetime:termination_notification` and `//mojo/public/cpp/bindings`.
   - No caller changes: `molt_ai_chat_handler.cc`, `web_agent.cc`, `automation_runner.cc` and `automation_background_browser.cc` are untouched.
7. **Platform specifics** (S).
   - macOS: nothing expected. The metallib stays in Framework `Resources/`, the plain Helper is used, and no new entitlements are needed. Only if the sandbox log shows a real denial: import `sandbox/policy/mac/on_device_model_execution.sb` unchanged, then patch it.
   - Windows: add the LPAC ACL step to `installer/windows/moltbrowser.nsi`, or apply the `kService` fallback (§2.4). Windows builds are paused, so this can trail.
   - Linux: nothing.
8. **Tests** (M).
   - Standalone: `StreamValidator`, `StopSequenceFilter`, and the moved engine utils.
   - Build machine: an `InferenceProcessHost` test against a fake in-process `Model` in `molt_ai_unittests`, with content test support. It should cover crash mid-stream, hang, flood, bad UTF-8, chunk after complete, and cancel while queued.
   - Manual QA checklist below.
9. **Docs** (S).
   - `ARCHITECTURE.md`: the runtime is now a sandboxed utility.
   - `CLAUDE.md`: llama.cpp is linked only by `chrome/services/molt_ai/inference`. The "no exception may escape `llama_*`" rule now means "kills the AI process" rather than the browser; still enforced.
   - `README.chromium` and the audit's S5 status.

Total is roughly 2,500 changed lines, about half of them moves: 5–7 days of development plus 1–2 days of verification per platform.

**Only verifiable on a build machine:**

- **Everything that compiles against Chromium:** mojom generation, GN visibility and `gn check`, the service, registration, the facade.
- **macOS:**
  - Metal device init in the sandbox, with `default.metallib` loaded from Framework `Resources/` through `bundleForClass`.
  - No sandbox denials beyond `machdep.cpu.brand_string`. Use `--enable-sandbox-logging` and `log stream --predicate 'sender=="Sandbox"'`.
  - First-load latency, which now includes Metal pipeline creation per process.
  - Codesigned and notarized build: the helper launches and Metal works under the hardened runtime.
- **Linux:**
  - No SIGSYS: `MAP_POPULATE` patched, plus `fadvise`/`madvise`, `clone3` fallback and thread creation.
  - `RLIMIT_DATA` with repack on 8B and 20B models.
  - `/proc/cpuinfo` denial harmless.
  - `socket(AF_INET)` denied.
  - The process is listed on `chrome://sandbox`.
- **Windows:**
  - The LPAC process starts from an NSIS install and from the ZIP.
  - `_open_osfhandle`/`_fdopen` and `CreateFileMapping` on the passed handle.
  - Job limit with repack.
- **All platforms:**
  - Tokens/s and first-token latency before and after.
  - Kill the utility mid-stream (`kill -9` / Task Manager) and check recovery.
  - Idle unload kills the process.
  - Memory-pressure unload works.
  - `DeleteModel` while loaded works.
  - The Task Manager label shows.

## 7. Risks and open questions for the owner

1. **Sandbox type on Linux/Windows CPU:** `kOnDeviceModelExecution` (RAM-scaled limits, Vulkan-ready, GPU-level attack surface) versus `kService` (tighter, but a 4 GB cap: repack off, smaller context). Recommendation: the former.
2. **Windows LPAC ACLs:** add them to NSIS, and decide what the portable ZIP does. Options: a `kService` variant, or local AI disabled on the ZIP. Do not fall back to in-process.
3. **No in-process fallback.** A kill switch that brings llama back into the browser re-opens S5. Proposal: no fallback; on failure show an error that suggests a cloud model. Dev debugging uses `--utility-startup-dialog` / `--no-sandbox`.
4. **Repack versus memory limits:** measure `use_extra_bufts` on and off (speed and RSS) for TinyLlama, Qwen3-8B and gpt-oss-20b on 16 GB and 32 GB machines.
5. **Latency:** process launch plus Metal pipeline creation on every reload after idle unload. Consider pre-warming the process when the chat opens.
6. **One process serves chat, agent and automation**, so a compromise can leak across them. Separate processes would double the RAM for multi-GB models; accept for now.
7. **GPU driver exposure and the macOS Metal shader cache** are reachable from the sandbox. This is the same as upstream; accept.
8. **Windows Vulkan:** ggml-vulkan with preload/warm-up versus ggml-webgpu over Chromium's Dawn (better sandbox fit, unknown performance). Also, ggml-vulkan's build-time SPIR-V generation needs a GN action.
9. **Crash dumps contain prompts:** confirm the crash-upload configuration excludes or strips the inference process.
10. **Phase-1 coordination:** a shared import of `chrome/utility/services.cc`/`BUILD.gn` and the `chrome/services/molt_ai/` layout.
11. **Callers converted to async (2c)?** It would remove the facade and its shutdown handling. Small but touches three call sites.
12. **Should `WebAgent`/automation auto-load the model after an idle unload or crash** like the chat does? Today they fail with "No model loaded".
13. **Stop-sequence semantics** (stop text excluded; applied after the reasoning filters) and whether any current caller wants stop sequences now.
14. **macOS `sysctl` denial noise:** accept it, or skip that `sysctlbyname` with a one-line ggml patch.
15. **Model directory** (aside; it now matters because the browser opens the file): the path comes from `getenv("HOME")`, which is usually unset on Windows, giving a `/tmp/moltbrowser/models` fallback. Out of scope, but it should use `base::PathService` / `base::GetHomeDir()`.

### Critical files for implementation

- `src/chrome/browser/molt_ai/runtime/browser_ai_runtime.cc`
- `src/chrome/browser/molt_ai/runtime/browser_ai_runtime.h`
- `src/third_party/llama_cpp/src/llama-mmap.cpp`
- `src/third_party/llama_cpp/BUILD.gn`
- `src/chrome/browser/molt_ai/runtime/stream_filters.h`
- `chrome/utility/services.cc` (upstream, to be imported to `src/chrome/utility/services.cc`)