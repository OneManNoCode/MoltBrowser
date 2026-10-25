# S5 phase 2: on-device inference in a sandboxed utility process

Status: implemented (October 2026), steps 1-4 and 6-9 of §6; pending a compile on the build machine and the per-platform device checks (§6, "Only verifiable on a build machine"). Step 5, registering the service in `chrome/utility/services.cc` and `chrome/utility/BUILD.gn`, is done together with phase 1, which patches the same two files. The owner's decisions are recorded in §0 and §7; how the implementation differs from this design, and why, is in §8. Audit item S5 ("Rule of 2"): `llama_model_load_from_file`, tokenization and inference ran in the unsandboxed browser process, so a GGUF or tokenizer bug became full browser compromise. Phase 1 (document parsing in a utility process) is a separate change.

Pin: Chromium 152.0.7977.169 (`92a6f151`). Upstream files below were read at that revision with `upshow new`.

## 0. Decisions in one page

1. **New service.** Add a Chrome-layer service, `molt_ai.mojom.MoltInferenceService`, in `src/chrome/services/molt_ai/inference/`.
   - It is launched with `content::ServiceProcessHost::Launch` and `[ServiceSandbox=sandbox.mojom.Sandbox.kOnDeviceModelExecution, RequireContext=sandbox.mojom.Context.kBrowser]`.
   - Every `llama_*`/`ggml_*` call moves there.
   - GN `visibility` on `//third_party/llama_cpp` makes it impossible for browser code to include `llama.h` again.
2. **Sandbox type.** Use `kOnDeviceModelExecution` on macOS, Linux and Windows. **Decided by the owner.**
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
7. **No caller changes in this phase** (none were needed; `PromptOptions` gained an optional `stop_sequences` field, §8). `BrowserAIRuntime` keeps its public API, including the blocking calls made from ThreadPool workers. So `MoltAIChatHandler`, `WebAgent`, `AutomationRunner` and `automation_background_browser` are untouched. Async variants and migrating callers to them is a later, optional step.
8. **Upstream files patched:** `chrome/utility/services.cc` and `chrome/utility/BUILD.gn`, imported unchanged first and then patched. No sandbox policy files are expected to change.
9. **Images (§10).** A vision-language model's projector is a second read-only handle, and the service receives raw RGB pixels within fixed bounds, never an image file: decoding stays in Chromium's own sandboxed decoders.
10. **Audio (§13).** A projector may also have an audio encoder; the service receives 16 kHz mono 16-bit PCM within fixed bounds, never an audio file: pages' audio is decoded in their renderer.

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
| Vulkan | Hook dlopens `libvulkan.so.1` + the RADV, ANV and NVIDIA drivers. On desktop Linux the broker allows `/dev/dri/card*` and the NVIDIA nodes but not `/dev/dri/renderD*`, `/dev/dri` itself or sysfs (those are ChromeOS-only), and only five fixed ICD manifest names | NVIDIA's driver can probably start; Mesa can't. See §11.4. |

**Windows.** This type uses LPAC with `lpacChromeInstallFiles`, `registryRead`, `lpacPnpNotifications` and a `chromeInstallFiles` impersonation capability (`sandbox_win.cc:439`). The token is `USER_RESTRICTED_SAME_ACCESS`/`USER_LIMITED` (`utility_sandbox_delegate_win.cc:157`), and the job memory limit scales with RAM.

| Need | Allowed? | Work-around |
|---|---|---|
| Start the process at all | **Only if the install directory grants read+execute to the LPAC capability SIDs.** Chrome's installer does this (`ConfigureAppContainerSandbox`, `install_worker.cc:967`). The 7-Zip portable ZIP does not. | **Done (step 7):** the NSIS installer runs `icacls "$INSTDIR" /grant` with exactly the two SIDs Chrome's setup grants (`chrome/installer/setup/configure_app_container_sandbox.cc` at 152: `chromeInstallFiles` `S-1-15-3-1024-3424233489-…-1043257646` and `lpacChromeInstallFiles` `S-1-15-3-1024-2302894289-…-3317428798`), `(OI)(CI)(RX)` = `FILE_GENERIC_READ \| FILE_GENERIC_EXECUTE` inherited by files and folders, before extracting (§2.4a). Verify on Windows. |
| Open the GGUF by path | No (LPAC has no user-profile access) | `ReadOnlyFile`, opened **without `FLAG_ASYNC`**. Upstream uses `FLAG_ASYNC`, but an overlapped handle breaks llama.cpp's synchronous CRT reads. Convert with `_open_osfhandle(h, _O_RDONLY\|_O_BINARY)` + `_fdopen(fd, "rb")`. A read-only handle has no `FILE_EXECUTE`, so it can't be mapped executable. |
| `CreateFileMapping`/`MapViewOfFile` on that handle (`llama-mmap.cpp:594`); `PrefetchVirtualMemory` | Yes | — |
| Job memory limit | 8/16/32/64 GB of committed memory | File-backed read-only views aren't charged; repack, KV and compute buffers are (§2.5). |
| Vulkan | Not after lockdown: `LowerToken()` applies `MITIGATION_FORCE_MS_SIGNED_BINS`, so `vulkan-1.dll` and vendor ICDs must be loaded before it; LPAC access to ICD registry keys and DriverStore unverified | Needs a pre-lockdown warm-up (§11.4). Alternative: ggml-webgpu over Chromium's Dawn (D3D12), which is what Chrome's own model service uses in this sandbox. |

### 2.4 Alternative for Linux/Windows CPU: `kService`/`kUtility`

- **Pros:**
  - Linux: a smaller seccomp policy, no GPU broker, no GPU ioctls, and no GPU libraries loaded before the sandbox.
  - Windows: a `USER_LOCKDOWN` token without LPAC, so no ACL dependency.
- **Cons:**
  - A 4 GB data/job limit. With repacking, an 8B Q4_K_M model alone needs about 4.3 GB of anonymous memory, so `use_extra_bufts=false` would be mandatory. That costs roughly 1.3–2× slower prompt processing on AVX2 (to be measured) and a capped context.
  - No path to Vulkan.
  - A second set of behaviours to test.
- **Suggestion:** `kOnDeviceModelExecution` everywhere. If Windows can't ship the LPAC ACLs (portable ZIP), use `kService` with repacking off on Windows only. That means a second mojom interface name, because the sandbox type is a compile-time attribute of the interface. Owner decision (§7).
- **Decided:** `kOnDeviceModelExecution` on macOS, Linux and Windows; no `kService` variant for now. The portable ZIP can't run on-device models (§2.4a).

### 2.4a Windows: installer ACLs and the portable ZIP

- `installer/windows/moltbrowser.nsi` grants the LPAC capability SIDs on `$INSTDIR` right after creating it and before `File /r`, as Chrome's setup does for its target and temp directories (`AddInstallWorkItems`, `install_worker.cc:967`). The in-app updater runs the same installer, and files written into the directory later inherit the ACEs. The step is best effort, like Chrome's (`set_best_effort(true)`): if `icacls` fails the install continues and local models can't start.
- **Portable ZIP: on-device AI needs the installer.** A directory unpacked from a ZIP has whatever ACLs its parent gives it, which normally don't include the LPAC capability SIDs, so the sandboxed process can't load MoltBrowser's own DLLs and dies before IPC. The browser reports that as a launch failure, not a model crash (so the crash-loop guard doesn't blame the model file), with "The portable ZIP can't run on-device models: install MoltBrowser with the installer, or use a cloud model." Cloud models work from the ZIP. A user can also grant the two SIDs on the unpacked folder by hand with the same `icacls` command.
- A later option for the ZIP is a `kService` variant of the interface (USER_LOCKDOWN token, no LPAC, so no ACL dependency; 4 GB job limit, so repacking off and a smaller context; §2.4). Not built.

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
  // Liveness while nothing visible is sent, at most every 500 ms: prompt
  // evaluation, then hidden reasoning (implemented as OnProgress, §8).
  OnProgress(uint32 done_tokens, uint32 total_tokens);
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
| `GetInstance`, `Initialize` | Unchanged, but no `llama_backend_init` in the browser. The CPU check stays (`base::CPU`) and runs before any launch; since §12 it asks for x86-64-v2 (SSE4.2, POPCNT), not AVX2. |
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
   - Chat answers, and `NAVIGATE`/`CLICK`/`TYPE` steps for `WebAgent`, the chat's proposed page actions and automation (docs/design/AGENT_ACTIONS.md).
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

1. **Sandbox type on Linux/Windows CPU:** `kOnDeviceModelExecution` (RAM-scaled limits, Vulkan-ready, GPU-level attack surface) versus `kService` (tighter, but a 4 GB cap: repack off, smaller context). Recommendation: the former. **Decided:** `kOnDeviceModelExecution` on macOS, Linux and Windows.
2. **Windows LPAC ACLs:** add them to NSIS, and decide what the portable ZIP does. Options: a `kService` variant, or local AI disabled on the ZIP. Do not fall back to in-process. **Decided:** NSIS grants the SIDs (§2.4a); the portable ZIP needs the installer for on-device AI (or a later `kService` variant).
3. **No in-process fallback.** A kill switch that brings llama back into the browser re-opens S5. Proposal: no fallback; on failure show an error that suggests a cloud model. Dev debugging uses `--utility-startup-dialog` / `--no-sandbox`. **Decided:** no fallback; the browser no longer links llama.cpp at all.
4. **Repack versus memory limits:** measure `use_extra_bufts` on and off (speed and RSS) for TinyLlama, Qwen3-8B and gpt-oss-20b on 16 GB and 32 GB machines.
5. **Latency:** process launch plus Metal pipeline creation on every reload after idle unload. Consider pre-warming the process when the chat opens.
6. **One process serves chat, agent and automation**, so a compromise can leak across them. Separate processes would double the RAM for multi-GB models; accept for now. **Accepted.**
7. **GPU driver exposure and the macOS Metal shader cache** are reachable from the sandbox. This is the same as upstream; accept.
8. **Windows Vulkan:** ggml-vulkan with preload/warm-up versus ggml-webgpu over Chromium's Dawn (better sandbox fit, unknown performance). Also, ggml-vulkan's build-time SPIR-V generation needs a GN action.
9. **Crash dumps contain prompts:** confirm the crash-upload configuration excludes or strips the inference process.
10. **Phase-1 coordination:** a shared import of `chrome/utility/services.cc`/`BUILD.gn` and the `chrome/services/molt_ai/` layout.
11. **Callers converted to async (2c)?** It would remove the facade and its shutdown handling. Small but touches three call sites.
12. **Should `WebAgent`/automation auto-load the model after an idle unload or crash** like the chat does? Today they fail with "No model loaded".
13. **Stop-sequence semantics** (stop text excluded; applied after the reasoning filters) and whether any current caller wants stop sequences now. **Decided:** as designed. No current caller sets any; `PromptOptions::stop_sequences` makes them available.
14. **macOS `sysctl` denial noise:** accept it, or skip that `sysctlbyname` with a one-line ggml patch.
15. **Model directory** (aside; it now matters because the browser opens the file): the path comes from `getenv("HOME")`, which is usually unset on Windows, giving a `/tmp/moltbrowser/models` fallback. Out of scope, but it should use `base::PathService` / `base::GetHomeDir()`.

### Critical files for implementation (as designed)

- `src/chrome/browser/molt_ai/runtime/browser_ai_runtime.cc`
- `src/chrome/browser/molt_ai/runtime/browser_ai_runtime.h`
- `src/third_party/llama_cpp/src/llama-mmap.cpp`
- `src/third_party/llama_cpp/BUILD.gn`
- `src/chrome/browser/molt_ai/runtime/stream_filters.h`
- `chrome/utility/services.cc` (upstream, to be imported to `src/chrome/utility/services.cc`)
## 8. Implementation notes and deviations

Where the code differs from the sections above, and why.

- **Progress is also liveness during reasoning.** `GenerationStreamer.OnPromptProgress` became `OnProgress`. It is also sent while the model generates hidden reasoning (reasoning tokens / budget), at most every 500 ms and only when nothing else was sent. Hidden reasoning sends no text, and a long reasoning budget on a slow CPU can take longer than the 2-minute liveness timeout. The browser accepts progress at any time, still at most 4 a second.
- **Stop filter position.** The service applies `StopSequenceFilter` after `Utf8Assembler` and `ToDisplayText`, not before `Utf8Assembler`. `ToDisplayText` turns invalid UTF-8 and NUL into U+FFFD. This way the service and the browser run the filter on exactly the same text, so a stop string can't match in one copy and not the other because of replaced bytes. Stop strings are valid UTF-8, so the filter still never splits a character. The browser rejects invalid UTF-8 from the service outright, so the service must never send any.
- **Engine layout.** `LlamaEngine` (`llama_engine.{h,cc}`) uses only std and `llama.h`: no `//base`, no mojo, no logging. It is built and run standalone against the vendored llama.cpp objects. `MoltModelImpl` and `MoltInferenceService` are the mojo/base glue around it. `engine/BUILD.gn` has a separate `:stream_filters` target next to `:inference_utils` and `:unit_tests`. It is the only part the browser links, for `StopSequenceFilter` and the UTF-8 checks.
- **The mojom caps are constants** (`kMaxHistoryTurns`, `kMaxChunkBytes`, ...), shared by both sides. The browser caps what it sends:
  - history keeps the newest 512 turns;
  - a text field over 32 MiB keeps its head;
  - a conversation key over 128 bytes is replaced by a digest, which is harmless because KV reuse compares exact token prefixes;
  - invalid stop strings are dropped.

  The service checks the caps again and treats a violation as a bad message.
- **Chunk and byte caps without the adaptive budgets.** The browser doesn't know which adaptive budgets the service picks, so a budget of 0 (adaptive) counts as `n_ctx` tokens:
  - total bytes: `min(2 MiB, min(n_ctx, visible + reasoning + 16) * 512)`;
  - chunks: that token bound + total / 4096 + 16, because the service splits a run of text over 4 KiB into several chunks.
- **Memory budget before load.** The service decides on CPU repacking before it knows the model's hparams. Repacking stays on only if the file size + 1 GiB (minimum KV) + 1 GiB (compute) fits in 85% of the sandbox limit. After loading, it caps `n_ctx` so the KV cache fits in what is left. The browser's own cap on `n_ctx` is 32768, the top RAM tier.
- **Footprint watchdog limit.** The browser can't see the KV size, so it uses an upper bound:
  - limit: `1.5 * (weights + max(1 GiB, (RAM - weights - 4 GiB) / 2) + 2 GiB)`;
  - sampled every 30 s: physical footprint on macOS, anonymous RSS + swap on Linux, private bytes on Windows.
- **The crash-loop guard is time-based.** `BrowserAIRuntime::LoadModel` can't tell an explicit pick in the model menu from the chat's auto-load, because the callers are unchanged. So "until the user picks the model again" became "for the rest of the window":
  - Two load crashes within an hour refuse the file for that hour. A re-downloaded file is a new key (id + size + mtime).
  - Generation crashes back off relaunches by 1 s, 5 s, then 30 s. Three within ten minutes refuse the model until fewer than three remain in the window.
  - Deaths while idle (e.g. the OS reclaiming memory) aren't counted.
- **Launch failures aren't crashes.** A process that never starts, or dies before IPC, is reported as `kLaunchFailed` and doesn't count against the model file. On Windows that is the portable ZIP without the LPAC ACLs.
- **Shutdown.** `AddAppTerminatingCallback` wakes every blocked worker. As a backstop, a blocked worker pings the UI thread when it has heard nothing for 5 s, and gives up if the ping isn't answered within 15 s, so a shutdown without the app-terminating notification can't block ThreadPool shutdown on it. `CancelRequest` wakes the blocked worker immediately.
- **Swap and unload.** A model swap loads the new process while the old one keeps serving. On success the old process stops taking requests, finishes the ones it has, then ends. `UnloadModel` and `DeleteModel` do the same and wait until the process has exited. Idle and memory-pressure unloads check and forget the model in one step under the state lock: a generation starting at that moment either pins the model first or sees it gone.
- **Android and iOS.** The service isn't registered there: step 5 guards it with `!BUILDFLAG(IS_ANDROID)`, and `//chrome/services/molt_ai/inference` asserts it isn't built for Android or iOS. `BrowserAIRuntime` reports on-device AI as unavailable there and never launches anything. Before this change, the Android GN graph linked llama.cpp into the browser; it no longer does.
- **Tests.** Step 8's tests went in with the code they test:
  - step 2: the moved engine utils, `StopSequenceFilter` and the UTF-8 helpers;
  - step 6: `StreamValidator`, `CrashLoopGuard`, and `InferenceProcessHost` against a fake in-process service.

### 8.1 Verified without a Chromium checkout

- **Unit tests** for the engine utils, `StopSequenceFilter`, `StreamValidator` and `CrashLoopGuard`, with system gtest: clang with ASan + UBSan, g++ `-O2 -Werror`, and a libc++ `-fno-exceptions -fno-rtti` syntax pass.
- **`LlamaEngine` end to end**, against the vendored llama.cpp built with the GN flags (`LLAMA_MOLTBROWSER_SANDBOXED` included) and a tiny GGUF, also under ASan + UBSan + LSan:
  - loading from a `FILE*` made with `fdopen`;
  - chat and raw prompts;
  - KV-prefix reuse and `ForgetConversation`;
  - cancellation and stop sequences;
  - oversized prompts, a size mismatch and garbage files;
  - the memory budget and `Tokenize`.
- **The `MAP_POPULATE` patch.** In a child process with a seccomp-bpf filter that applies Chrome's `RestrictMmapFlags` rule (`SECCOMP_RET_TRAP` for any other flag), the patched build loads the model and generates. The same test linked with an unpatched `llama-mmap.o` dies with SIGSYS.
- **The mojom**, with upstream's parser and C++ generator at the pin and `//mojo/public/mojom/base`'s typemaps.
- **Syntax** of the service, `InferenceProcessHost`, `BrowserAIRuntime` and the host unittest: `clang++ -fsyntax-only` against stand-in headers that mimic the generated bindings and the `//base`, mojo and content APIs used.
- **The NSIS script**, built with makensis 3.09; the SIDs it grants were compared with the upstream source.

## 9. Constrained decoding (structured output)

Status: implemented (October 2026), pending a compile on the build machine. The service can force the visible answer to follow a JSON Schema, with llama.cpp's grammar sampler. Used by the web AI APIs' `responseConstraint` (docs/design/WEB_AI_APIS.md §5); open to every `BrowserAIRuntime` caller through `PromptOptions::json_schema`, and used by the web agent and Agent mode's AI steps (docs/design/AGENT_ACTIONS.md).

### 9.1 The request field and the flow

```mojom
const uint32 kMaxConstraintSchemaBytes = 32768;

union OutputConstraint {
  string json_schema;   // JSON Schema as JSON text; over the cap = bad message
};

struct GenerationParams {
  ...
  OutputConstraint? constraint;   // null = free text
};

enum GenerationStatus {
  ...
  kConstraintUnsupported,   // outside the subset / over a cap; nothing generated
  kConstraintFailed,        // llama.cpp refused the grammar, or no token continues it
};
```

A union, so a regex or a caller-written grammar can be added later without changing the field.

1. **Browser** (`BrowserAIRuntime`): checks the size and passes the text on. It never parses it.
2. **Service, on arrival** (`MoltModelImpl`, main sequence): `JsonSchemaToGrammar()` (`engine/json_schema_grammar.{h,cc}`, std-only) turns the schema into GBNF. A refused schema is answered at once with `OnComplete(kConstraintUnsupported)`. It never enters the queue, so it doesn't wait behind other requests.
3. **Service, when the request runs** (`LlamaEngine::Generate`):
   - The grammar sampler is created before the KV cache is touched. If llama.cpp refuses the grammar, the request ends with `kConstraintFailed` and nothing is decoded.
   - A per-model token table (`GrammarVocab`, `grammar_sampler.{h,cc}`) is built on first use. It reads every token once, about 150 ms for 150k tokens.
   - Each token is picked by `GrammarSampler` (§9.3) and accepted into the grammar only after it was checked.
