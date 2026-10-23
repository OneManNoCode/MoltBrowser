// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The x86-64 CPU kernel levels //third_party/llama_cpp builds (x86-64-v2,
// AVX, AVX2; docs/design/SANDBOXED_INFERENCE.md section 12) compute the same
// results. For every quantized type, each level this CPU can run gives the
// x86-64-v2 level's dot product on the same inputs, and its own activation
// quantizer stays within quantization error of the float result. Run when
// updating the vendored llama.cpp: a kernel a level gets wrong shows here,
// not as slightly odd text on someone's old laptop.

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <vector>

#include "chrome/services/molt_ai/inference/engine/cpu_kernels.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/llama_cpp/chromium/ggml-cpu-x86-dispatch.h"
#include "third_party/llama_cpp/ggml/include/ggml-cpu.h"
#include "third_party/llama_cpp/ggml/include/ggml.h"

namespace molt_ai {
namespace {

// A multiple of every block size (QK_K = 256).
constexpr int kN = 4096;

// Every type whose dot product the levels implement (the vec_dot entries
// of chromium/ggml-cpu-x86-variants.h). Their activations are Q8_0, Q8_1 or
// Q8_K, the three quantizers the levels implement.
constexpr ggml_type kTypes[] = {
    GGML_TYPE_Q1_0,    GGML_TYPE_Q2_0,   GGML_TYPE_Q4_0,   GGML_TYPE_Q4_1,
    GGML_TYPE_Q5_0,    GGML_TYPE_Q5_1,   GGML_TYPE_Q8_0,   GGML_TYPE_MXFP4,
    GGML_TYPE_NVFP4,   GGML_TYPE_Q2_K,   GGML_TYPE_Q3_K,   GGML_TYPE_Q4_K,
    GGML_TYPE_Q5_K,    GGML_TYPE_Q6_K,   GGML_TYPE_TQ1_0,  GGML_TYPE_TQ2_0,
    GGML_TYPE_IQ2_XXS, GGML_TYPE_IQ2_XS, GGML_TYPE_IQ2_S,  GGML_TYPE_IQ3_XXS,
    GGML_TYPE_IQ3_S,   GGML_TYPE_IQ1_S,  GGML_TYPE_IQ1_M,  GGML_TYPE_IQ4_NL,
    GGML_TYPE_IQ4_XS,
};

molt_llama::X86CpuKernels ToDispatch(X86CpuKernels kernels) {
  switch (kernels) {
    case X86CpuKernels::kX86_64_V2:
      return molt_llama::X86CpuKernels::kX86_64_V2;
    case X86CpuKernels::kAvx:
      return molt_llama::X86CpuKernels::kAvx;
    case X86CpuKernels::kAvx2:
      return molt_llama::X86CpuKernels::kAvx2;
  }
  return molt_llama::X86CpuKernels::kX86_64_V2;
}

// Roughly normal values (a sum of four uniforms), deterministic.
std::vector<float> RandomFloats(uint32_t seed) {
  std::vector<float> v(kN);
  uint32_t x = seed;
  for (float& f : v) {
    float sum = 0;
    for (int i = 0; i < 4; ++i) {
      x ^= x << 13;
      x ^= x >> 17;
      x ^= x << 5;
      sum += static_cast<float>(x & 0xffffff) / 16777216.0f - 0.5f;
    }
    f = sum;
  }
  return v;
}

// The weights of |type|, kN values. Most types are quantized by ggml's
// reference code. The IQ types below would need ggml_quantize_init()'s
// search tables, seconds each to build, so they get random blocks instead:
// any bits are a valid block (grid indices, signs, sub-block scales), given
// a sane fp16 block scale. Layouts from ggml/src/ggml-common.h: the first
// six begin with `ggml_half d`; block_iq1_m keeps d's four nibbles in the
// top of its four 16-bit scale words, its last 8 bytes.
std::vector<uint8_t> Weights(ggml_type type, const std::vector<float>& values) {
  std::vector<uint8_t> q(ggml_row_size(type, kN));
  const size_t block = ggml_type_size(type);
  const uint16_t d = ggml_fp32_to_fp16(0.01f);
  auto random_blocks = [&]() {
    uint32_t x = 0x9e3779b9u + static_cast<uint32_t>(type);
    for (uint8_t& byte : q) {
      x ^= x << 13;
      x ^= x >> 17;
      x ^= x << 5;
      byte = static_cast<uint8_t>(x >> 24);
    }
  };
  switch (type) {
    case GGML_TYPE_IQ2_XXS:
    case GGML_TYPE_IQ2_XS:
    case GGML_TYPE_IQ2_S:
    case GGML_TYPE_IQ3_XXS:
    case GGML_TYPE_IQ3_S:
    case GGML_TYPE_IQ1_S:
      random_blocks();
      for (size_t b = 0; b < q.size(); b += block) {
        q[b] = static_cast<uint8_t>(d & 0xff);
        q[b + 1] = static_cast<uint8_t>(d >> 8);
      }
      return q;
    case GGML_TYPE_IQ1_M:
      random_blocks();
      for (size_t b = 0; b < q.size(); b += block) {
        for (size_t i = 0; i < 4; ++i) {
          // The high byte of 16-bit scale word i, little-endian.
          uint8_t& hi = q[b + block - 8 + 2 * i + 1];
          hi = static_cast<uint8_t>((hi & 0x0f) |
                                    (((d >> (4 * i)) & 0xf) << 4));
        }
      }
      return q;
    default:
      ggml_quantize_chunk(type, values.data(), q.data(), 0, 1, kN, nullptr);
      return q;
  }
}

class X86CpuKernelsTest : public testing::Test {
 protected:
  void SetUp() override {
    host_ = HostX86CpuKernels();
    if (!host_) GTEST_SKIP() << "this CPU is below x86-64-v2";
    ggml_cpu_init();
    saved_ = molt_llama::CurrentX86CpuKernels();
  }

