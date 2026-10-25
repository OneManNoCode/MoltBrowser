// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// ggml-vulkan's packed shaders (GN switch molt_llama_vulkan;
// docs/design/SANDBOXED_INFERENCE.md section 11.14): the descriptor format,
// every block of every shader file inflating, real variants coming back as
// SPIR-V, the dropped weight types' variants left out and refused, and the
// op gate that keeps those types on the CPU. Built with exceptions, like
// ggml-vulkan, because a refused variant throws.

#include <cstdint>
#include <cstring>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/brotli/include/brotli/decode.h"
#include "third_party/llama_cpp/chromium/ggml-vulkan-shader-pack.h"
#include "third_party/llama_cpp/ggml/include/ggml.h"

// From the generated ggml-vulkan-shaders.hpp (a few variants by name).
extern const uint64_t matmul_f32_f32_len;
extern const unsigned char matmul_f32_f32_data[];
extern const uint64_t mul_mat_vec_q4_k_f32_f32_len;
extern const unsigned char mul_mat_vec_q4_k_f32_f32_data[];
extern const uint64_t mul_mat_vec_mxfp4_f32_f32_len;
extern const unsigned char mul_mat_vec_mxfp4_f32_f32_data[];
extern const uint64_t dequant_q6_k_len;
extern const unsigned char dequant_q6_k_data[];
extern const uint64_t mul_mat_vec_iq1_s_f32_f32_len;
extern const unsigned char mul_mat_vec_iq1_s_f32_f32_data[];
extern const uint64_t get_rows_q4_0_len;
extern const unsigned char get_rows_q4_0_data[];
extern const uint64_t matmul_iq2_xs_f32_len;
extern const unsigned char matmul_iq2_xs_f32_data[];

// Defined in chromium/ggml-vulkan-moltbrowser.cpp, declared by the
// MoltBrowser block of ggml-vulkan.cpp.
std::vector<uint32_t> ggml_vk_moltbrowser_unpack_shader(const void* data,
                                                        size_t size);
bool ggml_vk_moltbrowser_op_types_built(const ggml_tensor* op);

