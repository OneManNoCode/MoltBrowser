// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// ggml_vk_shader_pack: a host tool run by BUILD.gn (GN switch
// molt_llama_vulkan) over the .cpp files llama.cpp's shader generator
// writes, to shrink the SPIR-V the browser binary embeds. The format and why:
// chromium/ggml-vulkan-shader-pack.h, docs/design/SANDBOXED_INFERENCE.md
// section 11.14.
//
//   ggml_vk_shader_pack --in RAW.cpp --out PACKED.cpp --family F.comp
//       --families A.comp,B.comp,... [--drop-type T]...
//     (--families may be repeated; the list is every shader file, in
//     BUILD.gn's order, which gives the family its index.)
//     Rewrites one generator output: every `const uint64_t N_len = S;` /
//     `const unsigned char N_data[S] = {...};` pair becomes a descriptor of
//     the variant's place in the family's brotli blocks (or a zero-length
//     stub for a variant of a dropped weight type, i.e. whose name has
//     `_T_` in it); every other line is copied as it is. Fails on anything
//     it doesn't expect, and checks that every kept variant inflates back to
//     the generator's bytes.
//   ggml_vk_shader_pack --registry OUT.cpp --families A.comp,B.comp,...
//       [--drop-type T]...
//     Writes the table of families, in --families order, and the list of
//     dropped weight types.
//
// Deterministic: the same input gives the same bytes (brotli quality 11).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "third_party/brotli/include/brotli/decode.h"
#include "third_party/brotli/include/brotli/encode.h"
#include "third_party/llama_cpp/chromium/ggml-vulkan-shader-pack.h"

namespace {

using ggml_vk_moltbrowser::kBlockRawSize;
using ggml_vk_moltbrowser::kDescriptorSize;
using ggml_vk_moltbrowser::kMaxBlockRawSize;
using ggml_vk_moltbrowser::ShaderDescriptor;

[[noreturn]] void Fail(const std::string& message) {
  fprintf(stderr, "ggml_vk_shader_pack: %s\n", message.c_str());
  exit(1);
}

bool ReadFile(const std::string& path, std::string* out) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  char buf[1 << 16];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, n);
  const bool ok = !ferror(f);
  fclose(f);
  return ok;
}

void WriteFile(const std::string& path, const std::string& data) {
  FILE* f = fopen(path.c_str(), "wb");
  if (!f || fwrite(data.data(), 1, data.size(), f) != data.size()) {
    Fail("can't write " + path);
  }
  if (fclose(f) != 0) Fail("can't write " + path);
}

std::vector<std::string> SplitLines(const std::string& text) {
  std::vector<std::string> lines;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(std::move(line));
    start = end + 1;
  }
  return lines;
}

std::vector<std::string> SplitComma(const std::string& s) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= s.size()) {
    size_t end = s.find(',', start);
    if (end == std::string::npos) end = s.size();
    if (end > start) out.push_back(s.substr(start, end - start));
    start = end + 1;
  }
  return out;
}

bool IsIdentifier(const std::string& s) {
  if (s.empty()) return false;
  for (char c : s) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_')) {
      return false;
    }
  }
  return !(s[0] >= '0' && s[0] <= '9');
}

// "mul_mm.comp" -> "mul_mm"; fails on anything but [a-z0-9_]+.comp.
std::string FamilyStem(const std::string& comp) {
  const std::string suffix = ".comp";
  if (comp.size() <= suffix.size() ||
      comp.compare(comp.size() - suffix.size(), suffix.size(), suffix) != 0) {
    Fail("not a .comp name: " + comp);
  }
  std::string stem = comp.substr(0, comp.size() - suffix.size());
  for (char c : stem) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
      Fail("unexpected shader file name: " + comp);
    }
  }
  return stem;
}

std::string FamilySymbol(const std::string& comp) {
  return "ggml_vk_moltbrowser_family_" + FamilyStem(comp);
}

// Parses an unsigned decimal that fits 32 bits.
bool ParseU32(const std::string& s, uint32_t* out) {
  if (s.empty() || s.size() > 10) return false;
  uint64_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + static_cast<uint64_t>(c - '0');
  }
  if (v > UINT32_MAX) return false;
  *out = static_cast<uint32_t>(v);
  return true;
}

