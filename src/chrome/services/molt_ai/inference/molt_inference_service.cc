// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/services/molt_ai/inference/molt_inference_service.h"

#include <stdio.h>

#include <algorithm>
#include <utility>

#include "base/files/file.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "build/build_config.h"
#include "chrome/services/molt_ai/inference/molt_embedding_model_impl.h"
#include "chrome/services/molt_ai/inference/molt_model_impl.h"
#include "third_party/llama_cpp/include/llama.h"
#include "third_party/llama_cpp/tools/mtmd/mtmd.h"

#if BUILDFLAG(IS_WIN)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include "base/files/scoped_file.h"
#endif

#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
#include <sys/resource.h>
#endif

namespace molt_ai {

namespace {

// llama.cpp's log (and its multimodal library's), at --v=1 only and only
// warnings and errors. It logs model metadata and failures, never prompt or
// response text: the pre-tokenizer debug output that would is compiled out
// upstream, and mtmd logs the text it tokenizes only at debug level, which
// this drops. May run on any thread.
void LlamaLogCallback(ggml_log_level level, const char* text, void*) {
  if (level != GGML_LOG_LEVEL_WARN && level != GGML_LOG_LEVEL_ERROR) return;
  VLOG(1) << "[MoltAI inference] llama.cpp: " << (text ? text : "");
}

// Second line of defence: the browser checks the CPU before launching. On
// x64 ggml's kernels are built for three levels, the lowest x86-64-v2
// (SSE4.2, POPCNT); the engine runs the best this CPU supports
// (docs/design/SANDBOXED_INFERENCE.md section 12).
bool CpuSupported() {
  return LlamaEngine::CpuSupported();
}

// This process's limit on anonymous memory, set by the sandbox
// (kOnDeviceModelExecution scales it with RAM). 0 = none known.
uint64_t SandboxMemoryLimit() {
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
  struct rlimit limit;
  if (getrlimit(RLIMIT_DATA, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY) {
    return static_cast<uint64_t>(limit.rlim_cur);
  }
  return 0;
#elif BUILDFLAG(IS_WIN)
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
  if (!::QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation,
                                   &info, sizeof(info), nullptr)) {
    return 0;
  }
  const DWORD flags = info.BasicLimitInformation.LimitFlags;
  uint64_t limit = 0;
  if (flags & JOB_OBJECT_LIMIT_JOB_MEMORY) limit = info.JobMemoryLimit;
  if (flags & JOB_OBJECT_LIMIT_PROCESS_MEMORY) {
    const uint64_t process_limit = info.ProcessMemoryLimit;
    limit = limit ? std::min(limit, process_limit) : process_limit;
  }
  return limit;
#else
  return 0;  // macOS: no limit; the browser's footprint watchdog bounds it
#endif
}

// The browser's read-only handle as a synchronous stdio stream for
// llama_model_load_from_file_ptr(). nullptr if it can't be converted.
ScopedStdioFile ToStdioFile(base::File file) {
  // llama.cpp reads synchronously; an overlapped (async) Windows handle
  // would break the CRT reads.
  if (!file.IsValid() || file.async()) return nullptr;
#if BUILDFLAG(IS_WIN)
  HANDLE handle = file.TakePlatformFile();
  const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle),
                                 _O_RDONLY | _O_BINARY);
  if (fd == -1) {
    ::CloseHandle(handle);
    return nullptr;
  }
  FILE* stream = _fdopen(fd, "rb");
  if (!stream) {
    _close(fd);  // also closes |handle|
    return nullptr;
  }
  return ScopedStdioFile(stream);
#else
  // Take the fd out of base::File first: fclose() closes it later, and no
  // ScopedFD may still own it then (fd-ownership enforcement).
  const int fd = file.TakePlatformFile();
  FILE* stream = fdopen(fd, "rb");
  if (!stream) {
    base::ScopedFD close_it(fd);
    return nullptr;
  }
  return ScopedStdioFile(stream);
#endif
}

mojom::LoadModelStatus ToMojo(EngineLoadStatus status) {
  switch (status) {
    case EngineLoadStatus::kOk:
      return mojom::LoadModelStatus::kOk;
    case EngineLoadStatus::kInvalidModelFile:
      return mojom::LoadModelStatus::kInvalidModelFile;
    case EngineLoadStatus::kOutOfMemory:
      return mojom::LoadModelStatus::kOutOfMemory;
    case EngineLoadStatus::kBackendUnavailable:
      return mojom::LoadModelStatus::kBackendUnavailable;
    case EngineLoadStatus::kUnsupportedCpu:
      return mojom::LoadModelStatus::kUnsupportedCpu;
    case EngineLoadStatus::kInvalidVisionProjector:
      return mojom::LoadModelStatus::kInvalidVisionProjector;
  }
  return mojom::LoadModelStatus::kInvalidModelFile;
}