namespace {

using ggml_vk_moltbrowser::DecodeDescriptor;
using ggml_vk_moltbrowser::EncodeDescriptor;
using ggml_vk_moltbrowser::kDescriptorSize;
using ggml_vk_moltbrowser::ShaderDescriptor;

constexpr uint32_t kSpirvMagicWord = 0x07230203;

TEST(VulkanShaderPackTest, DescriptorRoundTrips) {
  ShaderDescriptor d;
  d.family = 154;
  d.block = 7;
  d.offset = 0x12345678 & 0xfffc;
  d.size = 4096;
  d.checksum = 0xdeadbeef;
  unsigned char bytes[kDescriptorSize];
  EncodeDescriptor(d, bytes);
  EXPECT_EQ(0, std::memcmp(bytes, "MVKZ", 4));
  ShaderDescriptor back;
  ASSERT_TRUE(DecodeDescriptor(bytes, sizeof(bytes), &back));
  EXPECT_EQ(d.family, back.family);
  EXPECT_EQ(d.block, back.block);
  EXPECT_EQ(d.offset, back.offset);
  EXPECT_EQ(d.size, back.size);
  EXPECT_EQ(d.checksum, back.checksum);
  // Little-endian on every host.
  EXPECT_EQ(154, bytes[4]);
  EXPECT_EQ(0, bytes[5]);
}

TEST(VulkanShaderPackTest, DescriptorRefusesWhatIsNotOne) {
  ShaderDescriptor d;
  d.size = 8;
  unsigned char bytes[kDescriptorSize];
  EncodeDescriptor(d, bytes);
  ShaderDescriptor out;
  EXPECT_TRUE(DecodeDescriptor(bytes, sizeof(bytes), &out));
  EXPECT_FALSE(DecodeDescriptor(nullptr, sizeof(bytes), &out));
  EXPECT_FALSE(DecodeDescriptor(bytes, sizeof(bytes) - 1, &out));
  EXPECT_FALSE(DecodeDescriptor(bytes, 0, &out));

  // Raw SPIR-V is never taken for a descriptor.
  unsigned char spirv[kDescriptorSize] = {};
  std::memcpy(spirv, ggml_vk_moltbrowser::kSpirvMagic, 4);
  EXPECT_FALSE(DecodeDescriptor(spirv, sizeof(spirv), &out));

  auto with = [&](uint32_t offset, uint32_t size) {
    ShaderDescriptor x;
    x.offset = offset;
    x.size = size;
    unsigned char b[kDescriptorSize];
    EncodeDescriptor(x, b);
    ShaderDescriptor o;
    return DecodeDescriptor(b, sizeof(b), &o);
  };
  EXPECT_FALSE(with(0, 0));
  EXPECT_FALSE(with(0, 6));
  EXPECT_FALSE(with(0, ggml_vk_moltbrowser::kMaxBlockRawSize + 4));
  EXPECT_TRUE(with(0, ggml_vk_moltbrowser::kMaxBlockRawSize));
  EXPECT_FALSE(with(4, ggml_vk_moltbrowser::kMaxBlockRawSize));
  EXPECT_FALSE(with(UINT32_MAX - 3, 8));  // offset + size would wrap
}

TEST(VulkanShaderPackTest, ChecksumIsFnv1a) {
  EXPECT_EQ(2166136261u, ggml_vk_moltbrowser::ShaderChecksum(nullptr, 0));
  const unsigned char a = 'a';
  EXPECT_EQ(0xe40c292cu, ggml_vk_moltbrowser::ShaderChecksum(&a, 1));
}

TEST(VulkanShaderPackTest, EveryBlockInflates) {
  ASSERT_GT(ggml_vk_moltbrowser_shader_family_count, 100u);
  size_t blocks = 0;
  size_t raw_total = 0;
  for (uint32_t f = 0; f < ggml_vk_moltbrowser_shader_family_count; ++f) {
    const auto* family = ggml_vk_moltbrowser_shader_families[f];
    ASSERT_TRUE(family);
    ASSERT_TRUE(family->name);
    EXPECT_EQ(!family->blocks, family->block_count == 0) << family->name;
    for (uint32_t b = 0; b < family->block_count; ++b) {
      const auto& block = family->blocks[b];
      ASSERT_LE(block.raw_size, ggml_vk_moltbrowser::kMaxBlockRawSize);
      std::vector<uint8_t> raw(block.raw_size);
      size_t size = raw.size();
      ASSERT_EQ(BROTLI_DECODER_RESULT_SUCCESS,
                BrotliDecoderDecompress(block.size, block.data, &size,
                                        raw.data()))
          << family->name << " block " << b;
      EXPECT_EQ(block.raw_size, size);
      ++blocks;
      raw_total += size;
    }
  }
  EXPECT_GT(blocks, 0u);
  EXPECT_GT(raw_total, 1000000u);
}

void ExpectSpirv(const unsigned char* data, uint64_t len, const char* name) {
  SCOPED_TRACE(name);
  ASSERT_EQ(kDescriptorSize, len);
  const std::vector<uint32_t> spirv =
      ggml_vk_moltbrowser_unpack_shader(data, len);
  ASSERT_GE(spirv.size(), 5u);
  EXPECT_EQ(kSpirvMagicWord, spirv[0]);
  // The same variant inflates to the same words every time.
  EXPECT_EQ(spirv, ggml_vk_moltbrowser_unpack_shader(data, len));
}

TEST(VulkanShaderPackTest, KeptVariantsInflateToSpirv) {
  ExpectSpirv(matmul_f32_f32_data, matmul_f32_f32_len, "matmul_f32_f32");
  ExpectSpirv(mul_mat_vec_q4_k_f32_f32_data, mul_mat_vec_q4_k_f32_f32_len,
              "mul_mat_vec_q4_k_f32_f32");
  ExpectSpirv(mul_mat_vec_mxfp4_f32_f32_data, mul_mat_vec_mxfp4_f32_f32_len,
              "mul_mat_vec_mxfp4_f32_f32");
  ExpectSpirv(dequant_q6_k_data, dequant_q6_k_len, "dequant_q6_k");
  EXPECT_NE(ggml_vk_moltbrowser_unpack_shader(matmul_f32_f32_data,
                                              matmul_f32_f32_len),
            ggml_vk_moltbrowser_unpack_shader(dequant_q6_k_data,
                                              dequant_q6_k_len));
}

TEST(VulkanShaderPackTest, DroppedVariantsAreLeftOutAndRefused) {
  EXPECT_EQ(0u, mul_mat_vec_iq1_s_f32_f32_len);
  EXPECT_EQ(0u, get_rows_q4_0_len);
  EXPECT_EQ(0u, matmul_iq2_xs_f32_len);
  EXPECT_THROW(ggml_vk_moltbrowser_unpack_shader(
                   mul_mat_vec_iq1_s_f32_f32_data,
                   mul_mat_vec_iq1_s_f32_f32_len),
               std::runtime_error);
  // Neither raw SPIR-V nor a damaged descriptor is inflated.
  unsigned char bytes[kDescriptorSize];
  std::memcpy(bytes, matmul_f32_f32_data, kDescriptorSize);
  bytes[kDescriptorSize - 1] ^= 1;  // the checksum
  EXPECT_THROW(ggml_vk_moltbrowser_unpack_shader(bytes, sizeof(bytes)),
               std::runtime_error);
  std::memcpy(bytes, matmul_f32_f32_data, kDescriptorSize);
  bytes[4] = 0xff;  // a family past the table
  bytes[5] = 0xff;
  EXPECT_THROW(ggml_vk_moltbrowser_unpack_shader(bytes, sizeof(bytes)),
               std::runtime_error);
  std::memcpy(bytes, ggml_vk_moltbrowser::kSpirvMagic, 4);
  EXPECT_THROW(ggml_vk_moltbrowser_unpack_shader(bytes, sizeof(bytes)),
               std::runtime_error);
}

TEST(VulkanShaderPackTest, DroppedTypesAreGgmlTypes) {
  std::set<std::string> names;
  for (int t = 0; t < GGML_TYPE_COUNT; ++t) {
    const char* name = ggml_type_name(static_cast<ggml_type>(t));
    if (!name) continue;
    std::string lower(name);
    for (char& c : lower) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    names.insert(lower);
  }
  size_t n = 0;
  for (const char* const* d = ggml_vk_moltbrowser_dropped_types; *d; ++d) {
    EXPECT_TRUE(names.count(*d)) << *d;
    ++n;
  }
  EXPECT_GT(n, 0u);
}

class VulkanShaderPackOpTest : public testing::Test {
 protected:
  void SetUp() override {
    ggml_init_params params = {};
    params.mem_size = 1 << 20;
    params.no_alloc = true;
    ctx_ = ggml_init(params);
    ASSERT_TRUE(ctx_);
  }
  void TearDown() override { ggml_free(ctx_); }