// One line of the generator's hex dump: "0x3,0x2,...," (12 per line, the
// last line shorter), lower-case hex without leading zeros.
bool ParseHexLine(const std::string& line, std::vector<unsigned char>* out) {
  size_t i = 0;
  while (i < line.size()) {
    if (line.compare(i, 2, "0x") != 0) return false;
    i += 2;
    unsigned v = 0;
    size_t digits = 0;
    while (i < line.size() && line[i] != ',') {
      const char c = line[i];
      unsigned d;
      if (c >= '0' && c <= '9') {
        d = static_cast<unsigned>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        d = static_cast<unsigned>(c - 'a' + 10);
      } else {
        return false;
      }
      v = v * 16 + d;
      if (++digits > 2) return false;
      ++i;
    }
    if (digits == 0 || i >= line.size()) return false;  // needs the comma
    ++i;
    out->push_back(static_cast<unsigned char>(v));
  }
  return true;
}

std::string Compress(const std::vector<unsigned char>& raw) {
  std::string out(BrotliEncoderMaxCompressedSize(raw.size()), '\0');
  size_t size = out.size();
  if (out.empty() ||
      !BrotliEncoderCompress(
          BROTLI_MAX_QUALITY, BROTLI_MAX_WINDOW_BITS, BROTLI_MODE_GENERIC,
          raw.size(), raw.data(), &size,
          reinterpret_cast<uint8_t*>(out.data()))) {
    Fail("brotli failed");
  }
  out.resize(size);
  return out;
}

std::vector<unsigned char> Inflate(const std::string& packed,
                                   size_t raw_size) {
  std::vector<unsigned char> raw(raw_size);
  size_t size = raw.size();
  if (BrotliDecoderDecompress(packed.size(),
                              reinterpret_cast<const uint8_t*>(packed.data()),
                              &size, raw.data()) != BROTLI_DECODER_RESULT_SUCCESS ||
      size != raw_size) {
    Fail("brotli round trip failed");
  }
  return raw;
}

void AppendHex(const std::string& bytes, std::string* out) {
  static const char kHex[] = "0123456789abcdef";
  for (size_t i = 0; i < bytes.size(); ++i) {
    const unsigned char b = static_cast<unsigned char>(bytes[i]);
    out->append("0x");
    out->push_back(kHex[b >> 4]);
    out->push_back(kHex[b & 15]);
    out->push_back(',');
    if (i % 16 == 15) out->push_back('\n');
  }
  if (bytes.size() % 16 != 0) out->push_back('\n');
}

struct Variant {
  std::string name;
  std::vector<unsigned char> spirv;  // empty when dropped
  bool dropped = false;
  ShaderDescriptor descriptor;
};