  void TearDown() override {
    if (host_) molt_llama::UseX86CpuKernels(saved_);
  }

  // The levels this CPU can run, lowest first.
  std::vector<X86CpuKernels> Levels() const {
    std::vector<X86CpuKernels> levels;
    for (X86CpuKernels k : {X86CpuKernels::kX86_64_V2, X86CpuKernels::kAvx,
                            X86CpuKernels::kAvx2}) {
      if (static_cast<int>(k) <= static_cast<int>(*host_)) levels.push_back(k);
    }
    return levels;
  }

  std::optional<X86CpuKernels> host_;
  molt_llama::X86CpuKernels saved_ = molt_llama::X86CpuKernels::kX86_64_V2;
};

TEST_F(X86CpuKernelsTest, UseAndCurrent) {
  for (X86CpuKernels level : Levels()) {
    molt_llama::UseX86CpuKernels(ToDispatch(level));
    EXPECT_EQ(molt_llama::CurrentX86CpuKernels(), ToDispatch(level));
  }
}

TEST_F(X86CpuKernelsTest, DotProductsAgreeAcrossLevels) {
  const std::vector<float> weights = RandomFloats(1);
  const std::vector<float> activations = RandomFloats(2);
  for (ggml_type type : kTypes) {
    SCOPED_TRACE(ggml_type_name(type));
    const ggml_type_traits* traits = ggml_get_type_traits(type);
    const ggml_type_traits_cpu* cpu = ggml_get_type_traits_cpu(type);
    ASSERT_TRUE(traits->to_float);
    ASSERT_TRUE(cpu->vec_dot);
    const ggml_type_traits_cpu* act = ggml_get_type_traits_cpu(cpu->vec_dot_type);
    ASSERT_TRUE(act->from_float);

    // Weights made without any level's code, and what they stand for.
    const std::vector<uint8_t> qw = Weights(type, weights);
    std::vector<float> dequantized(kN);
    traits->to_float(qw.data(), dequantized.data(), kN);
    double exact = 0;
    double scale = 0;
    for (int i = 0; i < kN; ++i) {
      const double p = static_cast<double>(dequantized[i]) * activations[i];
      exact += p;
      scale += std::fabs(p);
    }
    ASSERT_GT(scale, 0);

    // The x86-64-v2 level: activations and the dot product.
    molt_llama::UseX86CpuKernels(molt_llama::X86CpuKernels::kX86_64_V2);
    std::vector<uint8_t> qa_v2(ggml_row_size(cpu->vec_dot_type, kN));
    act->from_float(activations.data(), qa_v2.data(), kN);
    float v2 = 0;
    cpu->vec_dot(kN, &v2, 0, qw.data(), 0, qa_v2.data(), 0, 1);

    for (X86CpuKernels level : Levels()) {
      SCOPED_TRACE(X86CpuKernelsName(level));
      molt_llama::UseX86CpuKernels(ToDispatch(level));
      // Same inputs: the same integer sums, added up in another order.
      // Measured at b11641: at most 1.4e-8 of |scale|.
      float same = 0;
      cpu->vec_dot(kN, &same, 0, qw.data(), 0, qa_v2.data(), 0, 1);
      EXPECT_NEAR(same, v2, 1e-6 * scale);
      // The level's own activations: within their quantization error
      // (measured: at most 5e-4 of |scale|).
      std::vector<uint8_t> qa(qa_v2.size());
      act->from_float(activations.data(), qa.data(), kN);
      float own = 0;
      cpu->vec_dot(kN, &own, 0, qw.data(), 0, qa.data(), 0, 1);
      EXPECT_NEAR(own, exact, 1e-2 * scale);
      EXPECT_NEAR(own, v2, 1e-3 * scale);
    }
  }
}

}  // namespace
}  // namespace molt_ai
