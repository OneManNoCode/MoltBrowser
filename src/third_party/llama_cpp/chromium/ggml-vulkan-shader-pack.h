// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The packed form of ggml-vulkan's embedded shaders (GN switch
// molt_llama_vulkan; docs/design/SANDBOXED_INFERENCE.md section 11.14).
//
// llama.cpp's generator (vulkan-shaders-gen) writes one .cpp per shader file
// that embeds every variant's SPIR-V as `const unsigned char <name>_data[]`
// with its size in `<name>_len`; ggml-vulkan.cpp passes the pair to
// ggml_vk_create_pipeline(). MoltBrowser's build runs ggml_vk_shader_pack
// over each of those files (chromium/ggml_vk_shader_pack.cc):
//
//  - The variants of one shader file (a "family") are laid out one after
//    the other in blocks of at most kBlockRawSize bytes (a variant never
//    straddles two blocks; one larger than that gets a block of its own)
//    and each block is brotli-compressed. Identical variants share bytes.
//  - <name>_data becomes a kDescriptorSize-byte descriptor (below) and
//    <name>_len its size. The descriptor names the family (an index into
//    ggml_vk_moltbrowser_shader_families, the order of BUILD.gn's shader
//    list), the block, where the variant starts in the block's raw bytes,
//    its size and a checksum of its SPIR-V.
//  - Variants for weight types the build leaves out (BUILD.gn
//    _ggml_vk_dropped_types) become <name>_len = 0; their ops never reach
//    the GPU (ggml_vk_moltbrowser_type_built()).
//
// ggml_vk_create_pipeline_func() inflates the one variant it compiles
// (ggml_vk_moltbrowser_unpack_shader(), in the sandboxed inference service)
// and hands the driver exactly the SPIR-V the generator wrote. Pipelines
// compile on first use, so a model inflates only the shaders it runs.
//
// Standard library only: shared by the host tool and the backend.

#ifndef THIRD_PARTY_LLAMA_CPP_CHROMIUM_GGML_VULKAN_SHADER_PACK_H_
#define THIRD_PARTY_LLAMA_CPP_CHROMIUM_GGML_VULKAN_SHADER_PACK_H_

#include <stddef.h>
#include <stdint.h>

namespace ggml_vk_moltbrowser {

// One brotli-compressed block of a family's variants.
struct PackedShaderBlock {
  const unsigned char* data;
  uint32_t size;      // compressed bytes
  uint32_t raw_size;  // bytes once inflated, at most kMaxBlockRawSize
};

// The blocks of one shader file's variants; |blocks| is null when every
// variant was left out.
struct PackedShaderFamily {
  const char* name;  // the .comp file, for error messages
  const PackedShaderBlock* blocks;
  uint32_t block_count;
};

// The raw size the packer aims each block at, and the most any block may
// hold once inflated (a single variant larger than the target gets a block
// of its own; the largest variant at b11641 is about 90 KB).
inline constexpr uint32_t kBlockRawSize = 256 * 1024;
inline constexpr uint32_t kMaxBlockRawSize = 4 * 1024 * 1024;

// SPIR-V's magic number, little-endian: never the start of a descriptor.
inline constexpr unsigned char kSpirvMagic[4] = {0x03, 0x02, 0x23, 0x07};
inline constexpr unsigned char kDescriptorMagic[4] = {'M', 'V', 'K', 'Z'};
inline constexpr size_t kDescriptorSize = 24;

struct ShaderDescriptor {
  uint32_t family = 0;
  uint32_t block = 0;
  uint32_t offset = 0;  // in the block's raw bytes
  uint32_t size = 0;    // SPIR-V bytes, a positive multiple of 4
  uint32_t checksum = 0;
};

// FNV-1a (32 bits) of the variant's SPIR-V: catches a packer or decoder
// mistake, not an attacker (the data is part of the binary).
inline uint32_t ShaderChecksum(const unsigned char* data, size_t size) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < size; ++i) {
    h ^= data[i];
    h *= 16777619u;
  }
  return h;
}

inline void EncodeDescriptor(const ShaderDescriptor& d,
                             unsigned char out[kDescriptorSize]) {
  for (size_t i = 0; i < 4; ++i) out[i] = kDescriptorMagic[i];
  const uint32_t fields[5] = {d.family, d.block, d.offset, d.size,
                              d.checksum};
  for (size_t f = 0; f < 5; ++f) {
    for (size_t b = 0; b < 4; ++b) {
      out[4 + 4 * f + b] = static_cast<unsigned char>(fields[f] >> (8 * b));
    }
  }
}

// False unless |data| is exactly a descriptor (size and magic) whose size
// is a positive multiple of 4 that fits a block after |offset|.
inline bool DecodeDescriptor(const void* data,
                             size_t size,
                             ShaderDescriptor* out) {
  if (!data || size != kDescriptorSize) return false;
  const unsigned char* p = static_cast<const unsigned char*>(data);
  for (size_t i = 0; i < 4; ++i) {
    if (p[i] != kDescriptorMagic[i]) return false;
  }
  uint32_t fields[5] = {};
  for (size_t f = 0; f < 5; ++f) {
    for (size_t b = 0; b < 4; ++b) {
      fields[f] |= static_cast<uint32_t>(p[4 + 4 * f + b]) << (8 * b);
    }
  }
  ShaderDescriptor d;
  d.family = fields[0];
  d.block = fields[1];
  d.offset = fields[2];
  d.size = fields[3];
  d.checksum = fields[4];
  if (d.size == 0 || d.size % 4 != 0 || d.size > kMaxBlockRawSize ||
      d.offset > kMaxBlockRawSize - d.size) {
    return false;
  }
  *out = d;
  return true;
}

}  // namespace ggml_vk_moltbrowser

// The packed families, in the order of BUILD.gn's shader list (generated by
// ggml_vk_shader_pack --registry).
extern const ggml_vk_moltbrowser::PackedShaderFamily* const
    ggml_vk_moltbrowser_shader_families[];
extern const uint32_t ggml_vk_moltbrowser_shader_family_count;
// The weight types whose variants were left out (BUILD.gn
// _ggml_vk_dropped_types), as ggml_type_name() spells them in lower case;
// null-terminated.
extern const char* const ggml_vk_moltbrowser_dropped_types[];

#endif  // THIRD_PARTY_LLAMA_CPP_CHROMIUM_GGML_VULKAN_SHADER_PACK_H_