4. **Browser, at the end**: the stream is validated as always (`StreamValidator`). On completion, `base::JSONReader` (RFC 8259, memory-safe Rust) checks that the text is complete JSON. If it isn't, the result fails with `invalid_output` (and `truncated` if the token budget ran out).

What a constraint changes in the engine:

- **Reasoning is off.** The grammar applies from the first generated token.
- **The prompt is pre-filled past any reasoning block,** so the model starts in its answer state. The prefill is our own text, tokenized with its special tokens; 16 prompt tokens are kept free for it.

  | Model or template | Prefill |
  |---|---|
  | gpt-oss | `<\|channel\|>final<\|message\|>`, after the template's `<\|start\|>assistant` |
  | Gemma 4 template that leaves a thought block open | `\n<channel\|>` |
  | Template that pre-opens `<think>` | `\n</think>\n\n` |
  | Qwen3 and Qwen3-MoE | `<think>\n\n</think>\n\n` (what Qwen3's own template writes with thinking off) |

- **The reasoning filters don't run** (Harmony, the Gemma thought filter, `<think>`). The visible text is exactly what the grammar matched. Stop sequences still apply.

### 9.2 Why the service converts the schema

The schema is page text, and turning it into a grammar is non-trivial processing of untrusted input in C++. By the Rule of 2 that belongs in the sandbox:

- A converter bug there is contained like a tokenizer bug.
- The grammar it produces feeds llama.cpp's GBNF parser in the same process, so whatever the caps miss is contained too.
- The browser has to check the result anyway, because a compromised service can return any text. It does so with `base::JSONReader`, which is memory-safe.

**Rejected alternative:** the browser parses the schema with `base::JSONReader`, walks it in C++ and sends GBNF. That puts a C++ walk of page input in the unsandboxed process, and the service would have to trust a grammar from the browser.

**The cost:** an unsupported schema is reported only once the request reaches the service. The web AI scheduler may hold it behind another page's generation first; the service itself answers it at once.

### 9.3 Threat model for grammar input

**Attackers:**

- A web page, which chooses the schema and the prompt (and so steers the model).
- A malicious or odd GGUF, which chooses the vocabulary.

**Goals:**

- Crash the inference process. A crash also feeds the crash-loop guard (§4.3): three generation crashes in ten minutes lock the model for the user.
- Hang it, or exhaust its memory.
- Make a page or a caller receive text that isn't JSON.
- Smuggle markup into the answer.

**1. Converter bounds** (`engine/json_schema_grammar.h`, `engine/bounded_json.h`):

| What | Cap |
|---|---|
| Input | 32 KiB |
| JSON values | 4096 |
| JSON nesting | 48 (the reader refuses deeper input before recursing) |
| Schema nesting | 12 |
| Properties per object | 64 |
| `anyOf` / `oneOf` alternatives | 16 |
| Enum values | 256, each at most 1 KiB |
| `maxLength` | 1024 |
| `minItems` / `maxItems` | 64 |
| Integer ranges | 1001 values |
| Output grammar | 128 KiB and 2048 rules |
| Sum of counted repetitions | 8192 (llama.cpp's parser expands each into rules and refuses more than 2000 in one) |

- **No recursion:** `$ref` is refused, so a schema can't express cycles.
- **Linear time:** enums and ranges are tries, and duplicate keys are found by sorting.
- **Live alternatives ≤ 64.** These are the alternatives the sampler can have alive at once: `anyOf` branches of the same kind, and an object's optional keys. Each costs a pass over the vocabulary when the fast path misses.
- **Grammar shape:**
  - ASCII only.
  - At most two levels of parentheses (llama.cpp's parser recurses per level).
  - No rule starts with itself (llama.cpp refuses left recursion).
  - Empty alternatives come first, because the parser reads past a line break after a trailing `|`.
- **Output is RFC 8259 JSON** that `base::JSONReader` accepts:
  - whitespace is bounded, so the model can't stall on blank lines;
  - numbers have at most 16 integer and 16 fraction digits and a 2-digit exponent;
  - `\u` escapes exclude surrogates.

**2. llama.cpp calls, audited at b11641** (listed at the top of `llama_engine.cc` and `grammar_sampler.cc`):

- **`llama_sampler_init_grammar`:** the parser catches its own `std::runtime_error` / `std::out_of_range` and returns `nullptr`. Its `GGML_ASSERT` (`n_prev_rules >= 1`) can't fail.
- **`llama_sampler_apply` on the grammar:** asserts a non-empty stack set, which only a bad accept could empty.
- **`llama_sampler_accept` on the grammar:** *throws* when a token leaves no stack, and *aborts* on an end-of-generation token before the value is complete. A candidate can pass `apply` yet do that on `accept` in two ways:
  - **A piece whose UTF-8 decodes to code point 0** (an overlong form in one token, or continuation bytes completing one). `apply` reads the 0 as the end of the piece; `accept` consumes it and throws. A negative-control test shows this against llama.cpp directly.
  - **A NUL byte.** The grammar stops reading the piece there while the text goes on.

  Both are unreachable:
  - `Utf8Prefix` allows only strictly valid UTF-8 across pieces.
  - Pieces with NUL are banned.
  - `GrammarSampler::Accept()` takes only the token `Sample()` just checked, with the same grammar call on the same code points, and never an end-of-generation token.

No local patch to llama.cpp is needed. The lazy and trigger-pattern grammars, which run `std::regex` on model output, are not used.

**3. Token bans under a grammar** (`engine/grammar_tokens.h`):

- **Control, unknown and unused tokens.** llama.cpp's grammar sees their markup (its piece cache renders with `special=true`) while the visible text is empty, so the two would disagree.
- **User-defined tokens that look like markup** (`<think>`, `<|channel>`).
- **Pieces with a NUL byte.**
- **Pieces over 256 bytes.** llama.cpp's candidate check recurses once per character of a piece.

**4. Time and memory:**

- Grammar setup (the parse, and the token table the first time) runs when the request starts, just before its first `OnProgress`. The browser's clocks for a running request already cover it: liveness (120 s without a message), the hard deadline (15 min) and the web AI 5-minute deadline. Cancellation is polled per token.
- Rejection-first sampling keeps the common case cheap. The chain picks from all logits as usual; only a pick the grammar refuses leads to a full pass over the filtered vocabulary.
- **Worst case measured:** about 45-50 ms per token (unconstrained: 2.4 ms), with
  - a 150k-token vocabulary;
  - schemas at the caps (64 live alternatives, the 1024-character string, the any-value grammar);
  - a random-weight model that misses the fast path on nearly every token;
  - an `-O1` build.
- **Memory per generation:** the stacks (bounded by the live-alternative cap and the nesting) and one candidate array.

**5. Output:**

- The text is what the grammar matched; strict UTF-8 makes `ToDisplayText` a no-op.
- The browser still parses it with `base::JSONReader`.
- A compromised service can return any valid JSON, or fail. It could always return arbitrary text, and §5.2 still applies.

**6. Logging:**

- Refusal reasons are fixed text. An unknown keyword's name is not repeated, since it is page text.
- llama.cpp prints a grammar it refuses to stderr, schema text included. That is unreachable with converter output, which the tests check always parses.

### 9.4 Not done

- **Regex constraints.** Blink sends only the pattern's source, without its flags (`/i`, `/u`), so it can't be applied faithfully. Refused, as before.
- **Reasoning before a constrained answer.** It would need a lazy grammar triggered after the reasoning block. Reasoning is skipped instead.
- **Streaming constrained text to pages.** The web AI session holds it back until it parses (WEB_AI_APIS.md §5).
- **Numeric bounds** beyond the forms listed in `json_schema_grammar.h`; `pattern`, `format` and the other refused keywords.

### 9.5 Tests

**In `molt_ai_unittests`** (std-only):

- `engine/json_schema_grammar_unittest.cc`:
  - every supported form, with accepted and refused texts per schema (through a small test-only GBNF recognizer);
  - every refusal class;
  - hostile schemas: nesting up to 10,000 deep, huge input, huge enums, wide objects and `anyOf`, nested `anyOf` past and exactly at the ambiguity cap, `$ref` cycles, the grammar-size and repetition budgets, and linear time.
- `engine/bounded_json_unittest.cc` and `engine/grammar_tokens_unittest.cc`.
- `inference_process_host_unittest.cc`: a request answered before its turn.

**Run outside the repo:**

- **End to end** against the vendored llama.cpp built with the GN flags, with tiny random-weight GGUFs (llama and qwen3 architectures), also under ASan and UBSan:
  - 195 constrained answers (16 schemas, three temperatures) all complete, parse, and validate against their schemas with Python's `jsonschema`;
  - raw prompts, refused grammars, cancellation, stop sequences, KV reuse and the Qwen3 prefill;
  - `GrammarSampler` with hand-made logits that favour every hostile token never picks one;
  - the llama.cpp negative control above.
- **Mutation checks:**
  - converter, reader and token rules: 74 of 75 mutations caught;
  - sampler and the engine's constrained path: 9 of 11 caught.

  The three survivors are equivalent (a redundant cap, pieces stored for banned tokens too, a filter flag that is already false under a grammar).
- **Syntax** of the service and browser code against stand-ins; **the mojom** with upstream's generator.

## 10. Image input (vision-language models)

Status: implemented (October 2026), pending a compile on the build machine. A vision-language model reads images through its vision projector (llama.cpp's "mmproj" GGUF), with llama.cpp's multimodal library (`tools/mtmd`) vendored at the same tag and built only into the sandboxed service. The registry's vision model is not offered until it and its projector are pinned (§10.4), so no user sees the feature yet. The first consumer is the Prompt API's image input (WEB_AI_APIS.md §5.1).

### 10.1 Decisions

1. **No image decoding in the service.** The service receives raw 8-bit RGB pixels with their width and height, never an image file.
   - The browser decodes with Chromium's own sandboxed decoders: the data decoder process for image files (`DecodeImageForModel()`, `runtime/model_image_prep.h`), Blink for a page's images (the Prompt API sends decoded `SkBitmap`s), the compositor for the web agent's screenshots (AGENT_ACTIONS.md §2.6).
   - It scales with Skia (`PrepareImageForModel()`), converts to RGB with transparent pixels over a neutral grey, and pads to the bounds below.
   - llama.cpp's helpers that decode files (`mtmd-helper`: stb_image, miniaudio, ffmpeg) are not vendored, so no compressed-image parser is linked into the service at all.
2. **Bounds** (`engine/image_input.h`, checked in the browser before sending and again in the service, which treats a violation as a bad message):

   | What | Bound |
   |---|---|
   | Side | 32 to 1024 pixels (web pages: scaled to at most 768) |
   | Aspect ratio | at most 8:1 |
   | Bytes | exactly width × height × 3 |
   | Images per request | 4, history included |
   | Where | user turns only (an image on an assistant turn is a bad message) |
   | Tokens per image | at most 1024 (`image_max_tokens`) |

   Images smaller or more extreme than the bounds are padded with grey, not refused; larger ones are scaled down, never up. The arithmetic is overflow-checked and tested with hostile sizes (zero, `UINT32_MAX`, 1×N, a buffer one byte short, row strides that wrap).
3. **The projector is a second read-only handle.** `ModelLoadParams.vision_projector` (`ReadOnlyFile?`) with its exact size; the browser sends one only for a registry model with a pinned projector. The service loads it from the `FILE*` (local patch `mtmd_init_from_file_ptr()` / `clip_init_from_file_ptr()`, README.chromium). clip copies the weights into memory; the RAM checks, the sandbox budget and the browser's footprint limit count the projector plus 512 MiB for the encoder. A projector the library refuses, or one made for another model (a different embedding width), is `kInvalidVisionProjector`. A service that reports `vision` without having been sent a projector is a protocol violation and is killed.
4. **The service places the images, not the text.** The message text gets one marker (`<__media__>`) per image in front of it, and `mtmd_tokenize_from_parts()` replaces the markers with the images in order; llama.cpp's own marker parsing isn't used. In a request with images the marker is neutralized like a special token in every untrusted field (context, history, the user's message), and the markers must pair up with the images one for one, or the request fails. So page text can't move, duplicate or add an image.
5. **No KV reuse across images.** A request with images neither reuses the cache of a previous turn nor leaves one to reuse (the cells hold image embeddings that the next request's tokens can't be compared with).
6. **Budget.** The new message's images count with the system prompt (never trimmed); a history turn's images count with that turn and are dropped with it. A request whose images don't fit the context fails instead of being cut.
7. **Status values.** `GenerationStatus.kImagesUnsupported` for images sent to a model loaded without a projector (answered at once, nothing queued); `ModelDescription.vision` says whether one is loaded; `LoadModelStatus.kInvalidVisionProjector`. The browser maps the first to `GenerationResult::images_unsupported` and fails such a request before it is sent when it already knows.
8. **Trust labels unchanged.** Images are untrusted content, like the page text they come with: they go with user turns, never with the system prompt or the preamble.

### 10.2 The flow

```
browser                                              inference service
-------                                              -----------------
page bitmap (Blink) / image file (data decoder)
  -> PrepareImageForModel (worker): Skia scale,
     RGB over grey, pad (image_input.h)
  -> ModelImage (checked: CheckModelImage)
ChatPrompt.user_images / ChatMessage.images
  -> ToServiceChat: <= 4 (new message's first,
     then the newest history turns'), each checked
  -> mojom.ImageInput {width, height, BigBuffer rgb} ->  MoltModelImpl: CheckImages (bad message
                                                         on any violation), kImagesUnsupported
                                                         without a projector, copies pixels out
                                                       LlamaEngine::Chat: count + budget with
                                                         placeholders, neutralize markers,
                                                         tokenize_from_parts, encode each image
                                                         chunk, decode embeddings (M-RoPE
                                                         positions where the model uses them)
```

### 10.3 Threat model

**Attackers:** a web page (it chooses the pixels, their size and the text around them, and through them steers the model); a malicious or odd projector GGUF (only from a pinned download, but still parsed in the sandbox).

**Goals:** run code in the browser through an image parser; crash, hang or exhaust the inference process; make the model see an image where the page shouldn't be able to put one; exhaust browser memory with images.

- **Parsers stay where they were.** Compressed images are parsed by Blink (the renderer, already the page's) or by the data decoder (its own sandboxed process); the browser handles only decoded bitmaps, with Skia's scaler, which Chromium runs on page images everywhere. Nothing new parses image files in the browser or the service.
- **The service trusts no pixel buffer.** `CheckImages()` checks count, role and every image's bounds and byte length before anything is queued; a buffer that can't be mapped fails the request. `EngineImage`s are copies; `mtmd_bitmap_init()` copies exactly width × height × 3 bytes after `CheckModelImage()` matched them.
- **Preprocessing is bounded.** At most 4 images of at most 1024 × 1024, at most 1024 tokens each; the encoder's buffers are sized at load (`warmup`), so an image can't fail for memory later. The per-request deadlines and liveness checks cover image encoding.
- **Exceptions.** mtmd is built with exceptions like the rest of llama.cpp. Every `mtmd_*` and `llama_batch_ext_*` call was audited at b11641; the list, with each call's throws and aborts, is at the top of `llama_engine.cc`. The entry points that can throw catch everything (`mtmd_init_from_file_ptr`, `mtmd_tokenize_from_parts`, `mtmd_encode_chunk`); the aborts need inputs the engine never makes (an index past an image's tokens, an unknown chunk type, a batch over `n_batch`, a non-causal image larger than one ubatch, which is checked before the call). Two upstream bugs found on the way are patched (README.chromium): a short read of the projector left weights uninitialized instead of failing, and a projector refused after loading leaked its clip contexts.
- **Environment.** With `LLAMA_MOLTBROWSER_SANDBOXED`, `MTMD_DEBUG_EMBEDDINGS` is ignored (no dump of image embeddings to the log or a file).
- **Logging.** mtmd's log goes through the service's filter (warnings and errors only).
- **Browser memory** (Prompt API, WEB_AI_APIS.md §5.1): bitmaps over 64 megapixels are refused unread; images are held per site (16) and in total (48), at most 1.7 MB each once scaled; a session's context keeps the pixels of its newest 4 images only.

### 10.4 The model and its pins

The registry gets **Qwen3-VL 2B Instruct** (Apache 2.0) in Q4_K_M, with its projector in Q8_0 (`kProjectors` in `browser_ai_runtime.cc`).

- **Why this one.** A permissive licence, a size that runs next to the browser on 8 GB machines, strong text reading in screenshots and documents for its size (the main use: pages and agent screenshots), and a projector type the vendored llama.cpp supports (Qwen3-VL, with M-RoPE). Rejected: Qwen2.5-VL 3B (research-only licence), Gemma 3 (Gemma terms need acceptance), LFM2-VL (its own licence), SmolVLM2 (weaker at reading text), Qwen2.5-VL 7B (too big to sit next to the chat's model on most machines).
- **Fail closed.** Its revision, both SHA-256s and both sizes are empty: until `tools/models/pin_models.py` fills all five from the same Hugging Face revision (it pins the projector from the model's own reply, and pins neither if the projector is missing), the model is not in the catalogue at all (`IsPinnedWithProjector()`). The repo and file names are marked to be confirmed when pinning.
- **Download (one unit).** The model and its projector are downloaded, verified, counted, sized and deleted together (`models/model_download_plan.*`, the chat handler's download flow in `molt_ai_chat_handler.cc`):
  - *Pins, fail closed.* Before any IO, `CheckUnit()` refuses a unit with a projector unless both files have a 40-hex revision, a well-formed SHA-256 and a size (`IsFullyPinned`), so even a registry mistake can't download an unpinned projector; a text-only model may still download unpinned, as "unverified", as before.
  - *Download.* `PlanModelDownload()` lists the files still to fetch, the weights first. Each gets its own `ModelDownloadJob` from `/resolve/<revision>/<file>` of the model's repo, with its own pinned SHA-256 and exact size: streamed into `<file>.partial`, hashed, and moved into place only once both match; a size or hash mismatch on either file discards that file's `.partial` and fails the download. Progress (`download-progress`) and the size the model pickers show cover the whole unit.
  - *Downloaded.* Only when every file is complete: present and, when pinned, of exactly its pinned size (`IsCompleteModelFile`, also what `RefreshModelStatus()` now checks). The weights without the projector, a `.partial`, or a file of the wrong size never count; the next download fetches only what is missing (a resumed `.partial` continues).
  - *Disk space.* The precheck needs room for every file still to fetch, less what its `.partial` holds.
  - *Cancel and delete.* Cancel stops the transfer and deletes the unit's files and `.partial` files on the job's file sequence, after any write or rename still queued there: a model is on disk whole or not at all. Delete removes both files (`DeleteModel`) and their `.partial` leftovers, and is refused while that model downloads. A failed download (network, hash) keeps what is in place for the next attempt, as before.

### 10.5 Not done

- **Audio**: §13. **Video**: not planned.
- **Images in the AI chat or in Agent mode's recorded workflows.** The runtime API takes them (`ChatPrompt::user_images`, `ChatMessage::images`); the Prompt API and the web agent's screenshots (AGENT_ACTIONS.md §2.6) are the only callers that send any.
- **Mapping the projector** instead of copying it: clip reads tensors into its own buffers.
- **KV reuse across images** (§10.1, 5).

### 10.6 Tests

**In `molt_ai_unittests`:**

- `engine/image_input_unittest.cc` (std-only): the bounds, hostile sizes and byte lengths, the scaling plan (including a 20,000-case random sweep of its invariants), padding and alpha.
- `runtime/model_image_unittest.cc`: which images a request sends (up to 4 new images and every split of up to 15 over three history turns).
- `runtime/model_image_prep_unittest.cc` (Skia, the data decoder in-process): scaling, padding, transparency, other colour types, a PNG decoded and scaled, junk refused.
- `runtime/inference_process_host_unittest.cc`: the projector handle is forwarded; `vision` without a projector is a violation.
- `models/model_download_plan_unittest.cc` and `models/download_policy_unittest.cc` (the download as one unit, §10.4): both files from the pinned revision, each verified against its own pin (a projector with the weights' hash or the wrong size is discarded); every pin field of both files required, upper-case or over-long hex refused, and a refused unit never planned; text-only models still downloadable unpinned; what a unit is; a fresh download (weights then projector, bytes); the weights without the projector and the projector without the weights (after a hash mismatch on either) as partial downloads that fetch only the missing file; `.partial` files never counting, a resumed one needing only its rest, a complete-sized leftover all of it; files of the wrong size; both complete; deletion; unpinned estimates.
- The web AI tests for the Prompt API (WEB_AI_APIS.md §12).

**Run outside the repo** (vendored llama.cpp and mtmd built with the GN flags, normally and with ASan, UBSan and LSan; tiny random-weight models made for the test: a llama text model with an MLP projector, and a Qwen2-VL model with its M-RoPE projector):

- 18 end-to-end engine tests. **An oracle:** for one image (two shapes), two images, and an image in the history before the new one, on both models, the engine's greedy answer is token for token what llama.cpp's own evaluation gives (`mtmd_tokenize()` and `mtmd_helper_eval_chunks()`, copied into the test from b11641, then the same sampling): same prompt, same positions, M-RoPE's 2-D image grid included. The rest: loading the projector; answers with images at and inside the bounds; refusal of images outside them, of a fifth image, of images on assistant turns; history images; page text that spells the marker can't place an image; no KV reuse across an image; cancellation during image encoding; a JSON-constrained answer with an image; images that don't fit the context; a text-only model refusing images; projectors for another model, of another size, and files that aren't projectors.
- Regression: the engine's earlier end-to-end suites (text, constrained decoding, embeddings) unchanged.
- **Mutation checks:**
  - the bounds, the scaling plan, the composition and the request plan (`image_input`, `model_image`): 15 of 15 caught;
  - the engine's image path against the end-to-end tests: 7 of 10 caught (a fifth image, an image on an assistant turn, the marker not neutralized, images on a text-only model, an image outside the bounds, M-RoPE rows and columns swapped, positions counted per token). The three survivors are equivalent: the KV key cleared after images (image cells hold placeholder tokens that never match, so nothing past them is reused anyway), markers that must pair with images (neutralization already guarantees it), and the new message's images budgeted up front (the re-plan loop reaches the same result);
  - the pinning of projectors: 4 of 4;
  - the download as one unit (`model_download_plan`, `IsCompleteModelFile`): 26 of 26, after three survivors got a test each (upper-case and over-long hex in a pin, a `.partial` exactly the file's size);
  - the Prompt API's checks: WEB_AI_APIS.md §12.
- **Syntax** of the service and browser code against stand-ins; **the mojom** with upstream's generator.

**Needs the build machine:** a compile; Metal and the GPU path for the encoder; a real pinned model (quality, speed, memory) on 8 and 16 GB machines, downloaded with its projector (progress, cancel, resume, delete); the Prompt API with real images in a page.

## 11. GPU inference on Windows and Linux (Vulkan)

Status: implemented behind the GN switch `molt_llama_vulkan` (October 2026), which stays **off** until it has been tested on real GPUs; on Windows the pre-lockdown step of §11.15 now loads the drivers, pending a Windows GPU machine. The roadmap item "GPU inference on Windows and Linux" wants llama.cpp's Vulkan backend (ggml-vulkan) in the inference service, with the CPU as the fallback. macOS keeps Metal. §11.1–11.6 are the research; §11.7 is what was built, §11.8 how it was tested, §11.9 what is left; §11.10–11.12 are the lasting GPU failure record, the setting and the pipeline cache that belong with turning the switch on, and §11.13 how they were tested; §11.14 cuts the shaders' size and §11.15 is the Windows pre-lockdown step. Upstream files below were read at 152.0.7977.169; llama.cpp is the vendored b11641.

### 11.1 Findings in one page

1. **Build.** ggml-vulkan needs the Vulkan headers (`vulkan.hpp`), SPIRV-Headers (`spirv.hpp`), a host tool (`vulkan-shaders-gen`) and a GLSL compiler with glslc's command line. Chromium 152 has every input except glslc: `third_party/vulkan-headers`, `vulkan-loader`, `glslang` (with a `glslang_validator` executable), `spirv-tools` and `spirv-headers` are in DEPS; shaderc is not.
2. **Shaders are built from source, not vendored**, with Chromium's own glslang and SPIRV-Tools through a small glslc-compatible host tool (§11.2). The output is about 40 MB of SPIR-V (§11.8); the build leaves out the weight types no catalogue model uses and compresses the rest, so the binary carries 2.6 MB (§11.14).
3. **The loader is opened at run time.** ggml-vulkan uses `vkGetInstanceProcAddr` and `vkGetPhysicalDeviceFeatures2` as link-time symbols, which would make the whole browser binary depend on `libvulkan.so.1` / `vulkan-1.dll`. A local patch takes them from a loader opened at run time (§11.3).
4. **Linux sandbox:** the seccomp policy already allows what Vulkan drivers need, but on desktop Linux the broker lets in only NVIDIA's driver. Mesa (every Intel and AMD GPU on a stock distro) needs a few broker additions, granted only to processes the browser launches for the GPU (§11.4).
5. **Windows sandbox:** this sandbox type allows only Microsoft-signed DLLs once it locks down, so the Vulkan loader and the vendor driver must be loaded before that. A pre-lockdown step in `utility_main.cc` does that for processes launched for the GPU (§11.15); it needs a Windows GPU machine to confirm.
6. **Runtime:** a device policy in MoltBrowser code (software rasterizers, layered and virtual GPUs refused, a minimum of video memory), layers offloaded to fit the device's free memory, and the CPU as fallback when anything fails (§11.5).
7. **Exceptions:** ggml-vulkan throws on device loss and on Vulkan errors by design, and `llama_decode` doesn't catch, so a lost GPU would terminate the service. Every llama.cpp call that can reach the GPU after load goes through wrappers built with exceptions (§11.6).
8. **Crashes:** a GPU process that crashes or hangs doesn't count against the model (the crash-loop guard of §4.3); the browser reloads the model on the CPU and stops asking for the GPU (§11.5). A driver that fails three times within 30 days isn't used again until it changes (a lasting record in Local State, §11.10); `molt://ai-settings` can turn the GPU off and retry a turned-off driver (§11.11).
9. **Pipeline cache:** the browser keeps one Vulkan pipeline cache per GPU, driver and shader set and hands it to the service read-only; the service seeds ggml-vulkan's cache with it and returns the cache after the first answers. Both sides check its header against the device; the driver parses the rest (§11.12).

### 11.2 Build

**What ggml-vulkan needs** (`ggml/src/ggml-vulkan/CMakeLists.txt` at b11641):

- `ggml-vulkan.cpp` (16,649 lines), `ggml-vulkan-buffers.cpp` and `ggml-vulkan-debug.cpp`, with `vulkan.hpp` (`VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1`: every Vulkan call goes through a dispatcher filled from `vkGetInstanceProcAddr`) and `spirv/unified1/spirv.hpp`.
- `vulkan-shaders-gen`, a host executable (std C++17 and threads). Run once without `--source`, it writes `ggml-vulkan-shaders.hpp` (the declarations); run once per `.comp` file (155 of them), it compiles every variant of that shader and writes a `.cpp` that embeds the SPIR-V as byte arrays. For each variant it runs `glslc -fshader-stage=compute --target-env=vulkan1.2|vulkan1.3 IN -o OUT [-O] [-MD -MF DEP] -DNAME=VALUE...` as a subprocess (`-O` is left out for cooperative-matrix, bf16, rope and dot2 shaders because of spirv-opt bugs) and fails on any output on stderr.
- CMake first compiles seven feature-test shaders to learn which extensions glslc supports, and passes `GGML_VULKAN_<FEATURE>_GLSLC_SUPPORT` to both the generator and `ggml-vulkan.cpp`.

**What Chromium 152 has** (DEPS at the pin):

| Need | In 152 | Notes |
|---|---|---|
| Vulkan headers | `third_party/vulkan-headers` @ `d314eae7` (`VK_HEADER_VERSION` 357), target `:vulkan_headers` | Has `vulkan.hpp`; ggml handles both the old and the `vk::detail` (≥ 301) namespaces. |
| SPIRV-Headers | `third_party/spirv-headers` @ `29981f65` | `spirv.hpp` |
| glslang | `third_party/glslang` @ `8292684e` (16.4.0+): `glslang_sources` built with `ENABLE_OPT=1`, and an `executable("glslang_validator")` | Compiles all seven feature tests (cooperative matrix 1 and 2, decode vector, integer dot product, bf16, e2m1, e4m3). The distro glslc here (shaderc 2023.8) supports only the first. |
| SPIRV-Tools | `third_party/spirv-tools` @ `a665e21f` | The optimizer that glslc's `-O` runs (`RegisterPerformancePasses`). |
| Vulkan loader | `third_party/vulkan-loader` @ `59a70594` | Chrome ships it as `libvulkan.so.1` / `vulkan-1.dll`. MoltBrowser's `scripts/package-linux.sh` copies `*.so` only, so Linux packages use the system loader (`libvulkan1`). |
| glslc / shaderc | no | |

**Shader sizes, measured here** (the vendored b11641 shaders, generated with the distro's glslc, so with cooperative matrix 1 only): 1,267 variants, **31.0 MB of SPIR-V**, 134 MB of generated C++, about 300 CPU-seconds of glslc. With Chromium's glslang every shader family is on, so expect more (§11.8 has the number from the GN-equivalent build). It is all linked into the browser binary, because the inference service runs in a utility process of the same binary.

**Build at build time, or generate once and vendor?**

- *Vendored:* 30+ MB of opaque binary (or about 130 MB of C arrays) in the repository at every llama.cpp update; reviews can't see what changed; CI would still need a pinned glslc to check that the blobs match the sources; and SPIR-V is code that the GPU driver's compiler parses, so blobs nobody can review are a supply-chain risk.
- *At build time with Chromium's glslang* (chosen): the GLSL sources are what gets reviewed, the compiler is the one Chromium pins, nothing generated is checked in, and the inputs move with the Chromium rebase. Cost: glslang and the SPIRV-Tools optimizer built for the host, and a few CPU-minutes of shader compiles, only when the switch is on.
- The generator needs glslc's command line (it runs it once per variant), and Chromium has no glslc. `glslang_validator` behind a wrapper doesn't match: its flags differ, it optimizes by default with a different pass list, and on Windows a script can't be the subprocess `CreateProcessA` starts. So a small host tool, `ggml_vk_glslc`, implements exactly the glslc subset the generator uses on glslang's and SPIRV-Tools' libraries: the same front end, `-O` as spirv-opt's performance passes (what glslc does), and `-MD -MF` depfiles for ninja.
- The feature defines are fixed in GN to what the pinned glslang supports, and a build step compiles the feature tests with `ggml_vk_glslc`, failing if one stops compiling, so a glslang roll can't silently drop a shader family.

### 11.3 Linking the loader

- `ggml_vk_instance_init()` calls `ggml_vk_default_dispatcher_instance.init(vkGetInstanceProcAddr)`, and two other places use the global `vkGetInstanceProcAddr` and `vkGetPhysicalDeviceFeatures2`. CMake links `Vulkan::Vulkan` for them.
- In Chromium that would add `DT_NEEDED libvulkan.so.1` to the one browser binary, which then wouldn't start without a Vulkan loader installed; on Windows it would import `vulkan-1.dll` into every process, renderers included.
- **Patch:** ggml-vulkan is built with `VK_NO_PROTOTYPES` (so no global Vulkan function can be referenced by accident) and takes `vkGetInstanceProcAddr` from a loader opened at run time (`dlopen("libvulkan.so.1")` / `LoadLibrary("vulkan-1.dll")`), only when the GPU is requested. On Linux the pre-sandbox hook has already loaded `libvulkan.so.1` with `RTLD_NODELETE`, so the `dlopen` inside the sandbox finds it by name without touching the file system. If the loader can't be opened, the backend isn't registered and the model loads on the CPU.

### 11.4 Sandbox

**Linux.** Our process gets upstream's `on_device_model::PreSandboxHook` (keyed on the sandbox type, `content/utility/utility_main.cc:335`), not the Dawn warm-up (`PreSandboxInit`, keyed on upstream's service name, `:292`).

- *seccomp* (`GetGpuProcessSandbox` → `GpuProcessPolicy`, `sandbox/policy/linux/bpf_gpu_policy_linux.cc`): dlopen syscalls (`:46`), `ioctl` and `getdents64` (`:80-81`), `mmap` with any flags on x86-64 (`:86`; so the `MAP_POPULATE` patch of §0.4 matters only on arm64), `memfd_create` without exec mappings (`:109`). Enough for Mesa's and NVIDIA's Vulkan drivers.
- *Libraries* (`LoadVulkanLibraries`, `content/common/gpu_pre_sandbox_hook_linux.cc:585`, before the sandbox): `libvulkan.so.1`, `libvulkan_radeon.so` (RADV), `libvulkan_intel.so` (ANV), `libGLX_nvidia.so.0` (NVIDIA's Vulkan ICD) and `libvulkan_freedreno.so`. Not lavapipe, NVK or hasvk, so those can't load in the sandbox at all.
- *Files* (`FilePermissionsForGpu`, `:622`). On desktop Linux (not ChromeOS, not Chromecast) the broker allows `/etc/drirc`; the two ICD directories with five fixed manifest names (`intel_icd.x86_64.json`, `radeon_icd.x86_64.json`, `nvidia_icd.json`, `mali_icd.json`, `freedreno_icd.aarch64.json`; `AddVulkanICDPermissions`, `:437`); and `/dev/shm`, `/dev/dri/card0-9`, the NVIDIA nodes and `/proc/driver/nvidia/params` (`AddStandardGpuPermissions`, `:454`). Render nodes, `/dev/dri` itself, `/sys/dev/char/*` (`AddDrmGpuPermissions`, `:243`) and brokered `readlink` (`CommandSetForGPU`, `:606`) are ChromeOS-only.
- *So:* NVIDIA's proprietary driver has its nodes, manifest and library, and can probably create a device (to verify). Mesa can't: libdrm's device enumeration lists `/dev/dri`, reads `/sys/dev/char/226:N/device/*` (with a `readlink` of its `subsystem` link) and opens `/dev/dri/renderD*`, all refused; and current Mesa names its manifests `intel_icd.json` and `radeon_icd.json`, without the architecture suffix (this machine's Mesa 25.2 does), which aren't on the list.
- *Minimal change, for GPU launches only:* the browser adds a switch (`--molt-inference-gpu`) to the command line of an inference process it launches for the GPU, and the hook then adds read-only `/dev/dri`; for each of `/dev/dri/renderD128`-`137` present, the node read-write and, read-only, the nine files libdrm reads about the GPU behind it, below `/sys/dev/char/226:N/device/`: the `drm` directory (stat and listing), the `subsystem` link (`readlink`), `uevent` (the bus address), `vendor`, `device`, `subsystem_vendor`, `subsystem_device`, `revision` and `config` (where a kernel has no `revision` file); the two unsuffixed Mesa manifests; and the broker's `readlink` (still limited to allowed paths). The list is `content/utility/on_device_model/molt_gpu_inference_sandbox_policy_linux.h`. Each entry is an exact path; nothing in sysfs is granted recursively, because the device's directory links to its bus and through the bus to every other device's directory (network addresses, disk and USB serial numbers, monitor EDIDs). ChromeOS's GPU sandbox grants the same drivers the whole `/sys/dev/char/226:N/` instead. libdrm reads these files through the `/sys/dev/char` path: it first tries to resolve it with `realpath()`, which needs a `readlink` of `/sys` that the broker refuses, and then uses the path as given. Without the switch the hook does exactly what upstream's does, so the CPU path's sandbox is unchanged. Render nodes are the unprivileged DRM interface (no mode setting); the process can already open the primary nodes `card0-9` read-write, and reaching the GPU driver's ioctls is the accepted cost of this sandbox type (§5.2, item 6). No library is added to the preload list.
- Not reachable in the sandbox, and harmless: Mesa's and NVIDIA's on-disk shader caches (the browser keeps a Vulkan pipeline cache for the service instead, §11.12), `drirc.d`, and implicit Vulkan layers (their manifests aren't readable, which keeps overlay layers out of the process).

**Windows.** The type runs in the LPAC `cr.sb.odm` with `lpacChromeInstallFiles`, `registryRead`, `lpacPnpNotifications` and a `chromeInstallFiles` impersonation capability (`sandbox/policy/win/sandbox_win.cc:439`), with the token `USER_RESTRICTED_SAME_ACCESS`/`USER_LIMITED` (`content/browser/service_host/utility_sandbox_delegate_win.cc:157`: "USER_RESTRICTED breaks the Direct3D backend") and no win32k lockdown.

- **Signed DLLs only after lockdown.** `GenerateConfigForSandboxedProcess` sets `MITIGATION_FORCE_MS_SIGNED_BINS` as a *delayed* mitigation for every type except ScreenAI, speech and the Media Foundation CDM (`sandbox_win.cc:505-510`); it takes effect at `LowerToken()` (`utility_main.cc:485`). MoltBrowser's own `vulkan-1.dll` isn't Microsoft-signed; vendor ICDs are signed by their vendors and WHQL-catalogued, and whether that passes the mitigation's check is unverified. Upstream avoids the question: its service creates Dawn devices in `PreSandboxInit`, before lockdown, keyed on its own service name, and `UtilityConfig.preload_libraries` (`PreLockdownSandboxHook`, `utility_main.cc:180`, filled by the PassKey-gated `ServiceProcessHost::Options::WithPreloadedLibraries`) loads named DLLs before lockdown.
- **What a Windows GPU path needs:** a pre-lockdown step for our service (a `utility_sub_type` check in `utility_main.cc`, like webnn's at `:474`) that loads the loader, creates an instance and a device so the ICD and its dependencies are mapped, and keeps that instance alive so they stay mapped (`vkDestroyInstance` unloads the ICD; a later `LoadLibrary` of a module already mapped doesn't map a new image). Whether the ICD's registry reads and DriverStore files are reachable from this LPAC, and whether the D3DKMT adapter queries the loader uses for discovery work there, needs a Windows machine with a GPU. Windows builds are paused (CLAUDE.md), so it isn't built here.
- **Built since:** §11.15. Without that step the loader or the ICD can't be loaded after lockdown (or the loader finds no usable device), so the backend isn't registered and the model loads on the CPU. A driver that crashes half-way is a GPU failure (§11.5), which moves the model to the CPU.

**macOS** is unchanged (Metal; the switch is refused there).

### 11.5 Runtime

**Which device.** ggml-vulkan's default (no `GGML_VK_VISIBLE_DEVICES`) takes every discrete and integrated GPU with 16-bit storage, drops duplicates (one GPU through two drivers: RADV preferred over AMDVLK, and so on), and only if none is left the first non-CPU device; it never picks a CPU-type device (lavapipe, llvmpipe, SwiftShader). It already works around specific driver bugs itself (old AMD Windows drivers, Intel Windows driver ranges, cooperative matrices only where they are fast). MoltBrowser adds a policy, a pure function over the properties ggml reads (vendor, device type, driver id, driver and API version, device-local memory):

- refused: CPU-type devices, and software drivers by driver id (llvmpipe, SwiftShader) whatever type they report; drivers layered on another API or forwarding from a VM by driver id (Dozen over D3D12, MoltenVK and KosmicKrisp over Metal, Venus and gfxstream, Vulkan SC emulation); virtual and "other" device types; API versions below 1.2 (ggml's own minimum); less than 2 GiB of device-local memory;
- a table of (vendor, driver id, driver version range) entries for drivers found broken in testing, empty until there is evidence;
- preferred: discrete over integrated, then the most free memory. One device only: a model is never split across GPUs.

**How many layers.** llama.cpp at b11641 has no fitting API in `llama.h` (`common/fit.cpp` isn't built). The service reads the layer count from the GGUF header and puts as many layers on the GPU as fit in 90% of the device's free memory (`VK_EXT_memory_budget` where the driver has it), after a reserve for compute buffers and a minimal KV cache; the rest stay on the CPU. Under a quarter of the layers isn't worth the transfers: then the CPU alone. After the load, `n_ctx` is also capped by the memory left on the device. If the model or its context still can't be allocated, it is loaded again on the CPU in the same process.

**Device lost, driver crash, hang.**

- A lost device (a GPU reset, a driver fault, a TDR on Windows) makes ggml-vulkan throw `vk::DeviceLostError`. With the wrappers of §11.6 the request ends with a new status, `kBackendFailed`; the service stops using the context and never frees it (freeing on a lost device can throw inside a destructor), and the browser ends that process and reloads the model on the CPU.
- A driver that crashes the process (or a `GGML_ABORT` in ggml-vulkan), and a GPU hang that stops progress (the liveness timeout of §5.3), look like any crash or hang to the browser. Today each counts in the model's crash-loop guard (§4.3: two load crashes in an hour, or three generation crashes in ten minutes, lock the model out), which would lock out a model that runs fine on the CPU.
- So the browser keeps a GPU record for the session: a crash, a hang or a `kBackendFailed` of a process that was loading with the GPU requested, or running on the GPU, is a **GPU failure**, not a model crash. It turns the GPU off for that model for the rest of the session (a load in progress is retried on the CPU at once), and two GPU failures across models turn it off for every model. Crashes on the CPU path count as before. A model file that crashes the tokenizer can therefore crash once more than before (on the GPU, then on the CPU), at most once per session. Across sessions the failures are also counted against the driver (§11.10).

### 11.6 Exceptions

ggml-vulkan must be built with exceptions: it uses `vulkan.hpp`'s throwing API and catches its own errors in places. What can reach a `-fno-exceptions` caller, at b11641:

- `ggml_backend_vk_reg()` (instance creation, device enumeration) catches everything and returns `nullptr`.
- Model loading and context creation (weight upload, device creation, pipelines) run inside `llama_model_load_from_file_ptr` and `llama_init_from_model`, which catch `std::exception`; every `vulkan.hpp` error derives from it (`vk::SystemError` from `std::system_error`).
- **Not caught:** `llama_decode` / `llama_process` / `llama_encode` (graph compute: queue submits and fence waits rethrow `vk::DeviceLostError` and throw `vk::SystemError` on other errors; `VK_CHECK`, `ggml-vulkan-types.h:195`), `llama_synchronize` and everything that synchronizes first (`llama_get_logits_ith`, `llama_get_embeddings*`), and `llama_memory_clear(mem, true)` (it clears device buffers). On the CPU these don't throw (the audit at the top of `llama_engine.cc`); on the GPU they can.
- `mtmd_encode_chunk` and `mtmd_init_from_file_ptr` catch `std::exception` (§10.3), so image encoding on the GPU is covered.
- Destructors: `llama_free` and `llama_model_free` on a lost device can throw out of a destructor, which terminates even with exceptions on. Hence nothing is freed after a GPU failure.
- `GGML_ABORT`s in ggml-vulkan (29, e.g. an op that `supports_op` accepted but the backend can't run) abort as they would on the CPU; the browser counts them as GPU failures (§11.5).

### 11.7 What was built

```
browser (BrowserAIRuntime, InferenceProcessHost)           inference service (kOnDeviceModelExecution)
-----------------------------------------------           -------------------------------------------
use_gpu = BUILDFLAG(MOLT_LLAMA_VULKAN) && !--disable-gpu
          && GpuFallbackGuard.AllowGpu(model file)
launch [--molt-inference-gpu] --LoadModel(use_gpu)-->      Linux pre-sandbox hook: + render nodes (switch only)
                                                           LlamaEngine::Load:
                                                             ListVulkanDevices (llama_guard: loader, instance)
                                                             ChooseGpuDevice -> PlanGpuOffload(block_count)
                                                             model+projector+context on the device
                                                             ... failed? all again on the CPU (kLoadFailed)
     <---- ModelDescription{backend, gpu_status, gpu_layers}  (checked: IsValidGpuDescription)
generation --------------------------------------------->  decode/logits/sample/clear through llama_guard
     <---- kBackendFailed (lost device) ----------------     context poisoned, never freed
crash / hang / memory kill while on the GPU:
  GpuFallbackGuard (not CrashLoopGuard); load retried on the CPU at once; GPU off for that model this session
```

**GN** (`src/third_party/llama_cpp/BUILD.gn`, `llama_cpp.gni`):

- `molt_llama_vulkan` (default false; asserts Linux or Windows).
- Host executables `ggml_vk_glslc` (`chromium/ggml_vk_glslc.cc` on `//third_party/glslang/src:glslang_sources` and `//third_party/spirv-tools/src:spvtools_opt`) and `vulkan_shaders_gen` (upstream's generator, unmodified, with the seven feature defines).
- `action("ggml_vulkan_checks")` (`chromium/ggml_vulkan_checks.py`): every enabled feature test compiles with `ggml_vk_glslc`, and the 155-file shader list in BUILD.gn equals the directory.
- `action("ggml_vulkan_shaders_hpp")` and `action_foreach("ggml_vulkan_shaders_cpp")` (one per `.comp`, with a depfile for its `#include`s), run through `//build/gn_run_binary.py`; tool paths are passed with `./` so a native build's bare tool name is never looked up in `PATH`.
- `source_set("ggml_vulkan")`: the three ggml-vulkan sources, the 155 generated files and `chromium/ggml-vulkan-moltbrowser.cpp`; defines the features, `GGML_VULKAN_MOLTBROWSER` and `VK_NO_PROTOTYPES`; exceptions on; the Vulkan headers' window-system platform defines undone (`-U`), since ggml needs no surfaces and X11's macros clash with C++.
- `source_set("llama_guard")` (always built; with exceptions): `chromium/llama_guard.{h,cc}`, plus `:ggml_vulkan` and `MOLT_LLAMA_VULKAN` with the switch.
- `//chrome/services/molt_ai/inference/public:buildflags` (`MOLT_LLAMA_VULKAN`) for the browser.

**llama.cpp patches** (README.chromium): `ggml-vulkan.cpp` initializes its dispatcher from `ggml_vk_moltbrowser_get_instance_proc_addr()` under `GGML_VULKAN_MOLTBROWSER`, and the eight places that call Vulkan directly (`vkGetInstanceProcAddr` twice, `vkGetDeviceProcAddr`, `vkGetPhysicalDeviceFeatures2` three times, and `vkCmdCopyBuffer` twice in `ggml-vulkan-buffers.cpp`) go through the dispatcher. Nothing else in llama.cpp changed; the generator is upstream's.

**Service:**

- `engine/gpu_offload.{h,cc}` (std-only): `CheckGpuDevice`, `ChooseGpuDevice`, `PlanGpuOffload`, `GpuContextLimit` (§11.5).
- `LlamaEngine::Load`: one attempt function for "on this device" and "on the CPU". With `use_gpu` in a Vulkan build: list devices, choose, read the GGUF block count, plan, then load the model, the projector (`mtmd_context_params::device`, so the encoder runs on the same GPU) and the context, with `n_ctx` also capped by `GpuContextLimit`. Any failure on the GPU frees what was loaded (the device is healthy: the failure was reported, not thrown at us) and loads everything on the CPU. Vulkan builds always pass an explicit device list to llama.cpp, empty for the CPU; macOS keeps `n_gpu_layers = -1` (Metal) and ignores `use_gpu`.
- After a guarded call caught an exception: `kBackendFailed` for the request and every later one, `ForgetConversation` does nothing, and `~Model` leaks the context, the model and the projector.
- The embedding model's process never asks for the GPU, so it never brings Vulkan up.

**Browser:**

- `InferenceProcessHost`: `LoadRequest::use_gpu` → `ModelLoadParams.use_gpu` and `--molt-inference-gpu` on the process's command line; `IsValidGpuDescription` (backend `"vulkan"` exactly with `kOn`, only when asked, 1..`kMaxGpuLayers` layers); `kBackendFailed` only from a model on the GPU (`StreamLimits::model_on_gpu`; from a model on the CPU it is a protocol violation, which counts against the model like a crash); `OnModelProcessLost(…, gpu_failure)`, true for a crash, hang or memory kill of a process that was loading for the GPU or running on it (not for a protocol violation or a launch failure).
- `BrowserAIRuntime`: `GpuFallbackGuard` next to `CrashLoopGuard` (same lock); a GPU failure during a load retries the load at once on the CPU; `kBackendFailed` records a GPU failure, forgets the model and ends its process, with the message "The graphics processor stopped working while running the on-device model. MoltBrowser will run the model on the processor instead: try again." The load log line shows `gpu_status` and `gpu_layers`. `--disable-gpu` keeps on-device AI off the GPU too.

**Sandbox (Linux):** `content/utility/on_device_model/on_device_model_sandbox_init.cc` (imported from 152, then hooked): `PreSandboxHook` passes its broker command set and file list through `MaybeAddMoltGpuInferencePermissions()` (`molt_gpu_inference_sandbox_linux.{h,cc}`, listed in that directory's imported BUILD.gn), which returns at once without `--molt-inference-gpu`. With it: read-only `/dev/dri`; read-write `/dev/dri/renderD128`–`137` and read-only `/sys/dev/char/<major>:<minor>/` for each present one; `intel_icd.json` and `radeon_icd.json` in both ICD directories; brokered `readlink`.

**Windows:** the pre-lockdown step of §11.15 (built later). Without it the loader or the ICD can't load after lockdown, so the service reports `kNoDevice` and loads on the CPU.

### 11.8 How it was tested

The machine this was built on has no Chromium checkout and no GPU: 4 cores, and Mesa 25.2's lavapipe (llvmpipe), a CPU rasterizer that the device policy refuses, as the only Vulkan device. So the GN targets were generated and built with the real GN binary in a stub source root, the Vulkan path ran end to end on lavapipe with a test-only override, failures were injected with a Vulkan layer, and the rest was syntax-checked against stand-ins.

**In the repository:**

- `engine/gpu_offload_unittest.cc` (12 tests, std-only). Verdicts: usable at exactly the minimums and without a driver id; software drivers by type and by driver id, whatever type they claim; the layered drivers; virtual, "other" and unknown types; API 1.1 with any patch or variant bits; one byte under the memory minimum; blocklist ranges, inclusive, per vendor and driver. The choice: no device, lavapipe never, discrete before integrated, then free memory (capped at the heap), ties keep the first, refused devices skipped wherever they are. The plan: the whole model, a partial offload within the reserves, never more than the repeating layers, under a quarter of the layers, an unknown layer count, too little memory, an empty file, sizes that would overflow; and the context cap.
- `runtime/stream_validator_unittest.cc`: `IsValidGpuDescription`; `GpuFallbackGuard` (one failure turns the GPU off for that model, two for every model).
- `runtime/inference_process_host_unittest.cc` (6 new): a GPU load is launched with `--molt-inference-gpu` and described as on the GPU; a GPU description nobody asked for is a violation; a crash on the GPU and a hang while loading for it are GPU failures; a crash after the service fell back to the CPU, and a death while idle, are not.
- `content/utility/on_device_model/molt_gpu_inference_sandbox_policy_linux_unittest.cc` (4 tests, std-only, part of `molt_ai_unittests` on Linux): the broker list of §11.4 for no render node, one and two; every entry an exact path, the sysfs ones read-only and one level below the device; none of the device's links or other entries.

**Run here:**

- *GN.* `gn gen --check` passes with the switch off and on. The stub root holds the worktree's `//third_party/llama_cpp` and the engine's std-only targets; glslang, SPIRV-Tools, SPIRV-Headers and Vulkan-Headers at the revisions 152 pins (checked against DEPS) with their own BUILD.gn files; Chromium-like flags (clang, libc++ with extensive hardening, C++23 after the targets' own `-std`, no exceptions or RTTI by default). Building it found one bug, fixed: a native build's host tool was passed by bare name and looked up in `PATH`.
- *Build.* With the switch on, the host glslang and SPIRV-Tools, `ggml_vk_glslc`, the generator, the feature checks, the 155 shader files, ggml-vulkan, llama.cpp and the engine build from scratch in 16 minutes with two jobs on 4 cores. The generated `mul_mm.comp.cpp` (79 MB) needs about 2.4 GB of compiler memory. Shaders: 1,569 variants, **40.2 MB of SPIR-V**, 174 MB of generated C++, 80 s of generation. Every variant passes the pinned SPIRV-Tools validator. A test program that links the engine and llama.cpp grows from 6.4 MB to 48.3 MB with the switch on (41.6 MB more text: the SPIR-V and ggml-vulkan's code).
- *Linking.* With the switch on, binaries have no `DT_NEEDED` on the Vulkan loader and no undefined `vk*` symbol. In the default environment ggml lists no device (it skips CPU-type devices); with `GGML_VK_VISIBLE_DEVICES=0` it lists lavapipe (type CPU, driver id 13, API 1.4); with a missing ICD, none.
- *Unit tests.* The engine directory's std-only suite (153 tests, the new ones included) passes in both configurations; with the switch off, a GPU request reports `kNotBuilt` and gives the same text as the CPU; the `stream_validator` tests pass with GCC `-Werror` and with clang ASan and UBSan.
- *End to end.* `LlamaEngine` built as the service builds it, on three tiny models (a ChatML llama, and llama.cpp's random llama and Qwen3 test models), three greedy chat turns each with KV reuse:

  | Case | How | Result, all three models |
  |---|---|---|
  | CPU | `use_gpu = false` | the baseline text |
  | No device | `use_gpu`, default environment | `kNoDevice`, CPU, same text |
  | Refused | lavapipe visible | `kNoUsableDevice`, CPU, same text |
  | On the GPU | lavapipe let through by `SetAllowSoftwareGpuForTesting` | backend `vulkan`, `kOn`, every layer, same text, same KV reuse |
  | Allocation failure | device allocations after the first fail | `kLoadFailed`, loaded again on the CPU in the same process, same text |
  | Lost while loading | device lost at the first submit | `kLoadFailed`, CPU, same text |
  | Lost mid-answer | device lost after 3/4 of a clean run's submits | that turn ends `kBackendFailed`, its text up to then identical; no crash, nothing freed |
  | Lost early | device lost after 400 submits | the second turn ends `kBackendFailed` after 8 tokens, the third at once with none |

  The failures come from a test-only Vulkan layer (kept outside the repository) that returns `VK_ERROR_DEVICE_LOST` from every queue submit and fence wait after N submits, or `VK_ERROR_OUT_OF_DEVICE_MEMORY` from device allocations after N. Clean runs make 828 to 897 submits and 7 or 8 allocations.
- *Numerics.* One prompt's last-token logits on the CPU and on lavapipe (shaders from `ggml_vk_glslc`): largest absolute difference 0.000886, 0.000010 and 0.000021 on the three models, cosine at least 0.9999996, the same top token.
- *Official builds.* Chromium's official desktop builds drop unwind tables (`exclude_unwind_tables`: `-fno-unwind-tables -fno-asynchronous-unwind-tables`), and an exception needs them in every frame between the throw and the catch. Built that way, the cases above (lost while loading, lost after 400 and after 700 submits, allocation failure) give the same results: code built with exceptions keeps the unwind entries of functions that can throw, and every frame from ggml-vulkan up to `llama_guard` is such code.
- *The broker list against libdrm and Mesa.* libdrm 2.4.125 and Mesa 25.2's RADV and ANV, run against a fake `/sys` and `/dev/dri` (an Intel and an AMD GPU, laid out as the kernel does) through an `LD_PRELOAD` shim that refuses what the broker would, with its matching rules, under the list the shipped hook produces: libdrm's device enumeration (`drmGetDevices2` and `drmGetDevice2`, with and without the PCI revision, and `drmGetPrimaryDeviceNameFromFd`, which libdrm_amdgpu calls) and the two drivers' enumeration give the same results as with the whole `/sys/dev/char/226:N/` readable, also on a kernel without the `revision` file (libdrm then reads `config`), and other devices' files reached through the GPU's links are refused. Of libdrm's functions only `drmGetDeviceNameFromFd2` fails (it reads the node's own `uevent`); neither driver nor libdrm_amdgpu imports it.
- *Static checks.* The mojom with upstream's parser and generator; the syntax of the service, `inference_process_host` and its test, `stream_validator`, and `browser_ai_runtime` (with `MOLT_LLAMA_VULKAN` 0 and 1) with clang and Chromium's flags against stand-ins; the sandbox hook and its helper against stand-ins of the broker types. `python3 tools/fuzz/molt_fuzz.py check` passes; no new parser of untrusted input (the layer count comes from ggml's GGUF parser, the one the model load already runs).

**Needs a Chromium build:** all of the above with the real toolchains (ggml-vulkan and the generator under Chromium's clang and libc++, and clang-cl on Windows), `gn check` on the real tree (new edges: the content utility directory on `//sandbox/linux:sandbox_services`, the browser on `public:buildflags` and `//content/public/common`), the browser unit tests, and the hardware of §11.9.

### 11.9 What is left

- **Real GPUs, before the switch goes on.** Nothing has run on a hardware GPU. To verify: NVIDIA's driver and Mesa's RADV and ANV on desktop Linux, in the real sandbox; speed against the CPU; load time and the first answer's time, with and without the pipeline cache (§11.12); a real device loss (a GPU reset) and a driver crash, and that each reaches the GPU record (§11.10). The load log line shows `gpu_status`, `gpu_layers` and the GPU's name; the device report logs its key.
- **The broker grants, on NVIDIA, RADV and ANV hardware, before the switch goes on.** The list of §11.4 comes from reading upstream, libdrm and Mesa and from running libdrm and the drivers' device enumeration against a fake sysfs (§11.8), not from a run on a GPU. On each of the three, in the real sandbox: the device is found and created with exactly that list, a model runs, and nothing the driver needs is refused (a refusal may show only as a driver warning in the log); and a GPU behind a PCI bridge, and a machine with two GPUs, find theirs. Grant more only with the file named, never a directory recursively.
- **Windows:** the pre-lockdown step is built (§11.15); what it needs from a Windows GPU machine is listed there and in the checklist.
- **The GPU record and the setting** are built (§11.10, §11.11). Left: the browser and settings-page tests in a Chromium build (§11.13), and on hardware, that a real driver crash at device creation, at the first decode and mid-answer is attributed to the reported driver. The blocklist is still empty until testing finds a broken driver; the record makes it less urgent (a broken driver costs at most three failures, then one a month).
- **The pipeline cache** is built (§11.12), but lavapipe, the only driver here, keeps its compute pipelines out of `VkPipelineCache`, so the win was measured only with an emulated caching driver. On RADV, ANV and NVIDIA: the cache's size after a model's first answer (the 32 MiB cap assumes a few MiB), the first answer's time cold and warm, and whether a driver keeps pipelines created with `VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR` (ggml-vulkan sets it wherever `VK_KHR_pipeline_executable_properties` exists, only for a debug printout) out of its cache; if one does, drop the flag in MoltBrowser builds (lavapipe stored nothing with or without it).
- **Binary size:** done, 2.6 MB instead of 40 MB (§11.14).
- **Memory on integrated GPUs.** There device memory is system memory; whether the footprint watchdog (§8) sees it depends on the driver. Not measured. A memory kill on the GPU counts as a GPU failure, so the worst case is a reload on the CPU.
- **Image encoding.** A device lost while the projector encodes an image is caught by mtmd (§11.6), so that request ends with `kDecodeFailed`; the next decode reports `kBackendFailed`.
- **Not planned:** embeddings on the GPU (the embedding model's process never asks); one model split across GPUs.
- **Sanitizers over the Vulkan path.** ASan and UBSan ran over the std-only code, not over ggml-vulkan with a driver.
- **CPUs without AVX2.** The CPU path, the fallback for every GPU failure, required AVX2 (`kUnsupportedCpu`) when this was written. Section 12 chooses the CPU kernels at run time instead.
- **macOS:** the record, the setting and the cache don't cover Metal (why: §11.11).

### 11.10 The lasting GPU failure record

`GpuFallbackGuard` (§11.5) remembers GPU failures for one session, so a driver that fails at every load costs a crash per model per session. `GpuDriverRecords` (`runtime/gpu_driver_record.{h,cc}`, std-only) remembers them across sessions, per driver.

**What counts.** Every GPU failure of §11.5 (a crash, hang or memory kill of a process loading for the GPU or running on it, and `kBackendFailed`), and one more: a device lost while the model was being loaded on it. The service used to report that as `kLoadFailed`, like running out of device memory, and reloaded on the CPU in the same process; now `molt_llama::DeviceLost()` asks the device's fence (`vkGetFenceStatus`, `VK_ERROR_DEVICE_LOST`) and the status is `kDeviceLost`, a GPU failure for the session guard and the record. Not counted, as before: running out of device memory (`kLoadFailed`), a death while idle, a protocol violation, a launch failure; and new: a crash after the browser refused the device (the CPU was loading).

**The key: what identifies the driver.** The vendor and device ids (two GPUs in one machine are two entries), the `VkDriverId` (RADV and AMDVLK on one GPU), `driverVersion` (so a new driver is tried at once) and `VkPhysicalDeviceIDProperties::driverUUID` (the driver build: a distribution's rebuild of the same version is a new driver too). The device's name is kept for display only; `apiVersion` is reported but adds nothing; the `pipelineCacheUUID` keys the cache instead (§11.12).

**Knowing the device before it can crash.** The service used to choose the device inside `LoadModel`, so a driver that crashed while creating the device, uploading weights or compiling pipelines took the process down before the browser knew which GPU it was. Now (`LlamaEngine::ChooseGpu`, `MoltInferenceService`):

1. The browser sends `ModelLoadParams.gpu_client` (a `GpuLoadClient` it implements) and `skip_gpu_drivers`, the record's turned-off keys (at most 8).
2. The service lists the Vulkan devices and chooses one with the device policy of §11.5, skipping those keys (`GpuDeviceInfo::turned_off`, verdict `kTurnedOff`: on a machine with two GPUs the other one is used). No device left because of the list: `GpuStatus::kTurnedOff`.
3. It reports the choice, `OnGpuChosen(GpuDeviceIdentity)`, and waits for the answer before it creates the device. The browser checks the report (once per load, only for a GPU load, the name within `kMaxGpuNameBytes`; else a violation), sanitizes it (`GpuDeviceReport`), and answers from the record and the setting: no (`kTurnedOff`, the CPU), or yes with the device's pipeline cache. A client that goes away counts as no.
4. A process lost after the report counts against the reported driver (`OnModelProcessLost(…, gpu_device)`); one lost before it (in the loader, instance creation or device listing, which run the driver too) against a single **unidentified** entry. When that entry is turned off the browser stops asking for the GPU at all, since it can't tell which driver it was.

The service is hostile, so its report is only a key to count against: the browser's own record decides (a turned-off device the service chooses anyway is refused), and `IsConsistentGpuStatus()` (`stream_validator.h`) holds the load's `GpuStatus` to what the browser saw: `kOn`, `kDeviceLost`, `kLoadFailed` and `kTooLittleMemory` only on a device it reported and the browser allowed, `kTurnedOff` only when it reported none or the browser said no, `kNotBuilt`, `kNoDevice` and `kNoUsableDevice` only without a report, `kNotRequested` exactly when the GPU wasn't asked for.

**N = 3 within 30 days, then off until the driver changes.** Why three:

- One failure is often not the driver's: a GPU reset caused by another program, a Windows TDR during a game, suspend and resume, a driver update replacing files under a running process, memory pressure on an integrated GPU. The session guard already absorbs those (one per model, two per session).
- A driver that fails at every load fails once or twice a session (once per model the user tries, two at most), so three failures within 30 days means at least two sessions. Two would turn a driver off after one bad session; four or more costs more crashes for little more certainty.
- What a driver that always fails costs: at most three failures, each a crash or hang that the browser follows with a CPU load at once (the user sees a slow load, not an error) or, mid-answer, one failed answer. Before: one per model per session, forever.

**The retry rule: probation.** A turned-off driver is tried again once 30 days have passed since it was turned off, when MoltBrowser's version (`kMoltBrowserVersion`) differs from the one that turned it off, or when the user clicks "Try the GPU again" (§11.11). Each is one attempt on probation: one more failure turns it off again for another 30 days. A broken driver therefore costs at most one failure a month. Why these:

- Time, because a driver's version isn't all that changes: on Linux the kernel driver (amdgpu, i915/xe, nvidia.ko) can fix resets without the Vulkan driver's version changing, and environmental causes (another program, a monitor setup, a BIOS update) go away.
- MoltBrowser's version and not Chromium's, because a MoltBrowser update is when llama.cpp, ggml-vulkan and its shaders change; Chromium's security releases come every week or two and would retry too often.
- After a quiet month on probation the entry is forgotten (three failures again).

**Storage: Local State, not a profile.** `molt_ai.gpu_inference.driver_records` (`runtime/gpu_inference_prefs.{h,cc}`, registered by `ChromeContentBrowserClient::RegisterLocalStatePrefs`), because `BrowserAIRuntime` is browser-wide (one model process serves every profile) and the record is about the hardware, not browsing: no profile's sync, Clear browsing data or Incognito state touches it. The runtime reads it at `Initialize()`, keeps it under its guard lock, and writes it on the UI thread after every change.

**Bounds, and the service's strings.**

- At most 8 entries. When full, the entry not turned off with the oldest event goes first, then the one turned off longest ago. A compromised service can invent keys to push real entries out, but it gets at most two GPU failures a session (the session guard), and it could crash the process anyway.
- A key is numbers, stored and logged as fixed-width lowercase hex (`vk:<vendor>:<device>:<driver id>:<driver version>:<uuid>`, or `unidentified`) and parsed back strictly. A name keeps only `[A-Za-z0-9 ()+-._/,[]]`, at most 64 bytes, and is shown as text. Neither is ever part of a path (the cache file's name is a digest, §11.12), and log lines print the key text and the sanitized name.
- Read back from Local State, everything is checked again: a bad key or a field of the wrong type drops the entry, times ahead of the clock are taken as now, at most three failure times, the stored version only `[0-9.]`.
- Times are wall-clock (the record outlives the session). A clock set back expires nothing early; set forward, a retry comes early (one more attempt).

**What reaches pages: nothing new.** The web AI APIs' device facts (WEB_AI_APIS.md §8.1: inference support, downloaded models, whether a model fits the RAM, the switch) don't include the GPU, and `HardwareCapability::gpu_backend` still says `"none"` on Linux and Windows. A turned-off driver makes answers CPU-fast, like a machine without a usable GPU; token timing already shows the hardware class to allowed sites (WEB_AI_APIS.md §8), and only to them. MoltNet windows have the web AI APIs off, and nothing of the record or the setting is exposed to any window; the settings page shows the GPU's name to the user only.

### 11.11 The setting: "Use the GPU for on-device AI"

A "Graphics processor" section on `molt://ai-settings` (`ui/webui/molt_ai/molt_gpu_inference_handler.{h,cc}`), shown only in builds with GPU inference (its state says `available`, which is `BUILDFLAG(MOLT_LLAMA_VULKAN)`; the section starts `hidden`):

- **Automatic** (default) or **Off**, `molt_ai.gpu_inference.mode` in Local State (0 or 1; any other value is Automatic). Off means the browser never asks for the GPU: the process is launched without `--molt-inference-gpu` and never brings Vulkan up. It applies from the next load.
- For each driver the record turned off: its name, driver version (in the vendor's encoding, `FormatGpuDriverVersion`), the date, and that models run on the processor until the driver changes; "a graphics driver that failed before it could be identified" for the unidentified entry; a note when the session guard turned the GPU off for the session, or `--disable-gpu` is set.
- **Try the GPU again**, shown with those: every turned-off driver goes on probation (§11.10) and the session's GPU failures are forgotten (`GpuFallbackGuard::Reset`), from the next load.

Messages: `gpuInferenceGetState(cb)`, `gpuInferenceSetMode(cb, "auto" | "off")`, `gpuInferenceRetry(cb)`. Every one checks `HasCallbackArgs()`; the mode goes through `BoundedStringArg()` (8 bytes) and only the two values are accepted. The two that change something need a real click: `HasTransientUserActivation()` on the page's frame (the WebUI pipe is frame-associated, so the click's activation reaches the browser first, as in the automation page's trust panel); without it the state comes back with an error and nothing changes. The script builds DOM with `createElement`/`textContent` only and wires its controls through `moltActions()` (radio buttons, not a `<select>`, so the choice is a click in the page).

**Not for Metal.** Considered and left out, because it isn't almost free:

- On macOS the service always runs Metal and ignores `use_gpu`. Off would need a new CPU-only path on Apple Silicon, and counting Metal crashes as GPU failures would change the crash-loop behaviour of the platform that ships today; there is no Mac here to test either.
- The record's key would need a Metal identity (the `MTLDevice`, the OS build as the driver version) that ggml-metal doesn't expose without another patch, and a Metal pipeline cache is a different API (`MTLBinaryArchive`).
- Metal has one driver per macOS release, updated only with the OS, and no failures have been reported. Revisit if some are.

### 11.12 The Vulkan pipeline cache

**When pipelines compile: during the first answer.** ggml-vulkan creates its pipeline objects with the device but compiles each on first use, during graph compute (`ggml_pipeline_request_descriptor_sets()` → `ggml_vk_load_shaders(device, pipeline)`); llama.cpp's context creation only reserves graphs. So a model's pipelines compile in its first answer, not at load (the vision projector's at its first image: its warm-up only reserves the graph). Measured on lavapipe: all 16 pipelines of the test model in the first answer, none in the second or third.

**The patch** (README.chromium; four blocks under `GGML_VULKAN_MOLTBROWSER`): `vk_device_struct` gets a `vk::PipelineCache`, created right after the device in `ggml_vk_get_device()` by `ggml_vk_moltbrowser_create_pipeline_cache()` from data the service set beforehand (`ggml_vk_moltbrowser_set_pipeline_cache_data`), destroyed with the device, and passed to `createComputePipeline()` instead of `VK_NULL_HANDLE`. A driver that rejects the data gets an empty cache; one that can't make a cache gets none (upstream's behaviour). `ggml_vk_moltbrowser_get_pipeline_cache_data()` returns `vkGetPipelineCacheData`. These use vulkan.hpp's result-returning overloads, which don't throw, inside `try`/`catch`, and the service reaches them only through `llama_guard`'s `SetPipelineCacheData`, `PipelineCacheSize` and `GetPipelineCacheData` (the checked list at the top of `llama_engine.cc`).

**The flow.**

1. *At load* (`OnGpuChosen`, §11.10): the browser names the file for the reported device and opens it on its cache sequence. Over 32 MiB, empty, too short, unreadable, or with a header that isn't the device's: deleted, and none is sent. Otherwise the read-only handle goes in the answer (`FLAG_WIN_SHARE_DELETE`, so a later write can replace it).
2. *The service* reads at most `kMaxPipelineCacheBytes` (32 MiB), checks the header again against the device it chose, and only then seeds the device's cache, before the model load creates the device.
3. *After a generation on the GPU* that completed (not cancelled, no GPU failure), the browser asks `Model.GetPipelineCache()`, at most 4 times per process. The engine compares the cache's size with its size after the load or when last sent: no growth, null; over the cap, an empty buffer, after which the browser deletes its copy so the next load starts a cache with only what is used; otherwise the data.
4. *The browser* refuses data over 32 MiB before reading any of it, then copies the rest out of the reply (over 64 KiB a `BigBuffer` is shared memory the service can still write to, so the bytes checked are the bytes stored), refuses a copy whose header isn't the process's reported device (either refusal drops the file, and nothing more is asked of that process; not a protocol violation, since a driver may write an odd cache), then writes it atomically (`ImportantFileWriter`, a temporary file then a rename) and keeps at most 4 files in the directory, deleting the least recently written.

**The header check** (`engine/gpu_pipeline_cache.{h,cc}`, std-only, shared by both sides; the only part of a cache either side reads): `VkPipelineCacheHeaderVersionOne`, least significant byte first: at least 32 bytes and at most the cap; `headerSize` at least 32 and within the data; `headerVersion` 1 (not Vulkan SC's); `vendorID` and `deviceID` the reported device's; `pipelineCacheUUID` the reported one. A device whose `pipelineCacheUUID` is all zeros gets no cache: its driver can't tell its own caches from another build's.

**The file.** `<user data>/MoltAI/GpuCache/vk-<32 hex digits>.bin`, the digits the start of the SHA-256 of `molt-vk-pipeline-cache:1|<driver key>|<pipelineCacheUUID>|<MoltBrowser version>/<Chromium version>` (`runtime/gpu_pipeline_cache_files.{h,cc}`). One file per GPU, driver (version and build), driver cache format, and ggml shader set: the shaders come from the build (llama.cpp's sources, compiled by the glslang of that Chromium), so a new version is a new file and the old one ages out under the 4-file cap. Nothing the service sent becomes a path component; the directory's cap and deletion touch only files with that name form. It is browser-wide, in the user data directory and not a profile, like the record.

**Deleted with on-device AI data.** Deleting a model (`BrowserAIRuntime::DeleteModel`, the model menu's Delete) deletes the whole `GpuCache` directory (it is shared by every model; the next GPU load compiles cold), in order with the cache sequence's writes.

**It holds no user content, checked.** A pipeline cache holds what the driver compiled from the inputs of `vkCreateComputePipelines`: the shader code (SPIR-V from MoltBrowser's binary, the same for every user), the specialization constants (ggml's tile, workgroup and type parameters, some derived from tensor shapes: head sizes, and for flash attention a tuning choice by batch and context size class) and the pipeline layout. Pipeline creation sees no tensor data, no prompt, no page text and no answer. So it says which kinds of model ran (architectures, quantization types, head sizes) and roughly how long their contexts were, which the model files on disk say too; it is deleted with them.

**Threat: a hostile cache for the next load.** A compromised inference process can send any bytes that pass the header check; it controls both its report and the data. The browser stores them, and at the next load of the same device, driver and MoltBrowser version, a fresh inference process hands them to the driver's cache deserializer before anything else. So an attacker who compromised one inference process can attack the next ones, across browser restarts, through code that parses untrusted data (application-provided caches) whose robustness varies by driver. What limits it:

- Same sandbox and privilege: the deserializer runs in the next inference process (`kOnDeviceModelExecution`, the same GPU grants as before). Nothing reaches the browser process: it reads 32 bytes of header and writes bytes to a file it names.
- 32 MiB at most, atomic writes, at most 4 files.
- The header must match the next process's own report, so a file is handed only to the device, driver build and cache format it was written for.
- A driver update, a driver rebuild, a change of the driver's cache format, or a MoltBrowser update makes a new file name; deleting a model deletes all of them.
- Chromium accepts the same for its GPU process: its shader and pipeline caches (`gpu/ipc/host/gpu_disk_cache.h`: `kGlShaders`, `kDawnWebGPU`, `kDawnGraphite`) are blobs the GPU process produces, the browser stores and hands back to the next GPU process.

Removing it would mean keeping no cache: there is nothing the browser can verify about a cache, since its producer is the party in question.

**Pages and MoltNet.** Cache files aren't visible to the web. A warm cache makes the first answer after a load faster, so an allowed site's first-call latency (already a documented gap: whether a model was loaded, WEB_AI_APIS.md §8) can also reflect whether a GPU model ran before on this browser version: the same class of signal, to allowed sites only, never in MoltNet windows (the web AI APIs are off there).

**Measured on lavapipe here** (Mesa 25.2.8 with LLVM 20.1.2; the 0.44 MB test model, three greedy answers of 24 tokens):

| | First answer | `vkCreateComputePipelines`, 16 pipelines | Cache after it |
|---|---|---|---|
| Mesa's own disk cache off (`MESA_SHADER_CACHE_DISABLE`, as in the sandbox, where it is unreachable) | 16.0–19.8 s | 14.7–18.7 s | 32 bytes (the header) |
| The same, seeded with a header-only cache | 19.1 s | 17.6 s | 32 bytes |
| Mesa's own disk cache on and warm (outside a sandbox) | 0.72–0.99 s | 0.48–0.78 s | 32 bytes |
| Emulated caching driver, cold (a miss costs 1 s) | 16.7 s | 16 misses | 164 bytes, stored |
| Emulated caching driver, warm (the stored file) | 0.79 s | 16 hits | unchanged, not written again |

lavapipe never stores compute pipelines in `VkPipelineCache` (its cache stays the 32-byte header after compiling all 16, also with ggml's statistics flag removed in a trial), so on lavapipe the cache saves nothing; it relies on Mesa's on-disk cache, which the sandbox can't reach. The second line is the cost a working cache removes: compiling these 16 pipelines takes 15–19 s on lavapipe and under a second when they come from a cache. The win on RADV, ANV and NVIDIA, which do store pipelines in `VkPipelineCache`, is to be measured (§11.9). The emulated driver (the test layer, below) shows the browser and the service deliver it: the warm first answer is 0.79 s against 16.7 s, with the same text.

### 11.13 How the record, the setting and the cache were tested

**In the repository:**

- `runtime/gpu_driver_record_unittest.cc` (18 tests, std-only): the key's text (exact form, round trip, strict parsing: case, length, separators, a stray byte), names (markup, quotes, controls, non-ASCII, a NUL, 64 bytes), versions, driver version formats (NVIDIA, Intel on Windows, Vulkan's); three failures turn a driver off, only that key, and only failures within 30 days count; retry after 30 days on probation, one failure off again, forgotten after a quiet month; retry after an update but not for an unusable version or one never recorded; the user's retry; the unidentified entry; at most 8 entries under a stream of invented keys, turned-off ones kept, the one turned off longest ago making room when all are; a clock set back; `Restore` (names, failure times, the version, probation, duplicates, the cap, future times, expiry) and its round trip; and the cache files: the key text (every part counts, the shader set filtered, none for an unidentified driver), file names, and the directory cap (oldest first, never the new file, never another file).
- `engine/gpu_pipeline_cache_unittest.cc` (9 tests): a device's own cache, a header alone, a longer header; empty and truncated; the cap; `headerSize` (below 32, zero, exactly the data, one past, huge); the version (Vulkan SC's, 0, 2); another vendor, device, or any byte of the UUID; little-endian fields; no cache for an all-zero UUID.
- `engine/gpu_offload_unittest.cc` (+1): a turned-off device is skipped for the next usable one, or none.
- `runtime/stream_validator_unittest.cc` (+2): `GpuFallbackGuard::Reset`; `IsConsistentGpuStatus` over every status and every combination of what the browser saw.
- `runtime/gpu_inference_prefs_unittest.cc` (5 tests, Chromium build): the prefs' defaults, the mode, the stored form's round trip (a driver version above `INT32_MAX` included), bad entries dropped (wrong types, a hostile key, markup in a name, odd failure times), at most 8 read.
- `runtime/inference_process_host_unittest.cc` (+10, Chromium build; the fake service now reports its device on a GPU load, as the real one does): the report reaches the delegate sanitized, with the skip list sent and the outcome carrying the device; a refused device loads on the CPU with `kTurnedOff`; a crash after a refusal is a model crash; a crash after the report carries the device, one before it none; a GPU status that doesn't fit what the browser saw is a violation, `kDeviceLost` on an allowed device isn't; a second report and an overlong name are violations; the pipeline cache is asked for after GPU generations, at most 4 times, stored when valid, refused when it is another device's or over the cap, dropped when empty; none asked on the CPU or after a GPU failure.
- `tools/webui_checks/webui_smoke_test.js`: the GPU section under the strict CSP: shown when available, hostile driver names stay text, rows of the wrong shape skipped, the calls and their arguments, a refused change shows the saved value and the error, retry, the `--disable-gpu` note, hidden without GPU inference, garbage answers.
- Fuzzers: `molt_gpu_pipeline_cache_fuzzer` (the header check against an independent reading of the header; half the inputs check against the device their own header names, so `kOk` is reached) and `molt_gpu_driver_record_fuzzer` (random sequences of failures, queries, retries and restores with times jumping both ways, checking the bounds, the round trips and the name and file-name forms).

**Run here:**

- The std-only tests above and the engine's suite: clang with ASan and UBSan and GCC `-O2 -Werror` (66 tests), and the engine directory's 170 in the stub GN build with the switch on.
- The compile check (`tools/compile_check/`, Chromium 152's real headers) on every C++ file changed: all clean, with the mojom bindings regenerated. `llama_guard.cc` is reported with errors only because the harness compiles it without exceptions, which GN builds it with.
- The three WebUI checks; `molt_fuzz.py check`; each fuzzer smoke-run and run for 4 minutes without a finding (the header check 57 million inputs, the record 436,000 operation sequences).
- **End to end on lavapipe** (`engine_gpu_e2e sim`: one browser session and one inference process per run, the browser's std-only record, file naming and header checks around a `LlamaEngine` built as the service builds it, the record in a file standing in for Local State; with the test-only Vulkan layer of §11.8, extended to override the reported driver version and pipeline-cache UUID, time pipeline creation, and emulate a driver that stores its pipelines):

  | Case | Result |
  |---|---|
  | Device lost at the first submit, three browser sessions | `kDeviceLost` each time, the model loaded on the CPU in the same process with the CPU's text; the third failure turns the driver off |
  | The next session | the driver in the skip list; `kTurnedOff`, no device chosen or created, the CPU's text |
  | A new driver version (the layer reports another `driverVersion`) | another key: on the GPU |
  | 30 days later | retried on probation, on the GPU; lost again: off again at once |
  | A MoltBrowser update; the user's retry | each retries on the GPU |
  | The setting Off | the GPU not asked for (`kNotRequested`) |
  | Cold, then warm (emulated driver) | cold: 16 misses, the cache returned after the first answer and stored (164 bytes), nothing new after the second; warm: the file sent, seeded before any pipeline, 16 hits, the same text, not written again |
  | A corrupt, truncated (16 bytes), other-vendor or empty file | refused by the browser and deleted, a cold load, replaced by a good cache |
  | Truncated within its entries (the header intact) | sent; the driver ignores what it can't read; replaced |
  | Another device's cache handed to the engine directly | dropped by the engine's own check (the driver gets none) |
  | A reply over the browser's cap; a stored file over it; the service's cap | refused, nothing stored; not sent; "too large", the stored copy dropped |
  | Another `pipelineCacheUUID` (the layer reports one) | another file; the old one not sent |
  | Real lavapipe given a header-only cache, and another driver's entries | accepted, no crash, the same text |

  The previous round's cases (§11.8) give the same results on this build, except that a device lost while loading now reports `kDeviceLost` instead of `kLoadFailed`; an allocation failure is still `kLoadFailed`.
- **Mutation checks:** 49 mutations of the new checks (the header check, the record's threshold, window, retries, probation, caps, eviction and sanitization, the cache files' naming and cap, `IsConsistentGpuStatus`, the session guard's reset, the turned-off verdict) against the std-only tests: 48 killed. The one that survives makes the record's age of a time ahead of the clock negative instead of zero, which every comparison treats alike. One more test was added for a survivor (no update retry against a version that wasn't recorded). Seven mutations of the settings section's script and markup (a name through `innerHTML`, shown without GPU inference, Off never sent, no retry, rows of the wrong shape, the radio not showing the saved value, an inline handler): all caught by the smoke test or the inline-handler check.

**Needs a Chromium build:** the host's and the prefs' unit tests, the settings page in a real renderer (the activation check), Local State written and read back by the real `PrefService`, the mojom with the real generator in the real build, and the hardware of §11.9.

### 11.14 Shader size

Status: implemented (October 2026) behind the same switch; built and run in the stub GN root (§11.8) and on lavapipe; pending a compile in Chromium. §11.9 listed the binary size as left: about 40 MB of SPIR-V in the one browser binary.

**Measured** (156's glslang 16.6.0, every shader family on): 1,569 variants, **40.2 MB of SPIR-V**, 174 MB of generated C++. Per family, before and after:

| Family | Variants | SPIR-V | Kept | Kept SPIR-V | In the binary |
|---|---|---|---|---|---|
| `mul_mm` | 561 | 18.3 MB | 173 | 5.3 MB | 787 KB |
| `mul_mat_vec` | 135 | 3.9 MB | 72 | 2.2 MB | 416 KB |
| `mul_mat_vecq` | 90 | 3.7 MB | 42 | 1.4 MB | 275 KB |
| `mul_mm_cm2` | 76 | 2.2 MB | 32 | 0.9 MB | 128 KB |
| `mul_mmq` | 84 | 1.5 MB | 42 | 0.7 MB | 89 KB |
| `copy_to_quant` | 52 | 1.0 MB | 27 | 0.4 MB | 48 KB |
| `mul_mmq_cm1` | 26 | 0.9 MB | 14 | 0.5 MB | 87 KB |
| `flash_attn` | 10 | 0.8 MB | 10 | 0.8 MB | 136 KB |
| the other 147 | 535 | 7.9 MB | 368 | 3.9 MB | 652 KB |
| **All 155** | **1,569** | **40.2 MB** | **780** | **16.1 MB** | **2.62 MB** |

"In the binary" is the compiled objects' text and data (`size`); the family table adds 1.5 KB.

**What was cut, and why each is safe:**

1. **Weight types no catalogue model uses** (`_ggml_vk_dropped_types` in `src/third_party/llama_cpp/BUILD.gn`, 18 types): `q1_0`, `q2_0`, `q4_0`, `q4_1`, `q2_k`, `q3_k`, the nine `iq*` types, `nvfp4`, `tq1_0` and `tq2_0`; 789 variants, 24.1 MB. The catalogue's chat models are Q4_K_M files (Q4_K and Q6_K, with Q5_0, Q5_1, Q8_0 or F16 for tensors whose rows aren't a multiple of 256), gpt-oss is MXFP4, the embedding model and the projectors are Q8_0, F16 and F32, and KV caches are F16: all kept. A model with a left-out type still runs: `ggml_backend_vk_device_supports_op()` answers false for any op whose result or source has such a type, and llama.cpp's scheduler, weight placement and offload all ask it, so those ops (and their weights) stay on the CPU. Before pinning a model with another type, `tools/models/gguf_types.py` lists its file's types against the list (and fails on a left-out one); take the type out of the list then. The build checks that every listed type is one the generator knows and isn't F32 or F16.
2. **Cooperative matrices: nothing cut.** `cm1` (`VK_KHR_cooperative_matrix`) is what NVIDIA (Turing and later), AMD (RDNA3, RADV and AMDVLK) and Intel (Arc, Xe2, ANV) expose; `cm2` (`VK_NV_cooperative_matrix2`) is NVIDIA's fastest path for prompt processing. ggml chooses per device at run time, so dropping either would make those GPUs slower, not unable; after the type cut they are 1.4 MB of SPIR-V and 215 KB in the binary.
3. **Duplicates.** 117 groups of identical variants (2.8 MB): within a family, identical variants share their bytes in a block.
4. **Compression.** brotli (Chromium's `//third_party/brotli`, quality 11, 16 MB window), per family, in blocks of up to 256 KB of whole variants (a larger variant gets its own block, at most 4 MB). Measured on all 1,569 variants:

   | Scheme | Size |
   |---|---|
   | zlib, each variant | 13.3 MB |
   | brotli, each distinct variant | 8.7 MB |
   | brotli, one stream per family | 4.4 MB (but one pipeline would inflate up to 18 MB) |
   | brotli, blocks of 128 KB / 256 KB / 512 KB / 2 MB | 6.6 / 5.95 / 5.3 / 4.8 MB |
   | brotli with a dictionary per family | 6.7 to 7.7 MB |

   256 KB keeps what one pipeline inflates small. Inflating every kept variant once took 0.64 s on one core (under a millisecond each), next to the driver's own compile of the same SPIR-V; pipelines are made on first use, so a model inflates only the variants it uses.

**How** (README.chromium has the patch list):

- The generator writes its `.cpp` files to `raw/` as before; a host tool, `ggml_vk_shader_pack` (`chromium/ggml_vk_shader_pack.cc`), rewrites each into the file that is compiled: a left-out variant's `_len` is 0, every other variant's `_data` is a 24-byte descriptor (magic `MVKZ`, family, block, offset, size, FNV-1a checksum of its SPIR-V), and the blocks follow. The symbol names don't change, so `ggml-vulkan.cpp`'s references don't either. The tool fails on any generator output it doesn't recognize, and inflates every block and checks every descriptor before writing. A second run writes the family table and the left-out list (`ggml-vulkan-shader-registry.cpp`).
- `ggml-vulkan.cpp`, two blocks under `GGML_VULKAN_MOLTBROWSER`: `ggml_vk_create_pipeline_func()` inflates the variant before making the shader module (`ggml_vk_moltbrowser_unpack_shader()`), and `supports_op` asks `ggml_vk_moltbrowser_op_types_built()`.
- The service checks each descriptor (magic, family, block, bounds, a size that is a multiple of 4 and at most 4 MB), the inflated block's size, the SPIR-V magic and the checksum. Anything else, and a left-out variant (which `supports_op` makes unreachable), throws inside ggml-vulkan, which `llama_guard` turns into `kBackendFailed` (§11.6): the model reloads on the CPU. The checksum guards against a packing bug, not an attacker (the data is in the binary).
- Side effect: the generated C++ the build compiles drops from 174 MB to 13.4 MB (§11.8's `mul_mm.comp.cpp` alone was 79 MB and needed 2.4 GB of compiler memory).

**Results unchanged, how it was checked:**

- *Byte for byte.* Every one of the 780 kept variants inflates to exactly the bytes the generator wrote, and each of the 789 left out has a left-out type in its name (a scratch program over the build's output). So for every kept type the GPU runs exactly the shaders it ran before; no "before" run is needed to compare against.
- *Unit tests* (`vulkan_shader_pack_unittest.cc`, 9, with the switch): descriptors round-trip and malformed ones are refused, the checksum, every block inflates, kept variants of several families inflate to SPIR-V, left-out ones are empty and refused, the left-out list are ggml types, and the op gate on kept and left-out types.
- *CPU against lavapipe*, `LlamaEngine` built as the service builds it, tiny random-weight llama models whose matrices are all of one type, three greedy turns of 16 tokens each, and llama.cpp's last-token logits:

  | Weights | On the GPU | Text, three turns | Logits: largest difference, cosine, top token |
  |---|---|---|---|
  | F32 | every layer | identical | 0.050, 0.9999982, same |
  | Q4_K, Q5_K, Q6_K | every layer | identical | 0.008 / 0.010 / 0.018, at least 0.9999, same |
  | F16, Q8_0, Q5_0, Q5_1 | every layer | the same first tokens, then apart at a near-tie | 0.35 to 0.58, at least 0.9997, same |
  | Q4_0, Q4_1 (left out) | every layer, their products on the CPU | the same first tokens, then apart | 0.56 / 0.67, at least 0.9997, same |

  The CPU multiplies quantized weights with 8-bit activations and the GPU doesn't, so a random model's greedy text parts where two tokens are nearly tied; the logits agree on every type, and on the types without a tie in the run the text is identical. The left-out types run, with their matrix products on the CPU.
- *GN*: `gn gen --check` with the switch on and off; the build from scratch with `-j1`.

**Needs a Chromium build:** the compile with the real toolchains, the browser binary's size with the switch on, and a catalogue model on a real GPU (§11.9).

### 11.15 Windows: loading the GPU drivers before the sandbox locks down

Status: implemented behind the same switch, which stays off in `configs/windows-x64.gn`; type-checked for Windows; pending a Windows machine with a GPU (Windows builds are paused, CLAUDE.md). §11.4 explains why: once this sandbox type locks down, only Microsoft-signed images may be mapped, and neither the Vulkan loader nor a GPU vendor's driver is one.

**Where.** `content/utility/utility_main.cc` (imported from 156 unchanged, then hooked): after webnn's pre-sandbox step and before the token is lowered, a process whose sandbox type is `kOnDeviceModelExecution` calls `on_device_model::MaybeWarmMoltGpuInferenceDrivers()` (`content/utility/on_device_model/molt_gpu_inference_sandbox_win.cc`). It does nothing without `--molt-inference-gpu`, which the browser adds only to inference processes it launches for the GPU (§11.4), and nothing in a build without the switch (`molt_gpu_inference_buildflags.h`). The CPU path and every other utility process are unchanged.

**What it does, and why each step:**

1. *Layers out.* It clears the environment variables that name Vulkan layers or layer paths (`VK_INSTANCE_LAYERS`, `VK_LOADER_LAYERS_ENABLE`, `VK_LAYER_PATH`, `VK_ADD_LAYER_PATH`, `VK_IMPLICIT_LAYER_PATH`, `VK_ADD_IMPLICIT_LAYER_PATH`) and sets `VK_LOADER_LAYERS_DISABLE=~all~`. Overlays and capture tools register implicit layers in the registry, which this sandbox can read (unlike Linux, where their manifests aren't readable, §11.4); without this they would be loaded and pinned too. Loaders before 1.3.234 ignore the variable.
2. *The loader from System32 only.* `LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)`: the loader the GPU driver installs, never one from the application directory, the current directory or `PATH`. ggml-vulkan later finds it already loaded (`GetModuleHandleW` first, `chromium/ggml-vulkan-moltbrowser.cpp`) and otherwise searches System32 only, too.
3. *Instance, devices, pipelines* (`WarmUpVulkanDrivers()`, `molt_gpu_driver_warmup.cc`, platform-independent): an instance (Vulkan 1.0, no layers, no extensions); up to 16 physical devices; for each discrete or integrated GPU (never CPU, virtual or "other" types) a device with one compute queue, a 55-word compute shader (an empty `main`, compiled with glslang 16.6.0 for Vulkan 1.0), its pipeline layout and a compute pipeline. Creating the device loads the vendor's user-mode driver; creating a pipeline loads its shader compiler, which several drivers load only then.
4. *Pin.* While all of that is alive, every module loaded since a snapshot taken before step 2 is pinned (`GetModuleHandleExW` with `GET_MODULE_HANDLE_EX_FLAG_PIN`): the loader, the drivers and what they loaded stay mapped for the life of the process. Then everything is destroyed in reverse order; without the pin, destroying the instance would unload the drivers.
5. *One log line* for the checklist: `On-device AI GPU warm-up: instance 1, GPUs N, devices N, pipelines N, modules pinned M, not pinned 0`. With no loader, a warning, and the model runs on the CPU.

After lockdown, ggml-vulkan's own instance loads the same modules, which are already mapped, so nothing new needs mapping.

**What it allows, and what not.** The sandbox policy doesn't change: the LPAC, the token, the job and the mitigations are upstream's for this type. The step only maps, before lockdown, what the System32 Vulkan loader and the installed GPU drivers load for their own devices, in a process the browser launched for the GPU; one process per model, so they stay only as long as that model. No page or model input reaches it: the names and the shader are fixed, and it runs before the process serves any IPC. Anything the drivers load later is still refused (that load fails, which is a GPU failure, §11.5: the model moves to the CPU). Third-party layers are kept out.

**Rejected:** `ServiceProcessHost::Options::WithPreloadedLibraries` (PassKey-gated, and it loads named DLLs, while the driver's names come from the registry at run time); turning the signed-binaries mitigation off for this type (a sandbox policy change in upstream code, for every model process); upstream's Dawn warm-up (D3D12 on Windows).

**Checked here:** the warm-up function's unit tests (8, a fake Vulkan; GCC and ASan); a real run on lavapipe (instance, device and pipeline made, the callback run while they live, 38 new shared objects reported); and a Windows type check of `molt_gpu_inference_sandbox_win.cc` and the warm-up (Chromium's clang and libc++ on the mingw-w64 headers, stand-ins for the three `//base` declarations it uses, `-Werror` with Chromium's warnings, the switch on and off).

**Only a Windows machine with a GPU can confirm** (the test plan is in `docs/BUILD_MACHINE_CHECKLIST.md`, items tagged "ai-finish"): that the driver files in the DriverStore are readable from this LPAC before lockdown; that the loader's adapter discovery (D3DKMT) and the drivers' registry reads work there; which modules each vendor loads at device and at pipeline creation, and whether any loads more later (a refused load after lockdown); that the pinned modules let ggml-vulkan's instance and devices come up after `LowerToken()`; that no non-Microsoft image is refused (Process Monitor: no `STATUS_INVALID_IMAGE_HASH`); NVIDIA, AMD and Intel, a laptop with two GPUs, and a machine without a Vulkan driver (the CPU).

## 12. CPU kernels for x86-64 CPUs without AVX2 (run-time choice)

Status: implemented (October 2026), pending a compile on the build machine and a run on real pre-AVX2 hardware (§12.10). §12.1–12.7 are the research and the decision, §12.8 what was built, §12.9 how it was tested, §12.10 what is left. On x64, `//third_party/llama_cpp` built ggml's x86 kernels with `-mavx2 -mfma -mf16c`, so on a CPU without AVX2 or FMA (Intel Core before Haswell, 2013; AMD before Excavator, 2015; Atom, Celeron and Pentium parts sold until about 2021; VMs that hide AVX) the browser refused on-device AI before launching the service, and the service checked again. The goal: those CPUs run models on the CPU with the best kernels each supports; AVX2 machines run exactly what they ran before. macOS is arm64 with Metal and isn't affected; this is x64 Linux and Windows. llama.cpp is the vendored b11641; Chromium files were read at 152.0.7977.169.

### 12.1 Findings in one page

1. **Only two translation units need AVX2.** BUILD.gn gives the AVX2 flags to `arch/x86/quants.c` and `arch/x86/repack.cpp` (and `arch/x86/cpu-feats.cpp`, which compiles to nothing here). Every other ggml-cpu file is built for Chromium's x64 baseline, `-msse3` (`build/config/compiler_cpu_abi.gn:58`, `build/config/win/BUILD.gn:123`). The two files define 40 functions: 28 quantized dot products and activation quantizers, and 12 repacked-matmul kernels.
2. **Side effect found on the way: no weight repacking on x64.** `ggml_cpu_has_avx2()` is a compile-time answer (`ggml-cpu.c:3657`, `#if defined(__AVX2__)`), and `ggml-cpu.c` is built without `-mavx2`, so it returns 0. Repacking (`ggml_repack_get_optimal_repack_type`, `repack.cpp:4975`) therefore never picks a layout on x64: the 12 repack kernels are dead code there, and the `cpu_repack_used` the service reports means "allowed", not "used". F32 and F16 operations (`vec.cpp`, `ops.cpp`: norms, softmax, RoPE, the F16 KV cache) run their SSE3 paths. §2.5's memory figures for repacking describe upstream's x64 behaviour, not this build's. Changing any of this would change what AVX2 machines run, so it is left as it is (§12.10).
3. **Upstream's answer is one shared library per variant.** `GGML_CPU_ALL_VARIANTS` with `GGML_BACKEND_DL` (§12.3) builds the whole CPU backend 14 times on x86 and loads the best one with `dlopen`. In our sandbox that needs ggml itself as a shared library, loading before the sandbox on Linux and before lockdown on Windows (two upstream hooks, the Windows one untestable without a Windows build), and more signed files to ship (§12.4).
4. **Linking whole backends side by side is unsafe.** One CPU backend defines about 400 external symbols plus 209 weak (COMDAT) ones, C++ template code and vtables that the linker merges across variants silently, which can hand a non-AVX caller AVX2 code (§12.5).
5. **Linking only the two ISA-specific files per variant is small and safe.** Their 40 symbols are plain C functions (no data, no weak symbols, at `-O0` and `-O2`, libstdc++ and libc++). A force-included header renames them per variant; a dispatch file keeps the original names as thunks that jump through the chosen variant's table, so nothing in ggml or llama.cpp changes (§12.5).
6. **Three variants:** `x86-64-v2` (SSE4.2 and POPCNT, no AVX), `avx` (Sandy Bridge and Ivy Bridge; AMD Bulldozer to Steamroller, Jaguar) and `avx2` (AVX2, FMA, F16C: today's flags). They cost about 5, 58 and 105 KB of code. No AVX-512 variant: in these files it only replaces two instructions of an int8 helper with one VNNI instruction and packs nibbles, and the AVX-512 repack paths are never chosen (point 2) (§12.6).
7. **The choice is a pure function** of the CPUID and XCR0 bits, scored like upstream's `cpu-feats.cpp` but also checking that the OS saves the AVX registers (upstream's "FIXME"). The service applies it before `llama_backend_init()` and before anything computes. Only CPUs below x86-64-v2 are still refused: Intel Core 2 and older, AMD before Bulldozer (Phenom, Athlon II), Atom before Silvermont (§12.7).

### 12.2 What needs AVX2 today

| File (BUILD.gn target) | Flags | What it defines |
|---|---|---|
| `ggml-cpu/arch/x86/quants.c` (`:ggml_cpu_arch_x86_c`) | `-mavx2 -mfma -mf16c -msse4.2` | `quantize_row_q8_0`, `_q8_1`, `_q8_K` (activations, the `from_float` of the quantized types) and 25 `ggml_vec_dot_*` kernels (Q1_0 to Q8_0, the K-quants, the IQ types, MXFP4, NVFP4, TQ1_0, TQ2_0). Each has an `__AVX2__` path; most also have an `__AVX__` path, two have an `__SSSE3__` path, and the rest fall back to the `_generic` C versions in `ggml-cpu/quants.c`. |
| `ggml-cpu/arch/x86/repack.cpp` (`:ggml_cpu_arch_x86`) | same | 12 kernels for repacked weights: `ggml_quantize_mat_q8_0_4x8`, `_q8_K_4x8`, and `ggml_gemv_*`/`ggml_gemm_*` for Q4_0, Q4_K, Q2_K, IQ4_NL and MXFP4 at 8x8. |
| `ggml-cpu/arch/x86/cpu-feats.cpp` (`:ggml_cpu_arch_x86`) | same | Nothing: its score function is exported only with `GGML_BACKEND_DL`. |
| everything else in `ggml-cpu/` | Chromium's `-msse3` | the backend, graph compute, operators, F32/F16 vector code, the generic quantized kernels. |

Who calls the 40: `ggml-cpu.c`'s `type_traits_cpu` table (31 entries: `from_float` and `vec_dot` per type), `ggml-cpu/repack.cpp` (12 calls in its kernel templates), and `amx/mmq.cpp` (2 calls, compiled out: AMX needs `__AMX_INT8__` and `__AVX512VNNI__` at compile time). `src/` and `tools/mtmd` call none of them.

### 12.3 Upstream: one shared library per variant

- `GGML_CPU_ALL_VARIANTS` (`ggml/src/CMakeLists.txt:489`) calls `ggml_add_cpu_backend_variant` for 14 x86 levels: x64, sse42, sandybridge, ivybridge, piledriver, haswell, skylakex, cannonlake, cascadelake, icelake, cooperlake, zen4, alderlake, sapphirerapids (five of them not with MSVC). Each is a complete CPU backend, all 17 ggml-cpu files compiled with that level's flags (`ggml-cpu/CMakeLists.txt:20-60, 345-417`), as `libggml-cpu-<name>.so` / `ggml-cpu-<name>.dll`.
- It requires `GGML_BACKEND_DL`, which requires `BUILD_SHARED_LIBS`: ggml's core (`ggml-base`) is a shared library the variants link against.
- The feature check, `arch/x86/cpu-feats.cpp`, is compiled separately without the level's flags and without LTO (so no AVX code can be inlined into it) and exports `ggml_backend_score()`: 0 if a required feature is missing, otherwise a sum of feature weights (FMA 1, F16C 2, SSE4.2 4, BMI2 8, AVX 16, AVX2 32, AVX-VNNI 64, AVX-512 128, ...). It reads CPUID only; "FIXME: this does not check for OS support".
- `ggml_backend_load_best("cpu")` (`ggml-backend-reg.cpp:481`) lists the executable's directory and the current directory, `dlopen`s every `ggml-cpu-*` library, calls its score, and registers the best.
- MoltBrowser already ships this layout once: the Windows Whisper helper (`release-windows-selfhosted.yml`) bundles upstream's `ggml.dll`, `ggml-base.dll` and `ggml-cpu-*.dll`. That is a separate, unsandboxed helper process with its own copy of ggml, which is why the layout works there.

### 12.4 Loading a variant library in the sandbox

- **One copy of ggml.** The chrome binary links ggml statically with hidden visibility, so a variant library can't resolve ggml's symbols from it. Either ggml (and llama.cpp, which calls it) becomes a shared library the browser binary depends on, or every variant carries its own ggml core: two sets of ggml globals in one process (the log callback, the FP16 tables, the backend registry), which upstream doesn't support.
- **Linux:** the broker (`FilePermissionsForGpu`) doesn't let the sandboxed process open or list the install directory, so the library must be opened in the pre-sandbox hook, by an explicit path chosen there (not `ggml_backend_load_best`'s directory scan), with `RTLD_NODELETE`, as `LoadLibrariesForGpu` does for Vulkan. That is a second MoltBrowser hook in `on_device_model_sandbox_init.cc` (§11.7 added the first).
- **Windows:** this sandbox type applies `MITIGATION_FORCE_MS_SIGNED_BINS` at `LowerToken()` (§11.4), and our DLL isn't Microsoft-signed, so it must be loaded before lockdown: `ServiceProcessHost::Options::WithPreloadedLibraries` is PassKey-gated, so an upstream content patch, the same open problem as Vulkan on Windows (§11.4). Windows builds are paused, so it can't be tested.
- **Packaging and signing:** every variant is another file per platform in the installer, the portable ZIP, the Linux packages and the update payload, and on Windows another Authenticode signature (`release-win.sh` signs the executables). A missing or stale variant file means a failed or slow load at run time, not a build error.
- **CFI:** the official Linux build's indirect-call checks don't cross library boundaries; ggml's own calls are exempt already (`cfi_icall_disable`), so this alone isn't a blocker.

### 12.5 Linking every variant into one binary

Measured here with clang 18 at `-O2` (whole backend with upstream's haswell flags):

| What is duplicated | External symbols | Weak (COMDAT) symbols | Code per variant |
|---|---|---|---|
| The whole CPU backend (17 files) | 400 (327 C, 73 C++), including 5 global tables | 209: C++ typeinfo and vtables of `ggml::cpu::repack` templates, template code | about 1.16 MB |
| Only `arch/x86/quants.c` and `arch/x86/repack.cpp` | 40 C functions, no data | none (also at `-O0`, with libstdc++ and libc++) | x86-64-v2 5 KB, avx 58 KB, avx2 105 KB |

Ways to rename the duplicated symbols:

- **A prefix macro per variant:** a header of `#define name prefix_name` lines, force-included into the variant's files. Works for C function names; it can't rename C++ template instances, vtables or the `std::` code a C++ file instantiates.
- **`objcopy --prefix-symbols`:** Chromium's official Linux and Windows builds use ThinLTO, so object files are LLVM bitcode, which objcopy can't rewrite; it would also need a post-compile step GN has no hook for (a `source_set`'s object paths aren't visible to GN).
- **A namespace per variant via `-D`:** `ggml-cpu.c` and `quants.c` are C.
- **The hazard of weak symbols:** a function the linker sees in several objects as COMDAT (an inline function, a template, a vtable) is kept once, from whichever object it picks. If that copy came from a file built with `-mavx2`, every caller gets AVX2 code, including the x86-64-v2 variant, and a CPU without AVX2 dies with SIGILL. Nothing warns about it.

So the whole-backend variants would need a rename scheme the C++ half doesn't allow, and would be unsafe without one. The two ISA-specific files have none of these problems: 40 C names, no weak symbols, a prefix macro covers them. A mismatch between the rename list and the dispatch list is a link error: a function missing from the rename list is defined three times (duplicate symbol), one missing from the dispatch list leaves its callers unresolved. The hazard of weak symbols is checked by listing the variant objects' symbols (§12.9).

### 12.6 The variants and their cost

| Variant | Flags (`quants.c`, `repack.cpp`) | Required at run time | CPUs | Kernels |
|---|---|---|---|---|
| `x86-64-v2` | `-msse4.2 -mpopcnt` | SSE3, SSSE3, SSE4.1, SSE4.2, POPCNT | Intel Nehalem and Westmere (2008-2010); Atom from Silvermont to Tremont (2013-2021) and the Celeron and Pentium parts built on them or sold without AVX; VMs that hide AVX | SSSE3 for Q4_0 and Q1_0; the generic C code for the rest (no SSE paths upstream) |
| `avx` | `-mavx -msse4.2 -mpopcnt` | the above and AVX with OS support (XCR0 bits 1-2) | Intel Sandy Bridge and Ivy Bridge (2011-2012); AMD Bulldozer to Steamroller, Jaguar and Puma | the `__AVX__` paths of most kernels, every K-quant, Q4_0, Q8_0 and MXFP4 among them |
| `avx2` | `-mavx2 -mfma -mf16c -msse4.2` (unchanged) | the above and AVX2, FMA, F16C | Intel Haswell (2013) and later, AMD Excavator (2015) and later, Zen | unchanged |

- **Why an `avx` level:** most quantized kernels upstream have AVX2, AVX and generic paths and no SSE path, and the catalog's models are Q4_K_M (Q4_K and Q6_K), Q8_0 and MXFP4. Sandy and Ivy Bridge are the most common CPUs without AVX2 still in use, and without this level they would run the generic C code. It costs one more copy of two files.
- **Why no F16C in `avx`:** Sandy Bridge and Bulldozer lack it, and in these two files it only speeds up the scalar FP16 conversions of block scales.
- **Why no AVX-512 level:** in `quants.c` it only turns the int8 helper of the Q4_0, Q5 and Q8_0 kernels (`mul_sum_us8_pairs_float`: two instructions) into one VNNI instruction, and packs nibbles with a 512-bit-only instruction; the AVX-512 repack kernels are never chosen in this build (§12.1, point 2). The gain is small, the frequency drop of 512-bit code on older Xeons is not. A `vnni` level (AVX-512-VNNI or AVX-VNNI on top of `avx2`) can be added later as one more row here.
- **Why not upstream's finer levels (ivybridge, piledriver):** the extra F16C/FMA paths in these two files are minor; three levels cover every x86-64-v2 CPU with the best large kernel family it has.

### 12.7 Choosing at run time

- **Where:** in the inference service, in `LlamaEngine::InitBackendOnce()` (the chat and the embedding engine both call it), before `llama_backend_init()` and before any ggml computation. No other process runs ggml.
- **How ggml sees it:** it doesn't. ggml keeps one statically registered CPU backend (`GGML_USE_CPU`), which llama.cpp finds by type (`ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_CPU)`, `llama-model.cpp:1154`, `llama-context.cpp:442`), and the Vulkan path's explicit device list (§11.5) holds only the GPU. The choice is made below the backend, at the level of the 40 kernels.
- **The function:** pure, std-only, unit-tested: CPUID leaf 1 ECX (SSE3, SSSE3, SSE4.1, SSE4.2, POPCNT, FMA, XSAVE, OSXSAVE, AVX, F16C), leaf 7 EBX (AVX2) and XCR0 in, the best variant whose requirements all hold out, scored like `cpu-feats.cpp`. AVX, AVX2, FMA and F16C count only when the OS saves the YMM registers (OSXSAVE set and XCR0 bits 1 and 2), as `base::CPU` checks: a VM or a kernel that leaves AVX state off would otherwise crash on the first AVX instruction. Below x86-64-v2 it returns nothing, and the service answers `kUnsupportedCpu`.
- **The browser** refuses only CPUs below x86-64-v2 (`base::CPU`: SSE3, SSSE3, SSE4.1, SSE4.2, POPCNT), with the same kind of message as before, naming SSE4.2.
- **Default:** before the choice is applied, the thunks use the `x86-64-v2` table, so a forgotten call costs speed, never a crash.
- **Cost on AVX2 machines:** the same kernels, compiled from the same source with the same flags; each kernel call goes through one more indirect jump. A call computes a whole row (thousands of multiply-adds), so the jump is noise; measured in §12.9.

### 12.8 What was built

```
browser                                        inference service (kOnDeviceModelExecution)
-------                                        -------------------------------------------
BrowserAIRuntime, EmbeddingModelManager:       LoadModel / LoadEmbeddingModel
  base::CPU: SSE3 SSSE3 SSE4.1 SSE4.2 POPCNT?    LlamaEngine::CpuSupported()? else kUnsupportedCpu
  no: "This CPU doesn't support SSE4.2, ..."     LlamaEngine::InitBackendOnce():
      (nothing is launched)                        level = ChooseX86CpuKernels(ReadX86CpuInfo())
                                                   molt_llama::UseX86CpuKernels(level)
                                                   llama_backend_init() (once)
                                                 ggml-cpu.c's type traits, e.g. Q4_K's vec_dot:
                                                   ggml_vec_dot_q4_K_q8_K (thunk)
                                                     -> molt_x86_<level>_ggml_vec_dot_q4_K_q8_K
```

**GN** (`src/third_party/llama_cpp/BUILD.gn`, x64 only: Linux, Windows, and the x64 half of a universal macOS build):

- `_ggml_x86_levels`: `v2` (`-msse4.2 -mpopcnt`), `avx` (`-mavx -msse4.2 -mpopcnt`), `avx2` (`-mavx2 -mfma -mf16c -msse4.2`, unchanged).
- `:ggml_cpu_x86_<level>`: `arch/x86/quants.c` and `arch/x86/repack.cpp`, unmodified, with the level's flags, `GGML_CPU_X86_VARIANT_PREFIX=molt_x86_<level>_`, and `chromium/ggml-cpu-x86-variants.h` force-included (`-include`; `/FI` with clang-cl). Visible only to the dispatch.
- `:ggml_cpu_x86_dispatch`: the thunks and `molt_llama::UseX86CpuKernels()`, with Chromium's baseline flags; its public config defines `GGML_MOLTBROWSER_X86_CPU_KERNELS` for the service. It replaces `:ggml_cpu_arch_x86` and `:ggml_cpu_arch_x86_c` in `:ggml`.
- `arch/x86/cpu-feats.cpp` is no longer built (it compiled to nothing).

**llama.cpp:** no vendored file changed. New MoltBrowser files in `chromium/` (README.chromium):

- `ggml-cpu-x86-variants.h`: the 40 names twice, as an X-macro list with each function's kind of signature (for the dispatch) and as `#define name prefix##name` renames (for the level builds). How a mismatch fails: §12.5.
- `ggml-cpu-x86-dispatch.{h,cpp}`: the level enum; one constant table of 40 function pointers per level, their element types taken with `decltype` from upstream's own declarations (`ggml-cpu/quants.h`, `ggml-cpu/repack.h`), so a changed upstream signature doesn't compile; a `constinit std::atomic` pointer to the table in use, the x86-64-v2 one until the service chooses; and the 40 thunks under ggml's names, each one load and an indirect jump.

**Service** (`src/chrome/services/molt_ai/inference/`):

- `engine/cpu_kernels.{h,cc}` (std-only, `:cpu_kernels`): `ChooseX86CpuKernels()` (pure), `ReadX86CpuInfo()` (CPUID leaves 0, 1 and 7, and XGETBV only with XSAVE and OSXSAVE, as `base::CPU` does), `HostX86CpuKernels()` (read once per process), `X86CpuKernelsName()`.
- `LlamaEngine`: `CpuSupported()`; `InitBackendOnce()` now returns false below x86-64-v2 and otherwise selects the level before `llama_backend_init()`, at every load (one store; a test can change the level between loads); `CpuKernelsName()`; `SetCpuKernelsForTesting()`. Both engines' `Load()` report a refused CPU as the new `EngineLoadStatus::kUnsupportedCpu`; the chat engine's description carries the level (`cpu_kernels`).
- `MoltInferenceService`: `CpuSupported()` asks the engine instead of `base::CPU` (no `base/cpu.h` any more), `kUnsupportedCpu` maps to the mojom status of the same name, and the level is logged at `--v=1`.

**Browser:** `BrowserAIRuntime::ComputeUnsupportedReason()` and `EmbeddingModelManager::NotOfferedReason()` ask `base::CPU` for SSE3, SSSE3, SSE4.1, SSE4.2 and POPCNT instead of AVX2 and FMA; the message there and for a `kUnsupportedCpu` load says "This CPU doesn't support SSE4.2, which on-device AI in MoltBrowser needs. Use a cloud model instead (AI Settings > Connect AI Providers)." The mojom changed only in a comment; the level isn't sent to the browser (§12.10).

### 12.9 How it was tested

This machine has no Chromium checkout. Its CPU has AVX2 and AVX-512, so every level runs natively, and qemu-user 8.2 emulates older CPUs: it raises SIGILL for any instruction the emulated model lacks, VEX-encoded scalar SSE included (checked first with a probe of each instruction family on Nehalem, Sandy Bridge, Ivy Bridge and Haswell).

**In the repository:**

- `engine/cpu_kernels_unittest.cc` (7 tests, in `molt_ai_unittests`): no CPUID, Core 2, AMD K10 and each missing x86-64-v2 feature are refused, also with AVX2 present; Nehalem gets x86-64-v2, Sandy Bridge, Ivy Bridge and Piledriver `avx`, Haswell and Skylake-X `avx2`; AVX without OS support (no YMM or no SSE state in XCR0, no OSXSAVE, no XSAVE) falls back to x86-64-v2; the AVX2 level needs FMA, F16C and leaf 7; the host's reading is consistent.
- `x86_cpu_kernels_unittest.cc` (2 tests, in `molt_ai_unittests` on x64): for the 25 quantized types the levels implement a dot product for, each level this CPU runs gives the x86-64-v2 level's result on the same inputs within 1e-6 of the sum of the products' magnitudes (measured: at most 1.4e-8; 5 of the 50 results bit-identical, so the levels do run different code), and with its own activation quantizer stays within 1e-2 of the float result (measured: 5e-4). The IQ types get random blocks instead of ggml's quantizer, whose search tables take seconds to build. 9 ms.

**Run here:**

- *GN.* The stub source root of §11.8, holding this worktree's `//third_party/llama_cpp` and the engine: `gn gen --check` passes with Vulkan off and on, and the test programs build with ninja at `-O1` and `-O2`.
- *Objects* (each level, at `-O1` and `-O2`): exactly the 40 functions, all with the level's prefix; no weak symbol, no data, no initializer section. `x86-64-v2` contains no VEX- or EVEX-encoded instruction and no YMM or ZMM register; `avx` no FMA, F16C or 256-bit integer instruction. The `avx2` level's code is instruction for instruction the old single build's (the same command line without the rename, disassembled and compared). A linked program defines each of the 40 names once (the thunk) and the 120 level functions.
- *Unit tests:* the engine directory's std-only suite (160 tests) and the two above pass; `cpu_kernels` also with GCC `-Werror`, clang ASan + UBSan, and libc++ `-fno-exceptions -fno-rtti -Wunsafe-buffer-usage`.
- *End to end:* `LlamaEngine` built as the service builds it, three greedy chat turns of 24 tokens with KV reuse, and one prompt's last-token logits, with each level forced (`SetCpuKernelsForTesting`) and with the CPU's own choice. 22 models: random llama models (n_embd 1024, n_ff 2816, 4 layers) quantized to Q4_K_M, Q4_0, Q8_0, Q6_K, Q2_K, IQ4_NL, Q5_K_M and Q3_K_M, flat ("random") and confident ("peaked": the output matrix is the token embeddings permuted, so each token has a clear successor, as in a real model, while every layer still runs); and llama.cpp's tiny llama, Qwen3 and a ChatML model at Q8_0 and Q4_0. No real model could be downloaded here.

  | Models | Text, every level vs `avx2` | Logits vs `avx2` |
  |---|---|---|
  | peaked, 8 types | identical, all three turns | at most 0.0032 apart on a range of 17 (1.9e-4); cosine ≥ 0.9999997; same top token, by a margin of about 14 |
  | random, 8 types | identical for 2; the others part at a near-tie, mostly in the second or third turn | at most 0.058 apart on a range of about 5.5 (1.1%); cosine ≥ 0.99979; same top token |
  | tiny, 6 | identical, except tiny-chatml Q4_0 on x86-64-v2 | within 1e-7, except that one: 0.016 on 2.1 |

  The CPU's own choice was `avx2` here and gave the `avx2` texts. Why the levels differ at all: before every matrix product ggml quantizes the activations to 8 bits, and upstream's AVX quantizer (shared by `avx` and `avx2`) rounds half to even and multiplies by 127/max, where the C one used by x86-64-v2 rounds half away from zero and multiplies by 1/(max/127); AVX2 also fuses multiply-adds. A value that rounds the other way moves one activation by one step, and in a random network such steps grow layer by layer. A confident model absorbs them; a flat one, where the two best tokens are 0.02 apart, picks the other one now and then. Upstream's CPU variants differ the same way; AVX2 machines don't, as they run the same code as before.
- *Old CPUs under qemu* (the engine as above, the CPU's own choice; six models):

  | Emulated CPU | Result |
  |---|---|
  | Penryn (Core 2: no SSE4.2) | load refused with `kUnsupportedCpu`, before any llama.cpp call; no fault |
  | Nehalem (x86-64-v2, no AVX) | `x86-64-v2` chosen, every run completes: no AVX instruction executed. Texts and logits bit-identical to the native run with `x86-64-v2` forced |
  | Sandy Bridge (AVX, no AVX2, FMA, F16C) | `avx` chosen, runs complete, bit-identical to the native `avx` runs |

  Negative controls: forcing `avx` on Nehalem, or `avx2` on Nehalem or Sandy Bridge, dies with SIGILL at the first product. The emulated CPUID results also check the choice on Westmere, Ivy Bridge, Opteron G4 (Bulldozer) and G5 (Piledriver), Haswell, Skylake-Server and EPYC, and on Haswell with XSAVE, FMA or F16C removed (x86-64-v2, `avx`, `avx`).
- *Speed* (`-O2`, on this machine's CPU):

  | Model | Old build | `avx2` | `avx` | `x86-64-v2` |
  |---|---|---|---|---|
  | Q4_K_M | 437 / 194 | 421 / 193 | 325 / 125 | 67 / 50 |
  | Q8_0 | 405 / 94 | 400 / 97 | 319 / 86 | 111 / 63 |
  | Q4_0 | 359 / 190 | 342 / 166 | 288 / 147 | 233 / 152 |
  | IQ4_NL | 364 / 189 | 362 / 179 | 282 / 137 | 42 / 28 |

  Prompt / generation tokens per second, one pinned thread, a 64-token prompt and 32 generated tokens, the median of three rounds (each the median of seven runs) of the peaked models. The old build (the base commit's llama.cpp, built the same way) and `avx2` run the same kernels; their differences are this shared machine's noise (rounds of one configuration differ by up to 30% in generation). The thunks' cost, measured without that noise: 6 instructions per kernel call with frame pointers, +0.75% to +0.87% of all instructions executed (callgrind) for these models, whose rows are 1024 wide (real models' rows are 3 to 5 times longer, so less); and in a loop of 2 million calls of one kernel, the thunk and a direct call took the same time within ±1.2%, either sign. Against `avx2` on this CPU, `avx` keeps 0.77 to 0.84 of the prompt speed and 0.65 to 0.89 of the generation speed. `x86-64-v2` keeps 0.68 and 0.92 for Q4_0 (it has SSSE3 kernels), but only 0.12 to 0.28 of the prompt speed and 0.16 to 0.65 of the generation speed for the K-quants, Q8_0 and IQ4_NL, which run ggml's generic C code there.

- *Size:* the two new levels and the dispatch add 69.7 KB to a program that links llama.cpp at `-O2` (68.7 KB of code and constants, 1 KB of tables: `x86-64-v2` 5.3 KB, `avx` 58.2 KB, the dispatch 3.7 KB); the `avx2` level is the old code. Upstream's way ships 14 libraries of about 1.4 MB each.
- *Static checks:* the service, the two model implementations, both engines, `browser_ai_runtime.cc` and `embedding_model_manager.cc`, with clang and Chromium's flags against the stand-ins of §11.8 (with the `base::CPU` methods used, as declared at 152), with and without `GGML_MOLTBROWSER_X86_CPU_KERNELS`; `cpuid` and `xgetbv` in the MSVC form compile for an `x86_64-pc-windows-msvc` target without `-mxsave`, as `base::CPU` relies on. `python3 tools/fuzz/molt_fuzz.py check` passes; CPUID isn't untrusted input, so no fuzzer was added.

**Needs a Chromium build:** all of the above with Chromium's toolchains: `gn check` on the real tree (new edges: the service on `engine:cpu_kernels` and the dispatch header, `molt_ai_unittests` on `inference:x86_cpu_kernels_unittests` and so on `//third_party/llama_cpp:ggml`), clang-cl's `/FI` and `-mpopcnt` on Windows, and the object checks above on an official (ThinLTO) build: `llvm-nm` on the level objects, which are bitcode there, must show only the 40 prefixed functions.

### 12.10 What is left

- **Real pre-AVX2 hardware.** Nothing ran on a real Sandy Bridge, Nehalem or Atom. To check there: the level chosen (`--v=1` logs it), a catalog model's speed, and no illegal-instruction crash. Expect the x86-64-v2 level to be slow on K-quant models (Q4_K_M runs ggml's generic C code there, about a quarter of the `avx2` level's generation speed and a sixth of its prompt speed on this CPU (§12.9); Q4_0, which has SSSE3 kernels, keeps 0.7 to 0.9, so Q4_0 builds of the catalog models would serve such CPUs much better); a model picker that warns on slow CPUs would need the level in the browser (below).
- **The level isn't in the browser's load log.** It is in `EngineModelDescription` and the service's `--v=1` log. Sending it in `ModelDescription` (an enum, which mojo validates) would put it in the browser's log line next to `gpu_status`, for bug reports about slow answers.
- **AVX2 machines leave speed on the table (found, not changed).** Everything in ggml-cpu except the two files is built for SSE3, so on AVX2 machines the F32/F16 operations run their SSE3 paths and `ggml_cpu_has_avx2()` is 0, which turns weight repacking off on x64 (§12.1, point 2), and `cpu_repack_used` reports what was allowed, not what happened. Building more of ggml-cpu per level (upstream builds all of it) would change what AVX2 machines run and how much memory a model takes (repacked weights are anonymous memory, §2.5), so it needs its own measurements; it also raises the hazard of §12.5, since the other files are C++ with weak symbols.
- **A `vnni` level** (AVX-512-VNNI or AVX-VNNI on top of `avx2`) is one more row in `_ggml_x86_levels` and `kLevels`; worth it only with repacking on. AMX stays out: its kernels live in `amx/`, not in the two files, they are compiled only with `__AMX_INT8__` and `__AVX512VNNI__`, and Linux must grant the AMX state (`arch_prctl(ARCH_REQ_XCOMP_PERM)`) before the sandbox (§2.3).
- **When updating llama.cpp:** compare the two lists in `ggml-cpu-x86-variants.h` with what `arch/x86/quants.c` and `repack.cpp` define, check the level objects for weak symbols and data, and run `X86CpuKernelsTest` (README.chromium).

## 13. Audio input

Status: implemented (October 2026), pending a compile on the build machine. A model whose projector has an audio encoder (llama.cpp's multimodal library, `tools/mtmd`, already vendored for §10) hears audio clips. The first consumer is the Prompt API's audio input (WEB_AI_APIS.md §5.2). The catalogue's audio model (§13.4) isn't offered until it is pinned and checked, so no user sees the feature yet.

### 13.1 Decisions

1. **No audio decoding in the browser or the service.** Pages' audio is decoded and resampled by Blink in the renderer (an `AudioBuffer`, or a file in a `Blob` or `ArrayBuffer`), to the rate and channel count the session asks for: 16 kHz, one channel. The browser receives float samples and turns them into 16-bit PCM (clamped; NaN and infinities become silence); the service receives that PCM in a buffer, never an audio file. llama.cpp's helpers that decode files (`mtmd-helper`: miniaudio, ffmpeg) are not vendored.
2. **Bounds** (`engine/audio_input.h`, checked in the browser before sending and again in the service, which treats a violation as a bad message):

   | What | Bound |
   |---|---|
   | Format | 16 kHz, one channel, 16-bit samples (the browser refuses any other rate or channel count) |
   | Length | 0.1 to 30 s (1,600 to 480,000 samples; the browser pads a shorter clip with silence) |
   | Bytes | exactly 2 per sample |
   | Clips per request | 4, history included |
   | Where | user turns only (a clip on an assistant turn is a bad message) |

   llama.cpp's Whisper preprocessing pads a clip with 30 s of silence and encodes whole 30-second windows, so a clip up to 29.99 s takes one window and the longest two.
3. **The projector is the same second handle** as an image projector (§10.1, 3): a projector may have an image encoder, an audio encoder or both. The service loads it as before; `ModelDescription.audio` says whether it has an audio encoder taking 16 kHz (a projector with neither encoder is `kInvalidVisionProjector`). A service that reports `audio` without having been sent a projector is a protocol violation and is killed.
4. **The service places the clips, as images.** A message's images and then its clips each get one media marker in front of its text, which `mtmd_tokenize_from_parts()` replaces in order; markers in untrusted text are neutralized, so page text can't move, duplicate or add a clip.
5. **Positions.** Audio embeddings take one position per token (on M-RoPE models the same position in all four sections, mtmd-helper's layout).
6. **No KV reuse across media; budget as images** (§10.1, 5 and 6): the new message's clips count with the system prompt, a history turn's go with it.
7. **Status values.** `GenerationStatus.kAudioUnsupported` for audio sent to a model without an audio encoder (answered at once); the browser maps it to `GenerationResult::audio_unsupported` and fails such a request before sending when it already knows (`LoadedModelHearsAudio()`).
8. **The catalogue knows what a projector takes** (`kProjectors` in `browser_ai_runtime.cc`: `images`, `audio`), for the model pickers and the Prompt API's availability; what a request may carry is what the loaded projector reports.
9. **Only verified audio encoders run** (hardening). llama.cpp's audio projectors differ in how many tokens a window takes, so the service gives audio only to projectors on an allow-list (`AllowedAudioWindowTokens()`, `engine/audio_input.h`): a projector type and frame-stacking factor, each verified here to give a window exactly the tokens llama.cpp reserves for it. The list holds one entry, Voxtral with a stacking factor of 4 (375 tokens a window), the layout llama.cpp's converter writes.
   - *From the file, in the service.* The projector's own header is read before llama.cpp loads it (`molt_llama::ReadProjectorFacts()`, through `llama_guard`: ggml's GGUF parser, every exception caught, the file rewound): its encoders, the audio projector type (as clip picks it) and the stacking factor. The browser's claim isn't consulted.
   - *Off the list.* A projector whose only encoder is an audio encoder off the list isn't loaded at all: the model loads for text alone, and audio answers `kAudioUnsupported`. One that also has an image encoder loads for images, without audio.
   - *At load.* Even on the list, audio is on only after a placeholder clip (no samples, nothing encoded) showed llama.cpp reserving the list's count for its window; otherwise the projector goes too and the model loads for text.
   - *Per window.* Every audio window is checked against the same count before it is encoded, and clip's own check of its graph's output returns an error instead of ending the process (local patch, README.chromium), so a disagreement fails that request (`kDecodeFailed`) and nothing else.
   - The same local patch has llama.cpp count a window in the order its graph builds it, for every Whisper-type projector, and runs Voxtral without pooling, as the original model does.

### 13.2 The flow

```
browser                                              inference service
-------                                              -----------------
page audio (Blink, renderer): decode, mix to mono,
  resample to 16 kHz (instance info)
  -> on_device_model.mojom.AudioData (floats)
ModelAudioFromPage: one channel at 16 kHz, frame
  count = data, 1..480,000 samples; clamp, NaN -> 0,
  16-bit, pad to 1,600  (runtime/model_audio.h)
ChatPrompt.user_audio / ChatMessage.audio
  -> ToServiceChat: <= 4 (new message's first,
     then the newest history turns'), each checked
  -> mojom.AudioInput {BigBuffer pcm} ------------->  MoltModelImpl: CheckAudio (bad message on
                                                         any violation), kAudioUnsupported without
                                                         an audio encoder, copies samples out
                                                       LlamaEngine::Chat: count + budget with
                                                         placeholders (silence of the length),
                                                         neutralize markers, tokenize_from_parts,
                                                         encode each 30-s window, decode embeddings
```

### 13.3 Threat model

**Attackers:** a web page (it chooses the audio, its length and the text around it); a compromised renderer (any channel count, rate, frame count and sample values, and any data length); a malicious or odd projector (pinned download only, parsed in the sandbox).

- **Parsers stay where they were.** Compressed audio is decoded in the renderer, the page's own sandboxed process; the browser handles only float samples (a shape check and a clamped conversion, no parser), and the service only 16-bit PCM within the bounds.
- **The browser trusts no AudioData.** Channel count, rate and frame count must be exactly what the session asked for and match the data; non-finite samples become silence; at most 4 clips of at most 30 s per request, so the conversion is bounded (at most 1.9 million samples, after mojo already copied them). The fuzzers (§13.6) drive it with arbitrary shapes and values.
- **The service trusts no buffer.** `CheckAudio()` checks count, role and every clip's byte length before anything is queued; a buffer that can't be mapped fails the request. `mtmd_bitmap_init_from_audio()` copies exactly the checked samples. The preprocessing (an FFT and a mel filter bank in `mtmd-audio.cpp`) is linear in the clip, at most two windows per clip.
- **Exceptions and aborts.** The audio calls (`mtmd_support_audio`, `mtmd_get_audio_sample_rate`, `mtmd_bitmap_init_from_audio`, the encode of an audio chunk) and the projector header read are in the audited list at the top of `llama_engine.cc`. Only allow-listed encoders run (§13.1, 9), and a token-count disagreement is an error for the request, not the end of the process.
- **Browser memory.** A clip held for a page is at most 0.96 MB and counts as an image against the per-site and total caps (WEB_AI_APIS.md §5.2).

### 13.4 The model and its pins

The registry gets **Voxtral Mini 3B (2507)** (Mistral AI, Apache 2.0) in Q4_K_M, with its projector (a Whisper-large-v3 encoder and a two-layer adapter) in Q8_0, from `ggml-org/Voxtral-Mini-3B-2507-GGUF` (repo and file names marked to be confirmed when pinning).

- **Why this one.** A permissive licence; speech understanding (transcription, questions and summaries about what is said, several languages), not only transcription; a size that sits next to the browser on 8 GB machines; and a projector type the vendored llama.cpp has (`voxtral`). Families looked at (sizes and terms as published, to confirm when pinning):

  | Family | Size | Terms | Why not |
  |---|---|---|---|
  | Voxtral Mini 3B | 3B + 0.6B encoder | Apache 2.0 | chosen |
  | Voxtral Small | 24B | Apache 2.0 | too big next to the chat's model |
  | Ultravox (Llama 3.x based) | 1B / 8B | Llama licence (acceptance) | terms; weaker at 1B |
  | Qwen2-Audio | 7B | Apache 2.0 | too big for its quality next to the chat's model |
  | Qwen2.5-Omni | 3B / 7B | 3B: research licence; 7B: Apache 2.0 | terms (3B), size (7B) |
  | Qwen3-Omni | 30B-A3B | Apache 2.0 | too big |
  | Qwen3-ASR, Granite Speech | 0.6B to 8B | Apache 2.0 | transcription and translation only, not questions about audio |
  | Gemma 4 E2B / E4B | 2B / 4B effective | Gemma terms (acceptance) | terms; worth it later as one model that sees and hears (the catalogue's E4B entry is hidden until pinned) |
  | LFM2-Audio | 1.5B | LFM licence | terms |
  | MERaLiON, Music Flamingo, MiMo-Audio | various | various | regional, music or research models |

- **Fail closed, twice.** Its revision, both SHA-256s and both sizes are empty: until `tools/models/pin_models.py --only voxtral-mini-3b --write` fills all five from one revision, it isn't in the catalogue (`IsPinnedWithProjector()`). Its entry is also hidden: after pinning, the owner unhides it by hand once the pinned files pass the audio check on the build machine (`docs/BUILD_MACHINE_CHECKLIST.md`, "ai-finish"), because the vendored llama.cpp's Voxtral path has run here only with test projectors (in the converter's layout, §13.6).
- **Download** as one unit with its projector, exactly as §10.4.
- **The pick for pages.** A page that asks for audio gets a downloaded model that hears audio when one has to be loaded (WEB_AI_APIS.md §5.2); the chat's model is never swapped out.

### 13.5 Not done

- **Audio in the AI chat** (a microphone button or an attached recording) and in Agent mode. The runtime API takes it (`ChatPrompt::user_audio`, `ChatMessage::audio`); the Prompt API is the only caller.
- **Audio output** (speech), and **video**.
- **Streaming audio** (clips are whole, at most 30 s).
- **A model that sees and hears** (Gemma 4, Qwen2.5-Omni 7B): the plumbing allows both in one request; no catalogue model has both.
- **Converting the samples on a worker.** The browser converts a request's clips on the UI thread, bounded as above; a worker, as for images, if profiling shows it.

### 13.6 Tests

**In the repository:**

- `engine/audio_input_unittest.cc` (8, std-only): the bounds and byte lengths, the conversion (only one channel at 16 kHz with the data's frame count, clamping, NaN and infinities, rounding, padding to the minimum, the longest clip), the float conversion, and the allow-list (Voxtral at a factor of 4 only; other factors, other types, near names and embedded NULs refused).
- `runtime/model_audio_unittest.cc` (3): a page's audio becoming a clip; validity and length in seconds; which clips a request sends (every split of up to 9 over three turns stays within 4 and takes the newest).
- `runtime/inference_process_host_unittest.cc`: `audio` with the projector the browser sent is recorded; without one it is a violation.
- The web AI tests for the Prompt API (WEB_AI_APIS.md §12) and `tools/models/pin_models_test.py` (the audio model and its projector pinned as one; every projector all-or-nothing).
- Fuzzers (`docs/FUZZING.md`): `molt_audio_input_fuzzer` (the service's bounds and the browser's conversion on arbitrary shapes and values) and `molt_web_ai_conversation_fuzzer` (clips in a session's context, and a page's audio becoming a clip). Run here for 7 and 3 minutes and in smoke mode, without a finding.

**Run outside the repository** (`LlamaEngine` and llama.cpp built by the stub GN root of §11.8, with a tiny random-weight llama model and a random-weight projector of llama.cpp's Voxtral type: Whisper encoder, frame stacking, two-layer adapter):

| Case | Result, on the CPU and with the language model on lavapipe |
|---|---|
| A 1 s clip; a 0.1 s clip | loaded with `audio` and not `vision`; one window each; an answer |
| Two clips; a clip in the history and one in the new message | two windows each; an answer |
| 29.99 s and exactly 30 s | two windows (Whisper's padding); an answer |
| 30 s and one sample, 0.1 s less one sample, five clips | refused (`kTokenizeFailed` from the engine; the service refuses them before as bad messages) |
| A projector off the allow-list (Voxtral's type with a factor of 2; Ultravox's type with 8) | not loaded: the model answers text, every audio request `kAudioUnsupported` |

The projector is the layout llama.cpp's converter writes (a factor of 4); one window takes 375 tokens. The answers start with the same tokens on the CPU and with the language model on lavapipe (random weights part them later at near-ties).

**The count check** (llama.cpp's multimodal library directly, every window encoded, clips of 0.1 s, 1 s, 29.99 s and 30 s): the count llama.cpp reserves and the count its graph produces agree for the Voxtral layout (375 a window) and for the Ultravox layout with a factor of 8 (188 a window), and every window encodes. The safety nets were also run against a build whose count was made wrong on purpose: each encode failed as an error without ending the process, and the engine's load-time check turned audio off (the model answered text, audio requests `kAudioUnsupported`).

**Needs the build machine:** a compile; the pinned Voxtral model with real speech (a question about a recording, a transcription, a 30 s clip, five clips refused) on the CPU and on a GPU, its speed and memory on 8 and 16 GB machines, and the token count of one window (the Prompt API's per-clip count assumes at most 375); the Prompt API with real audio in a page (WEB_AI_APIS.md §12).