mojom::GpuStatus ToMojo(EngineGpuStatus status) {
  switch (status) {
    case EngineGpuStatus::kNotRequested:
      return mojom::GpuStatus::kNotRequested;
    case EngineGpuStatus::kNotBuilt:
      return mojom::GpuStatus::kNotBuilt;
    case EngineGpuStatus::kNoDevice:
      return mojom::GpuStatus::kNoDevice;
    case EngineGpuStatus::kNoUsableDevice:
      return mojom::GpuStatus::kNoUsableDevice;
    case EngineGpuStatus::kTooLittleMemory:
      return mojom::GpuStatus::kTooLittleMemory;
    case EngineGpuStatus::kLoadFailed:
      return mojom::GpuStatus::kLoadFailed;
    case EngineGpuStatus::kOn:
      return mojom::GpuStatus::kOn;
  }
  return mojom::GpuStatus::kNotRequested;
}

}  // namespace

MoltInferenceService::MoltInferenceService(
    mojo::PendingReceiver<mojom::MoltInferenceService> receiver)
    : receiver_(this, std::move(receiver)) {
  // Before any other llama.cpp call; the default loggers write to stderr.
  llama_log_set(&LlamaLogCallback, nullptr);
  mtmd_log_set(&LlamaLogCallback, nullptr);
}

MoltInferenceService::~MoltInferenceService() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

void MoltInferenceService::LoadModel(mojom::ModelLoadParamsPtr params,
                                     mojo::PendingReceiver<mojom::Model> model,
                                     LoadModelCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (load_requested_) {
    receiver_.ReportBadMessage("LoadModel called twice");
    return;
  }
  load_requested_ = true;

  if (!CpuSupported()) {
    std::move(callback).Run(mojom::LoadModelStatus::kUnsupportedCpu, nullptr);
    return;
  }
  ScopedStdioFile file = ToStdioFile(std::move(params->weights));
  if (!file) {
    std::move(callback).Run(mojom::LoadModelStatus::kInvalidModelFile,
                            nullptr);
    return;
  }
  // Image input: the vision projector, read completely during the load.
  // (ReadOnlyFile? is a base::File; invalid means none.)
  ScopedStdioFile projector;
  if (params->vision_projector.IsValid()) {
    projector = ToStdioFile(std::move(params->vision_projector));
    if (!projector) {
      std::move(callback).Run(mojom::LoadModelStatus::kInvalidVisionProjector,
                              nullptr);
      return;
    }
  }

  EngineLoadParams load;
  load.file_size = params->file_size;
  load.vision_projector_size =
      projector ? params->vision_projector_size : 0;
  load.total_ram_bytes = params->total_ram_bytes;
  load.cpu_threads = std::clamp<uint32_t>(params->cpu_threads, 1, 256);
  load.max_context_tokens = params->max_context_tokens;
  load.allow_cpu_repack = params->allow_cpu_repack;
  load.memory_limit_bytes = SandboxMemoryLimit();
  load.use_gpu = params->use_gpu;

  // Generation must not run on the thread that receives mojo messages (a
  // closed streamer would only be seen between requests).
  // CONTINUE_ON_SHUTDOWN: a decode in progress can't delay process exit.
  engine_ = base::SequenceBound<LlamaEngine>(
      base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_BLOCKING,
           base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN}));
  engine_.AsyncCall(&LlamaEngine::Load)
      .WithArgs(std::move(file), load, std::move(projector))
      .Then(base::BindOnce(&MoltInferenceService::OnLoaded,
                           weak_factory_.GetWeakPtr(), std::move(model),
                           std::move(callback)));
}

void MoltInferenceService::OnLoaded(mojo::PendingReceiver<mojom::Model> model,
                                    LoadModelCallback callback,
                                    EngineLoadResult result) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (result.status != EngineLoadStatus::kOk) {
    engine_.Reset();  // frees whatever was loaded, on the engine sequence
    std::move(callback).Run(ToMojo(result.status), nullptr);
    return;
  }
  if (!result.description.cpu_kernels.empty()) {
    VLOG(1) << "[MoltAI inference] CPU kernels: "
            << result.description.cpu_kernels;
  }
  auto description = mojom::ModelDescription::New();
  description->architecture = std::move(result.description.architecture);
  description->backend = std::move(result.description.backend);
  description->context_tokens = result.description.context_tokens;
  description->train_context_tokens = result.description.train_context_tokens;
  description->vocab_size = result.description.vocab_size;
  description->cpu_repack_used = result.description.cpu_repack_used;
  description->vision = result.description.vision;
  description->gpu_status = ToMojo(result.description.gpu_status);
  description->gpu_layers = result.description.gpu_layers;
  model_ = std::make_unique<MoltModelImpl>(
      std::move(engine_), result.description.vision, std::move(model),
      base::BindOnce(&MoltInferenceService::OnModelDisconnected,
                     weak_factory_.GetWeakPtr()));
  std::move(callback).Run(mojom::LoadModelStatus::kOk,
                          std::move(description));
}

