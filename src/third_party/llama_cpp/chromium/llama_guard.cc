// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Built with exceptions (see llama_guard.h): every function catches
// everything.

#include "third_party/llama_cpp/chromium/llama_guard.h"

#include <atomic>
#include <cstring>
#include <mutex>

#include "third_party/llama_cpp/ggml/include/gguf.h"

#if defined(MOLT_LLAMA_VULKAN)
#include "third_party/llama_cpp/chromium/ggml-vulkan-moltbrowser.h"
#include "third_party/llama_cpp/ggml/include/ggml-vulkan.h"
#endif

namespace molt_llama {

namespace {

std::atomic<bool> g_backend_failed{false};

void MarkFailed() {
  g_backend_failed.store(true, std::memory_order_relaxed);
}

// A sanity bound on the layer count read from an untrusted header.
constexpr uint32_t kMaxLayerCount = 4096;

}  // namespace

bool BackendFailed() noexcept {
  return g_backend_failed.load(std::memory_order_relaxed);
}

int32_t Decode(llama_context* ctx, llama_batch batch) noexcept {
  try {
    return llama_decode(ctx, batch);
  } catch (...) {
    MarkFailed();
    return kBackendFailedResult;
  }
}

int32_t Process(llama_context* ctx,
                llama_process_type type,
                llama_batch_ext* batch) noexcept {
  try {
    return llama_process(ctx, type, batch);
  } catch (...) {
    MarkFailed();
    return kBackendFailedResult;
  }
}

float* GetLogitsIth(llama_context* ctx, int32_t i) noexcept {
  try {
    return llama_get_logits_ith(ctx, i);
  } catch (...) {
    MarkFailed();
    return nullptr;
  }
}

llama_token SamplerSample(llama_sampler* sampler,
                          llama_context* ctx,
                          int32_t idx) noexcept {
  try {
    return llama_sampler_sample(sampler, ctx, idx);
  } catch (...) {
    MarkFailed();
    return LLAMA_TOKEN_NULL;
  }
}

bool MemoryClear(llama_memory_t mem, bool data) noexcept {
  try {
    llama_memory_clear(mem, data);
    return true;
  } catch (...) {
    MarkFailed();
    return false;
  }
}

uint32_t ReadLayerCount(FILE* file) noexcept {
  if (!file) return 0;
  uint32_t layers = 0;
  try {
    gguf_init_params params;
    params.no_alloc = true;
    params.ctx = nullptr;
    gguf_context* gguf = gguf_init_from_file_ptr(file, params);
    if (gguf) {
      const int64_t arch = gguf_find_key(gguf, "general.architecture");
      if (arch >= 0 && gguf_get_kv_type(gguf, arch) == GGUF_TYPE_STRING) {
        const std::string key =
            std::string(gguf_get_val_str(gguf, arch)) + ".block_count";
        const int64_t k = gguf_find_key(gguf, key.c_str());
        if (k >= 0) {
          switch (gguf_get_kv_type(gguf, k)) {
            case GGUF_TYPE_UINT32:
              layers = gguf_get_val_u32(gguf, k);
              break;
            case GGUF_TYPE_INT32: {
              const int32_t v = gguf_get_val_i32(gguf, k);
              layers = v > 0 ? static_cast<uint32_t>(v) : 0;
              break;
            }
            case GGUF_TYPE_UINT64: {
              const uint64_t v = gguf_get_val_u64(gguf, k);
              layers = v <= kMaxLayerCount ? static_cast<uint32_t>(v) : 0;
              break;
            }
            default:
              break;
          }
        }
      }
      gguf_free(gguf);
    }
  } catch (...) {
    layers = 0;
  }
  if (layers > kMaxLayerCount) layers = 0;
  if (std::fseek(file, 0, SEEK_SET) != 0) layers = 0;
  return layers;
}

GpuDevice::GpuDevice() = default;
GpuDevice::~GpuDevice() = default;
GpuDevice::GpuDevice(const GpuDevice&) = default;
GpuDevice& GpuDevice::operator=(const GpuDevice&) = default;

bool VulkanBuilt() noexcept {
#if defined(MOLT_LLAMA_VULKAN)
  return true;
#else
  return false;
#endif
}

bool ListVulkanDevices(std::vector<GpuDevice>* devices) noexcept {
  if (!devices) return false;
  devices->clear();
#if defined(MOLT_LLAMA_VULKAN)
  try {
    // ggml_backend_vk_reg() initializes once and catches its own errors;
    // the mutex only keeps two callers from listing at the same time.
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    ggml_backend_reg_t reg = ggml_backend_vk_reg();
    if (!reg) return false;
    const size_t count = ggml_backend_reg_dev_count(reg);
    for (size_t i = 0; i < count; ++i) {
      // Creates ggml's device contexts on first use (Vulkan queries that
      // can throw; caught below).
      ggml_backend_dev_t dev = ggml_backend_reg_dev_get(reg, i);
      ggml_vk_moltbrowser_device_info info;
      if (!dev ||
          !ggml_vk_moltbrowser_get_device_info(static_cast<int>(i), &info)) {
        continue;
      }
      GpuDevice d;
      d.device = dev;
      d.index = static_cast<int>(i);
      d.vendor_id = info.vendor_id;
      d.device_id = info.device_id;
      d.device_type = info.device_type;
      d.driver_id = info.driver_id;
      d.driver_version = info.driver_version;
      d.api_version = info.api_version;
      d.memory_total = info.memory_total;
      d.memory_free = info.memory_free;
      d.name = info.name;
      std::memcpy(d.driver_uuid.data(), info.driver_uuid,
                  d.driver_uuid.size());
      std::memcpy(d.pipeline_cache_uuid.data(), info.pipeline_cache_uuid,
                  d.pipeline_cache_uuid.size());
      devices->push_back(std::move(d));
    }
  } catch (...) {
    devices->clear();
    return false;
  }
  return !devices->empty();
#else
  return false;
#endif
}

bool SetPipelineCacheData(int device,
                          const std::vector<uint8_t>& data) noexcept {
#if defined(MOLT_LLAMA_VULKAN)
  try {
    return ggml_vk_moltbrowser_set_pipeline_cache_data(device, data.data(),
                                                       data.size());
  } catch (...) {
    return false;
  }
#else
  return false;
#endif
}

std::optional<size_t> PipelineCacheSize(int device) noexcept {
#if defined(MOLT_LLAMA_VULKAN)
  try {
    size_t size = 0;
    if (ggml_vk_moltbrowser_get_pipeline_cache_data(device, nullptr, &size)) {
      return size;
    }
  } catch (...) {
  }
#endif
  return std::nullopt;
}

bool GetPipelineCacheData(int device,
                          size_t max_bytes,
                          std::vector<uint8_t>* data) noexcept {
  if (!data) return false;
  data->clear();
#if defined(MOLT_LLAMA_VULKAN)
  try {
    size_t size = 0;
    if (!ggml_vk_moltbrowser_get_pipeline_cache_data(device, nullptr, &size) ||
        size > max_bytes) {
      return false;
    }
    data->resize(size);
    // The cache may have grown since the size query (it doesn't between
    // requests, which is when the service asks); VK_INCOMPLETE fails then.
    if (!ggml_vk_moltbrowser_get_pipeline_cache_data(device, data->data(),
                                                     &size) ||
        size > data->size()) {
      data->clear();
      return false;
    }
    data->resize(size);
    return true;
  } catch (...) {
    data->clear();
    return false;
  }
#else
  return false;
#endif
}

bool DeviceLost(int device) noexcept {
#if defined(MOLT_LLAMA_VULKAN)
  try {
    return ggml_vk_moltbrowser_device_lost(device);
  } catch (...) {
    return true;
  }
#else
  return false;
#endif
}

}  // namespace molt_llama