  ggml_tensor* Weight(ggml_type type) {
    return ggml_new_tensor_2d(ctx_, type, 256, 8);
  }
  ggml_tensor* Activations() {
    return ggml_new_tensor_2d(ctx_, GGML_TYPE_F32, 256, 4);
  }

  ggml_context* ctx_ = nullptr;
};

TEST_F(VulkanShaderPackOpTest, OpsOnKeptTypesMayRunOnTheGpu) {
  for (ggml_type t : {GGML_TYPE_F32, GGML_TYPE_F16, GGML_TYPE_BF16,
                      GGML_TYPE_Q8_0, GGML_TYPE_Q5_0, GGML_TYPE_Q5_1,
                      GGML_TYPE_Q4_K, GGML_TYPE_Q5_K, GGML_TYPE_Q6_K,
                      GGML_TYPE_MXFP4}) {
    SCOPED_TRACE(ggml_type_name(t));
    EXPECT_TRUE(ggml_vk_moltbrowser_op_types_built(
        ggml_mul_mat(ctx_, Weight(t), Activations())));
  }
  EXPECT_TRUE(ggml_vk_moltbrowser_op_types_built(
      ggml_add(ctx_, Activations(), Activations())));
}

TEST_F(VulkanShaderPackOpTest, OpsOnDroppedTypesStayOnTheCpu) {
  for (ggml_type t : {GGML_TYPE_Q4_0, GGML_TYPE_Q4_1, GGML_TYPE_Q2_K,
                      GGML_TYPE_Q3_K, GGML_TYPE_IQ1_S, GGML_TYPE_IQ2_XS,
                      GGML_TYPE_IQ4_NL, GGML_TYPE_IQ4_XS, GGML_TYPE_TQ1_0,
                      GGML_TYPE_NVFP4}) {
    SCOPED_TRACE(ggml_type_name(t));
    // As a matmul's weights, rows to gather, or a copy's destination.
    EXPECT_FALSE(ggml_vk_moltbrowser_op_types_built(
        ggml_mul_mat(ctx_, Weight(t), Activations())));
    ggml_tensor* rows = ggml_new_tensor_1d(ctx_, GGML_TYPE_I32, 2);
    EXPECT_FALSE(
        ggml_vk_moltbrowser_op_types_built(ggml_get_rows(ctx_, Weight(t), rows)));
    EXPECT_FALSE(ggml_vk_moltbrowser_op_types_built(
        ggml_cpy(ctx_, Activations(), ggml_new_tensor_2d(ctx_, t, 256, 4))));
  }
}

}  // namespace