void MoltInferenceService::OnModelDisconnected() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // The browser unloaded the model; it drops the service pipe too, which
  // ends the process. Free the weights now regardless.
  model_.reset();
}

void MoltInferenceService::LoadEmbeddingModel(
    mojom::EmbeddingModelLoadParamsPtr params,
    mojo::PendingReceiver<mojom::EmbeddingModel> model,
    LoadEmbeddingModelCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (load_requested_) {
    receiver_.ReportBadMessage("a model was already loaded");
    return;
  }
  // Caps from molt_inference.mojom (the browser never sends more).
  if (params->dimensions == 0 ||
      params->dimensions > mojom::kMaxEmbeddingDimensions ||
      params->max_tokens_per_text < mojom::kMinEmbedTokensPerText ||
      params->max_tokens_per_text > mojom::kMaxEmbedTokensPerText ||
      params->terminator.size() > mojom::kMaxEmbedTerminatorBytes) {
    receiver_.ReportBadMessage("embedding model parameters over the caps");
    return;
  }
  load_requested_ = true;

  if (!CpuSupported()) {
    std::move(callback).Run(mojom::LoadModelStatus::kUnsupportedCpu, nullptr);
    return;
  }
  ScopedStdioFile file = ToStdioFile(std::move(params->weights));
  if (!file) {
    std::move(callback).Run(mojom::LoadModelStatus::kInvalidModelFile,
                            nullptr);
    return;
  }

  EmbeddingLoadParams load;
  load.file_size = params->file_size;
  load.cpu_threads = std::clamp<uint32_t>(params->cpu_threads, 1, 256);
  switch (params->pooling) {
    case mojom::EmbeddingPooling::kMean:
      load.pooling = EmbeddingPoolingType::kMean;
      break;
    case mojom::EmbeddingPooling::kCls:
      load.pooling = EmbeddingPoolingType::kCls;
      break;
    case mojom::EmbeddingPooling::kLast:
      load.pooling = EmbeddingPoolingType::kLast;
      break;
  }
  load.max_tokens_per_text = params->max_tokens_per_text;
  load.terminator = params->terminator;
  load.dimensions = params->dimensions;

  // As for LoadModel: never on the thread that receives mojo messages.
  embedding_engine_ = base::SequenceBound<LlamaEmbeddingEngine>(
      base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_BLOCKING,
           base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN}));
  embedding_engine_.AsyncCall(&LlamaEmbeddingEngine::Load)
      .WithArgs(std::move(file), load)
      .Then(base::BindOnce(&MoltInferenceService::OnEmbeddingLoaded,
                           weak_factory_.GetWeakPtr(), std::move(model),
                           std::move(callback)));
}

void MoltInferenceService::OnEmbeddingLoaded(
    mojo::PendingReceiver<mojom::EmbeddingModel> model,
    LoadEmbeddingModelCallback callback,
    EmbeddingLoadResult result) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (result.status != EngineLoadStatus::kOk) {
    embedding_engine_.Reset();  // frees it on the engine sequence
    std::move(callback).Run(ToMojo(result.status), nullptr);
    return;
  }
  auto description = mojom::EmbeddingModelDescription::New();
  description->architecture = std::move(result.info.architecture);
  description->backend = std::move(result.info.backend);
  description->native_dimensions = result.info.native_dimensions;
  description->dimensions = result.info.dimensions;
  description->max_tokens_per_text = result.info.max_tokens_per_text;
  description->vocab_size = result.info.vocab_size;
  // Closing the EmbeddingModel pipe frees the model, as for a chat model.
  embedding_model_ = std::make_unique<MoltEmbeddingModelImpl>(
      std::move(embedding_engine_), std::move(model),
      base::BindOnce(
          [](base::WeakPtr<MoltInferenceService> self) {
            if (self) self->embedding_model_.reset();
          },
          weak_factory_.GetWeakPtr()));
  std::move(callback).Run(mojom::LoadModelStatus::kOk,
                          std::move(description));
}

std::unique_ptr<MoltInferenceService> RunMoltInferenceService(
    mojo::PendingReceiver<mojom::MoltInferenceService> receiver) {
  return std::make_unique<MoltInferenceService>(std::move(receiver));
}

}  // namespace molt_ai