int Pack(const std::string& in_path,
         const std::string& out_path,
         const std::string& family,
         const std::vector<std::string>& families,
         const std::vector<std::string>& drop_types) {
  const auto it = std::find(families.begin(), families.end(), family);
  if (it == families.end()) Fail(family + " is not in --families");
  const uint32_t family_index = static_cast<uint32_t>(it - families.begin());
  for (const std::string& t : drop_types) {
    if (!IsIdentifier(t)) Fail("bad --drop-type " + t);
  }

  std::string text;
  if (!ReadFile(in_path, &text)) Fail("can't read " + in_path);
  const std::vector<std::string> lines = SplitLines(text);

  // Lines to copy, with each variant's place among them.
  std::vector<std::string> kept_lines;
  std::vector<std::pair<size_t, size_t>> variant_at;  // line index, variant
  std::vector<Variant> variants;
  std::map<std::string, size_t> seen;
  const std::string len_prefix = "const uint64_t ";
  const std::string data_prefix = "const unsigned char ";
  for (size_t i = 0; i < lines.size(); ++i) {
    const std::string& line = lines[i];
    // "const uint64_t <name>_len = <size>;" followed by the data array.
    if (line.rfind(len_prefix, 0) == 0 &&
        line.find('[') == std::string::npos) {
      const size_t eq = line.find(" = ");
      if (eq == std::string::npos || line.back() != ';') {
        Fail("unexpected line: " + line);
      }
      std::string lhs = line.substr(len_prefix.size(), eq - len_prefix.size());
      if (lhs.size() <= 4 || lhs.compare(lhs.size() - 4, 4, "_len") != 0) {
        Fail("unexpected line: " + line);
      }
      const std::string name = lhs.substr(0, lhs.size() - 4);
      uint32_t size = 0;
      if (!IsIdentifier(name) ||
          !ParseU32(line.substr(eq + 3, line.size() - eq - 4), &size) ||
          size == 0 || size % 4 != 0 || size > kMaxBlockRawSize) {
        Fail("unexpected variant: " + line);
      }
      const std::string header = data_prefix + name + "_data[" +
                                 std::to_string(size) + "] = {";
      if (i + 1 >= lines.size() || lines[i + 1] != header) {
        Fail("expected the data of " + name);
      }
      Variant v;
      v.name = name;
      size_t j = i + 2;
      for (; j < lines.size() && lines[j] != "};"; ++j) {
        if (!ParseHexLine(lines[j], &v.spirv)) {
          Fail("bad data line in " + name + ": " + lines[j]);
        }
      }
      if (j >= lines.size() || v.spirv.size() != size) {
        Fail("data of " + name + " doesn't match its size");
      }
      if (!std::equal(std::begin(ggml_vk_moltbrowser::kSpirvMagic),
                      std::end(ggml_vk_moltbrowser::kSpirvMagic),
                      v.spirv.begin())) {
        Fail(name + " isn't SPIR-V");
      }
      if (!seen.emplace(name, variants.size()).second) {
        Fail("duplicate variant " + name);
      }
      const std::string padded = "_" + name + "_";
      for (const std::string& t : drop_types) {
        if (padded.find("_" + t + "_") != std::string::npos) {
          v.dropped = true;
          v.spirv.clear();
        }
      }
      variant_at.emplace_back(kept_lines.size(), variants.size());
      kept_lines.emplace_back();  // the variant's place
      variants.push_back(std::move(v));
      i = j;
      continue;
    }
    if (line.rfind(data_prefix, 0) == 0) Fail("unexpected line: " + line);
    kept_lines.push_back(line);
  }

  // Lay the kept variants out in blocks, identical ones sharing bytes.
  std::vector<std::vector<unsigned char>> blocks;
  std::map<std::vector<unsigned char>, ShaderDescriptor> placed;
  for (Variant& v : variants) {
    if (v.dropped) continue;
    auto found = placed.find(v.spirv);
    if (found != placed.end()) {
      v.descriptor = found->second;
      continue;
    }
    if (blocks.empty() ||
        (!blocks.back().empty() &&
         blocks.back().size() + v.spirv.size() > kBlockRawSize)) {
      blocks.emplace_back();
    }
    ShaderDescriptor d;
    d.family = family_index;
    d.block = static_cast<uint32_t>(blocks.size() - 1);
    d.offset = static_cast<uint32_t>(blocks.back().size());
    d.size = static_cast<uint32_t>(v.spirv.size());
    d.checksum = ggml_vk_moltbrowser::ShaderChecksum(v.spirv.data(),
                                                     v.spirv.size());
    blocks.back().insert(blocks.back().end(), v.spirv.begin(), v.spirv.end());
    if (blocks.back().size() > kMaxBlockRawSize) Fail("block too large");
    v.descriptor = d;
    placed.emplace(v.spirv, d);
  }

  std::vector<std::string> packed;
  size_t raw_total = 0;
  size_t packed_total = 0;
  for (const std::vector<unsigned char>& block : blocks) {
    packed.push_back(Compress(block));
    raw_total += block.size();
    packed_total += packed.back().size();
    if (Inflate(packed.back(), block.size()) != block) {
      Fail("brotli round trip differs");
    }
  }
  // Every kept variant must come back from its descriptor as it went in.
  for (const Variant& v : variants) {
    if (v.dropped) continue;
    unsigned char bytes[kDescriptorSize];
    ggml_vk_moltbrowser::EncodeDescriptor(v.descriptor, bytes);
    ShaderDescriptor d;
    if (!ggml_vk_moltbrowser::DecodeDescriptor(bytes, sizeof(bytes), &d) ||
        d.family != family_index || d.block >= blocks.size() ||
        d.offset + d.size > blocks[d.block].size() ||
        !std::equal(v.spirv.begin(), v.spirv.end(),
                    blocks[d.block].begin() + d.offset) ||
        ggml_vk_moltbrowser::ShaderChecksum(
            blocks[d.block].data() + d.offset, d.size) != d.checksum) {
      Fail("descriptor of " + v.name + " doesn't round-trip");
    }
  }

  const std::string symbol = FamilySymbol(family);
  std::string out;
  out += "// Packed by ggml_vk_shader_pack (MoltBrowser, GN switch\n";
  out += "// molt_llama_vulkan) from vulkan-shaders-gen's output for " +
         family + ":\n";
  out += "// " + std::to_string(variants.size()) + " variants, " +
         std::to_string(raw_total) + " bytes of distinct SPIR-V kept in " +
         std::to_string(blocks.size()) + " brotli blocks of " +
         std::to_string(packed_total) + " bytes. Do not edit.\n";
  out += "// chromium/ggml-vulkan-shader-pack.h has the format.\n";
  bool included = false;
  // The generator's own lines, in order, with the variants replaced.
  size_t next = 0;
  for (size_t i = 0; i < kept_lines.size(); ++i) {
    if (next < variant_at.size() && variant_at[next].first == i) {
      const Variant& v = variants[variant_at[next].second];
      ++next;
      if (v.dropped) {
        out += "const uint64_t " + v.name + "_len = 0;\n";
        out += "const unsigned char " + v.name + "_data[1] = {0};\n";
        continue;
      }
      unsigned char bytes[kDescriptorSize];
      ggml_vk_moltbrowser::EncodeDescriptor(v.descriptor, bytes);
      out += "const uint64_t " + v.name + "_len = " +
             std::to_string(kDescriptorSize) + ";\n";
      out += "const unsigned char " + v.name + "_data[" +
             std::to_string(kDescriptorSize) + "] = {\n";
      AppendHex(std::string(reinterpret_cast<const char*>(bytes),
                            sizeof(bytes)),
                &out);
      out += "};\n";
      continue;
    }
    out += kept_lines[i] + "\n";
    // The blocks go right after the generator's #include of its header.
    if (!included && kept_lines[i].rfind("#include ", 0) == 0) {
      included = true;
      out += "#include \"third_party/llama_cpp/chromium/"
             "ggml-vulkan-shader-pack.h\"\n\n";
      out += "namespace {\n";
      for (size_t b = 0; b < packed.size(); ++b) {
        out += "const unsigned char kBlock" + std::to_string(b) + "[" +
               std::to_string(packed[b].size()) + "] = {\n";
        AppendHex(packed[b], &out);
        out += "};\n";
      }
      if (!packed.empty()) {
        out += "const ggml_vk_moltbrowser::PackedShaderBlock kBlocks[] = {\n";
        for (size_t b = 0; b < packed.size(); ++b) {
          out += "    {kBlock" + std::to_string(b) + ", " +
                 std::to_string(packed[b].size()) + ", " +
                 std::to_string(blocks[b].size()) + "},\n";
        }
        out += "};\n";
      }
      out += "}  // namespace\n\n";
      out += "extern const ggml_vk_moltbrowser::PackedShaderFamily " + symbol +
             ";\n";
      out += "const ggml_vk_moltbrowser::PackedShaderFamily " + symbol +
             " = {\"" + family + "\", " +
             (packed.empty() ? std::string("nullptr, 0")
                             : "kBlocks, " + std::to_string(packed.size())) +
             "};\n";
    }
  }
  if (!included) Fail("no #include in " + in_path);
  if (next != variant_at.size()) Fail("internal error placing variants");
  WriteFile(out_path, out);
  return 0;
}

