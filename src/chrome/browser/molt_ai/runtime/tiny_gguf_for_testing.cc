// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/runtime/tiny_gguf_for_testing.h"

#include <cstring>
#include <string_view>
#include <vector>

namespace molt_ai {

namespace {

// GGUF version 3 (ggml/docs/gguf.md, ggml/src/gguf.cpp).
constexpr uint32_t kVersion = 3;
constexpr uint32_t kAlignment = 32;  // general.alignment's default
enum ValueType : uint32_t {
  kUint32 = 4,
  kInt32 = 5,
  kFloat32 = 6,
  kBool = 7,
  kString = 8,
  kArray = 9,
};
constexpr uint32_t kTensorF32 = 0;  // GGML_TYPE_F32

// llama_vocab's token types (llama-vocab.h).
enum TokenType : int32_t {
  kNormal = 1,
  kUnknown = 2,
  kControl = 3,
  kByte = 6,
};

constexpr uint32_t kEmbd = 64;
constexpr uint32_t kHeads = 4;
constexpr uint32_t kHeadsKv = 2;
constexpr uint32_t kFf = 128;
constexpr uint32_t kHeadDim = kEmbd / kHeads;

class Buffer {
 public:
  void U32(uint32_t v) { Raw(&v, sizeof(v)); }
  void I32(int32_t v) { Raw(&v, sizeof(v)); }
  void U64(uint64_t v) { Raw(&v, sizeof(v)); }
  void F32(float v) { Raw(&v, sizeof(v)); }
  void Str(std::string_view s) {
    U64(s.size());
    bytes_.append(s.data(), s.size());
  }
  void Raw(const void* p, size_t n) {
    bytes_.append(static_cast<const char*>(p), n);
  }
  void PadTo(size_t alignment) {
    bytes_.append((alignment - bytes_.size() % alignment) % alignment, '\0');
  }
  size_t size() const { return bytes_.size(); }
  std::string& bytes() { return bytes_; }

 private:
  std::string bytes_;
};

class Writer {
 public:
  void KvU32(std::string_view key, uint32_t v) {
    Key(key, kUint32);
    kv_.U32(v);
  }
  void KvF32(std::string_view key, float v) {
    Key(key, kFloat32);
    kv_.F32(v);
  }
  void KvBool(std::string_view key, bool v) {
    Key(key, kBool);
    const uint8_t b = v ? 1 : 0;
    kv_.Raw(&b, 1);
  }
  void KvStr(std::string_view key, std::string_view v) {
    Key(key, kString);
    kv_.Str(v);
  }
  void KvStrings(std::string_view key, const std::vector<std::string>& v) {
    Key(key, kArray);
    kv_.U32(kString);
    kv_.U64(v.size());
    for (const std::string& s : v) {
      kv_.Str(s);
    }
  }
  void KvF32s(std::string_view key, const std::vector<float>& v) {
    Key(key, kArray);
    kv_.U32(kFloat32);
    kv_.U64(v.size());
    for (float f : v) {
      kv_.F32(f);
    }
  }
  void KvI32s(std::string_view key, const std::vector<int32_t>& v) {
    Key(key, kArray);
    kv_.U32(kInt32);
    kv_.U64(v.size());
    for (int32_t i : v) {
      kv_.I32(i);
    }
  }

  // An F32 tensor of |ne0| x |ne1| (ne1 0: one dimension): ones, or small
  // deterministic noise.
  void Tensor(std::string_view name, uint32_t ne0, uint32_t ne1, bool ones) {
    infos_.Str(name);
    infos_.U32(ne1 ? 2 : 1);
    infos_.U64(ne0);
    if (ne1) {
      infos_.U64(ne1);
    }
    infos_.U32(kTensorF32);
    data_.PadTo(kAlignment);
    infos_.U64(data_.size());
    const uint64_t n = uint64_t{ne0} * (ne1 ? ne1 : 1);
    for (uint64_t i = 0; i < n; ++i) {
      data_.F32(ones ? 1.0f : Noise());
    }
    ++tensors_;
  }

  std::string Finish() {
    Buffer out;
    out.Raw("GGUF", 4);
    out.U32(kVersion);
    out.U64(tensors_);
    out.U64(kvs_);
    out.Raw(kv_.bytes().data(), kv_.size());
    out.Raw(infos_.bytes().data(), infos_.size());
    out.PadTo(kAlignment);
    data_.PadTo(kAlignment);
    out.Raw(data_.bytes().data(), data_.size());
    return std::move(out.bytes());
  }

 private:
  void Key(std::string_view key, uint32_t type) {
    kv_.Str(key);
    kv_.U32(type);
    ++kvs_;
  }