int Registry(const std::string& out_path,
             const std::vector<std::string>& families,
             const std::vector<std::string>& drop_types) {
  if (families.empty()) Fail("no --families");
  for (const std::string& t : drop_types) {
    if (!IsIdentifier(t)) Fail("bad --drop-type " + t);
  }
  std::string out;
  out += "// Generated by ggml_vk_shader_pack --registry (MoltBrowser, GN "
         "switch\n// molt_llama_vulkan). Do not edit.\n";
  out += "#include \"third_party/llama_cpp/chromium/"
         "ggml-vulkan-shader-pack.h\"\n\n";
  for (const std::string& f : families) {
    out += "extern const ggml_vk_moltbrowser::PackedShaderFamily " +
           FamilySymbol(f) + ";\n";
  }
  out += "\nconst ggml_vk_moltbrowser::PackedShaderFamily* const\n"
         "    ggml_vk_moltbrowser_shader_families[] = {\n";
  for (const std::string& f : families) {
    out += "        &" + FamilySymbol(f) + ",\n";
  }
  out += "};\n";
  out += "const uint32_t ggml_vk_moltbrowser_shader_family_count = " +
         std::to_string(families.size()) + ";\n\n";
  // The weight types whose variants were left out, as ggml_type_name()
  // spells them in lower case.
  out += "const char* const ggml_vk_moltbrowser_dropped_types[] = {\n";
  for (const std::string& t : drop_types) out += "    \"" + t + "\",\n";
  out += "    nullptr,\n};\n";
  WriteFile(out_path, out);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::string in, out, family, registry;
  std::vector<std::string> families, drop_types;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= argc) Fail("missing value for " + arg);
      return argv[++i];
    };
    if (arg == "--in") {
      in = value();
    } else if (arg == "--out") {
      out = value();
    } else if (arg == "--family") {
      family = value();
    } else if (arg == "--families") {
      for (std::string& f : SplitComma(value())) families.push_back(f);
    } else if (arg == "--drop-type") {
      drop_types.push_back(value());
    } else if (arg == "--registry") {
      registry = value();
    } else {
      Fail("unknown argument " + arg);
    }
  }
  for (const std::string& f : families) FamilyStem(f);
  if (!registry.empty()) return Registry(registry, families, drop_types);
  if (in.empty() || out.empty() || family.empty()) {
    Fail("usage: --in RAW --out PACKED --family F --families LIST "
         "[--drop-type T]... | --registry OUT --families LIST");
  }
  return Pack(in, out, family, families, drop_types);
}