  // In [-0.05, 0.05), from a fixed LCG.
  float Noise() {
    seed_ = seed_ * 6364136223846793005ull + 1442695040888963407ull;
    const double unit = static_cast<double>(seed_ >> 11) / 9007199254740992.0;
    return static_cast<float>((unit - 0.5) * 0.1);
  }

  Buffer kv_;
  Buffer infos_;
  Buffer data_;
  uint64_t kvs_ = 0;
  uint64_t tensors_ = 0;
  uint64_t seed_ = 1234;
};

}  // namespace

std::string TinyLlamaGgufForTesting() {
  std::vector<std::string> tokens;
  std::vector<float> scores;
  std::vector<int32_t> types;
  auto add = [&](std::string token, int32_t type, float score) {
    tokens.push_back(std::move(token));
    types.push_back(type);
    scores.push_back(score);
  };
  add("<unk>", kUnknown, 0);
  add("<s>", kControl, 0);
  add("</s>", kControl, 0);
  for (int b = 0; b < 256; ++b) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    add(std::string("<0x") + kHex[b >> 4] + kHex[b & 15] + ">", kByte, 0);
  }
  // "\xE2\x96\x81" is SentencePiece's word start (U+2581).
  int i = 0;
  for (const char* piece :
       {"\xE2\x96\x81the",
        "\xE2\x96\x81"
        "a",
        "\xE2\x96\x81of",
        "\xE2\x96\x81"
        "and",
        "\xE2\x96\x81to", "\xE2\x96\x81is", "\xE2\x96\x81page",
        "\xE2\x96\x81hello", "ing", "ed"}) {
    add(piece, kNormal, -static_cast<float>(i++));
  }
  add("<|im_start|>", kControl, 0);
  add("<|im_end|>", kControl, 0);

  Writer w;
  w.KvStr("general.architecture", "llama");
  w.KvStr("general.name", "MoltBrowser tiny test model");
  w.KvU32("llama.context_length", kTinyGgufContextLength);
  w.KvU32("llama.embedding_length", kEmbd);
  w.KvU32("llama.block_count", 1);
  w.KvU32("llama.feed_forward_length", kFf);
  w.KvU32("llama.attention.head_count", kHeads);
  w.KvU32("llama.attention.head_count_kv", kHeadsKv);
  w.KvF32("llama.attention.layer_norm_rms_epsilon", 1e-5f);
  w.KvU32("llama.rope.dimension_count", kHeadDim);
  w.KvU32("llama.vocab_size", static_cast<uint32_t>(tokens.size()));
  w.KvStr("tokenizer.ggml.model", "llama");
  w.KvStrings("tokenizer.ggml.tokens", tokens);
  w.KvF32s("tokenizer.ggml.scores", scores);
  w.KvI32s("tokenizer.ggml.token_type", types);
  w.KvU32("tokenizer.ggml.bos_token_id", 1);
  w.KvU32("tokenizer.ggml.eos_token_id", 2);
  w.KvU32("tokenizer.ggml.eot_token_id",
          static_cast<uint32_t>(tokens.size() - 1));
  w.KvBool("tokenizer.ggml.add_bos_token", true);
  w.KvStr("tokenizer.chat_template",
          "{% for m in messages %}<|im_start|>{{ m['role'] }}\n"
          "{{ m['content'] }}<|im_end|>\n{% endfor %}"
          "{% if add_generation_prompt %}<|im_start|>assistant\n{% endif %}");

  const uint32_t vocab = static_cast<uint32_t>(tokens.size());
  w.Tensor("token_embd.weight", kEmbd, vocab, false);
  w.Tensor("output_norm.weight", kEmbd, 0, true);
  w.Tensor("output.weight", kEmbd, vocab, false);
  w.Tensor("blk.0.attn_norm.weight", kEmbd, 0, true);
  w.Tensor("blk.0.attn_q.weight", kEmbd, kEmbd, false);
  w.Tensor("blk.0.attn_k.weight", kEmbd, kHeadDim * kHeadsKv, false);
  w.Tensor("blk.0.attn_v.weight", kEmbd, kHeadDim * kHeadsKv, false);
  w.Tensor("blk.0.attn_output.weight", kEmbd, kEmbd, false);
  w.Tensor("blk.0.ffn_norm.weight", kEmbd, 0, true);
  w.Tensor("blk.0.ffn_gate.weight", kEmbd, kFf, false);
  w.Tensor("blk.0.ffn_down.weight", kFf, kEmbd, false);
  w.Tensor("blk.0.ffn_up.weight", kEmbd, kFf, false);
  return w.Finish();
}

}  // namespace molt_ai
